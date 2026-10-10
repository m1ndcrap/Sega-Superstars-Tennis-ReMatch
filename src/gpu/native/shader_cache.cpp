// sst_rematch - native renderer: shader cache

#include "gpu/native/shader_cache.h"

#include <sst_translate.h>

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>

#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

#include <dxcapi.h>
#include <wrl/client.h>

namespace sst::native
{

  using Microsoft::WRL::ComPtr;

  namespace
  {

    // Bump when the translator, the prelude or the compile options change so stale cache files are rebuilt
    constexpr uint32_t kCacheVersion = 1;
    constexpr uint32_t kFileMagic = 0x53535453; // "SSTS"

    struct FileHeader
    {
      uint32_t magic;
      uint32_t version;
      uint32_t spec_mask;
      uint32_t dxil_size;
    };

    uint32_t ReadBE32(const uint8_t *p)
    {
      uint32_t v;
      std::memcpy(&v, p, 4);
      return __builtin_bswap32(v);
    }

    // A signed DXIL container has a non-zero digest (bytes 4..19). dxcompiler.dll signs only when dxil.dll is loadable; D3D12 rejects unsigned DXIL.
    bool IsSigned(const std::vector<uint8_t> &dxil)
    {
      if (dxil.size() < 20)
        return false;
      for (size_t i = 4; i < 20; ++i)
        if (dxil[i])
          return true;
      return false;
    }

  }

  struct Worker
  {
    ComPtr<IDxcCompiler3> compiler;
  };

  bool ShaderCache::Initialize(const rex::ui::d3d12::D3D12Provider &provider,
                               const std::filesystem::path &cache_dir)
  {
    provider_ = &provider;
    cache_dir_ = cache_dir;
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);
    std::filesystem::create_directories(cache_dir_ / "failed", ec);

    // Check DXC is usable before starting workers
    ComPtr<IDxcCompiler3> probe;
    if (FAILED(provider.DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&probe))))
    {
      REXLOG_ERROR(
          "[sst-native] dxcompiler.dll not found: shaders can't be compiled. Copy dxcompiler.dll and "
          "dxil.dll from the Windows SDK (Windows Kits\\10\\bin\\<version>\\x64) next to the exe.");
    }

    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const unsigned count = std::clamp(hw - 2, 1u, 8u);
    for (unsigned i = 0; i < count; ++i)
      workers_.emplace_back([this, i]
                            { WorkerMain(i); });
    REXLOG_INFO("[sst-native] shader cache at {} ({} compile threads)", cache_dir_.string(), count);
    return true;
  }

  void ShaderCache::Shutdown()
  {
    {
      std::lock_guard lock(queue_mutex_);
      stopping_ = true;
      queue_.clear();
    }
    queue_cv_.notify_all();
    for (auto &t : workers_)
      t.join();
    workers_.clear();
    LogStats("at shutdown");
  }

  std::filesystem::path ShaderCache::FilePath(uint64_t hash, uint32_t spec) const
  {
    char name[40];
    std::snprintf(name, sizeof(name), "%016llX_%u.bin", static_cast<unsigned long long>(hash), spec);
    return cache_dir_ / name;
  }

  ShaderEntry *ShaderCache::Register(const uint8_t *container, uint32_t size)
  {
    const uint64_t hash = XXH3_64bits(container, size);
    {
      std::shared_lock lock(entries_mutex_);
      auto it = entries_.find(hash);
      if (it != entries_.end())
        return it->second.get();
    }

    auto entry = std::make_unique<ShaderEntry>();
    entry->hash = hash;
    const uint32_t flags = ReadBE32(container);
    entry->stage = (flags & 1) ? ShaderStage::kVertex : ShaderStage::kPixel;
    entry->memexport = (flags & 0x10) != 0; // 0x102A1111: memory-export vertex shader
    entry->container.assign(container, container + size);
    if (entry->stage == ShaderStage::kVertex)
    {
      sst::xenos::VertexInput inputs[32];
      uint32_t count = 0;
      if (sst::xenos::GetVertexInputs(container, size, inputs, &count, 32, &entry->ucode_offset))
        entry->vertex_inputs.assign(inputs, inputs + count);
    }

    ShaderEntry *raw;
    {
      std::unique_lock lock(entries_mutex_);
      auto [it, fresh] = entries_.try_emplace(hash, std::move(entry));
      raw = it->second.get();
      if (!fresh)
        return raw;
    }
    stats_.registered.fetch_add(1);
    if (raw->memexport)
    {
      stats_.memexport.fetch_add(1);
      REXLOG_INFO("[sst-native] shader {:016X}: memory-export vertex shader, not translated yet",
                  hash);
      return raw;
    }
    GetVariant(raw, 0);
    return raw;
  }

  void ShaderCache::BindObject(uint32_t guest_object, ShaderEntry *entry)
  {
    if (!guest_object || !entry)
      return;
    std::unique_lock lock(entries_mutex_);
    objects_[guest_object] = entry;
  }

  ShaderEntry *ShaderCache::FindObject(uint32_t guest_object) const
  {
    std::shared_lock lock(entries_mutex_);
    auto it = objects_.find(guest_object);
    return it == objects_.end() ? nullptr : it->second;
  }

  const ShaderVariant *ShaderCache::GetVariant(ShaderEntry *entry, uint32_t spec)
  {
    if (!entry || entry->memexport)
      return nullptr;
    if (spec != 0)
    {
      // Until variant 0 is translated we don't know which bits matter.
      if (!entry->translated.load(std::memory_order_acquire))
        return nullptr;
      spec &= entry->spec_mask.load(std::memory_order_relaxed);
    }

    ShaderVariant *variant;
    bool queue = false;
    {
      std::lock_guard lock(entry->variants_mutex);
      auto &slot = entry->variants[spec];
      if (!slot)
      {
        slot = std::make_unique<ShaderVariant>();
        queue = true;
      }
      variant = slot.get();
    }
    if (queue)
    {
      pending_.fetch_add(1);
      {
        std::lock_guard lock(queue_mutex_);
        queue_.push_back({entry, spec, variant});
      }
      queue_cv_.notify_one();
      return nullptr;
    }
    return variant->state.load(std::memory_order_acquire) == ShaderVariant::State::kReady ? variant
                                                                                          : nullptr;
  }

  void ShaderCache::WorkerMain(unsigned index)
  {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    if (auto fn = reinterpret_cast<SetThreadDescriptionFn>(reinterpret_cast<void *>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
    {
      wchar_t name[32];
      swprintf(name, 32, L"sst shader compiler %u", index);
      fn(GetCurrentThread(), name);
    }
#endif
    Worker worker;
    for (;;)
    {
      Job job;
      {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [this]
                       { return stopping_ || !queue_.empty(); });
        if (stopping_)
          return;
        job = queue_.front();
        queue_.pop_front();
      }
      Build(worker, job);
      if (pending_.fetch_sub(1) == 1)
        LogStats("queue drained");
    }
  }

  void ShaderCache::Build(Worker &worker, const Job &job)
  {
    ShaderEntry &entry = *job.entry;
    ShaderVariant &variant = *job.variant;
    const auto path = FilePath(entry.hash, job.spec);

    auto finish = [&](ShaderVariant::State state, uint32_t mask)
    {
      if (job.spec == 0)
      {
        entry.spec_mask.store(mask, std::memory_order_relaxed);
        entry.translated.store(true, std::memory_order_release);
      }
      variant.state.store(state, std::memory_order_release);
    };

    // Disk cache
    if (std::ifstream in{path, std::ios::binary})
    {
      FileHeader header{};
      if (in.read(reinterpret_cast<char *>(&header), sizeof(header)) && header.magic == kFileMagic &&
          header.version == kCacheVersion && header.dxil_size > 0 &&
          header.dxil_size < (16u << 20))
      {
        variant.dxil.resize(header.dxil_size);
        if (in.read(reinterpret_cast<char *>(variant.dxil.data()), header.dxil_size))
        {
          stats_.from_disk.fetch_add(1);
          finish(ShaderVariant::State::kReady, header.spec_mask);
          return;
        }
      }
      variant.dxil.clear();
    }

    // Translate
    const auto start = std::chrono::steady_clock::now();
    auto tr = sst::xenos::Translate(entry.container.data());
    auto fail = [&](const std::string &why, const std::string &source)
    {
      stats_.failed.fetch_add(1);
      REXLOG_WARN("[sst-native] shader {:016X} variant {}: {}", entry.hash, job.spec, why);
      char name[40];
      std::snprintf(name, sizeof(name), "%016llX_%u", static_cast<unsigned long long>(entry.hash),
                    job.spec);
      std::ofstream(cache_dir_ / "failed" / (std::string(name) + ".bin"), std::ios::binary)
          .write(reinterpret_cast<const char *>(entry.container.data()),
                 std::streamsize(entry.container.size()));
      if (!source.empty())
        std::ofstream(cache_dir_ / "failed" / (std::string(name) + ".hlsl")) << source;
      std::ofstream(cache_dir_ / "failed" / (std::string(name) + ".txt")) << why;
      finish(ShaderVariant::State::kFailed, tr.spec_constants_mask);
    };
    if (!tr.ok)
      return fail("translation failed: " + tr.error, {});
    const std::string source = tr.hlsl + sst::xenos::SpecConstantsDefinition(job.spec);

    // Compile to DXIL
    if (!worker.compiler &&
        FAILED(provider_->DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&worker.compiler))))
      return fail("dxcompiler.dll not available", source);

    const wchar_t *args[] = {
        L"-T",
        tr.pixel_shader ? L"ps_6_0" : L"vs_6_0",
        L"-HV",
        L"2021",
        L"-all-resources-bound",
        L"-Wno-ignored-attributes",
        L"-Qstrip_reflect",
        L"-Qstrip_debug",
    };
    DxcBuffer buffer = {source.data(), source.size(), DXC_CP_UTF8};
    ComPtr<IDxcResult> result;
    HRESULT status = E_FAIL;
    if (FAILED(worker.compiler->Compile(&buffer, args, UINT32(std::size(args)), nullptr,
                                        IID_PPV_ARGS(&result))) ||
        FAILED(result->GetStatus(&status)) || FAILED(status))
    {
      std::string errors = "DXC failed";
      ComPtr<IDxcBlobUtf8> err;
      if (result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err), nullptr)) &&
          err && err->GetStringLength())
        errors += ":\n" + std::string(err->GetStringPointer(), err->GetStringLength());
      return fail(errors, source);
    }
    ComPtr<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object ||
        !object->GetBufferSize())
      return fail("DXC returned no object", source);

    const auto *bytes = static_cast<const uint8_t *>(object->GetBufferPointer());
    variant.dxil.assign(bytes, bytes + object->GetBufferSize());
    if (!IsSigned(variant.dxil) && !stats_.unsigned_warned.exchange(true))
      REXLOG_WARN(
          "[sst-native] DXIL is unsigned: dxil.dll was not found next to dxcompiler.dll. D3D12 will "
          "reject these shaders; copy dxil.dll from the Windows SDK next to the exe.");

    stats_.compiled.fetch_add(1);
    stats_.compile_us.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count()));

    // Store
    if (IsSigned(variant.dxil))
    {
      const FileHeader header = {kFileMagic, kCacheVersion, tr.spec_constants_mask,
                                 uint32_t(variant.dxil.size())};
      auto tmp = path;
      tmp += ".tmp";
      {
        std::ofstream out(tmp, std::ios::binary);
        out.write(reinterpret_cast<const char *>(&header), sizeof(header));
        out.write(reinterpret_cast<const char *>(variant.dxil.data()),
                  std::streamsize(variant.dxil.size()));
      }
      std::error_code ec;
      std::filesystem::rename(tmp, path, ec);
    }
    finish(ShaderVariant::State::kReady, tr.spec_constants_mask);
  }

  void ShaderCache::LogStats(const char *when) const
  {
    const uint32_t compiled = stats_.compiled.load();
    const uint64_t us = stats_.compile_us.load();
    REXLOG_INFO(
        "[sst-native] shaders {}: {} unique ({} memexport skipped), {} from disk cache, {} compiled "
        "(avg {:.1f} ms), {} failed, {} pending",
        when, stats_.registered.load(), stats_.memexport.load(), stats_.from_disk.load(), compiled,
        compiled ? double(us) / compiled / 1000.0 : 0.0, stats_.failed.load(), pending_.load());
  }

}