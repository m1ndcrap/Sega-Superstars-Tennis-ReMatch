// sst_rematch - native renderer: shader cache

#include "gpu/native/shader_cache.h"

#include <sst_translate.h>

#include <rex/cvar.h>
#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "gpu/native/dxc.h"

#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#include <compressapi.h>
#endif

REXCVAR_DEFINE_STRING(sst_shader_override, "", "SST",
                      "Native renderer debug: folder (next to the exe, or a full path) of replacement shaders. <HASH>.hlsl "
                      "replaces that shader's translated HLSL (a first line \"// dxc: -Od\" adds DXC arguments); all.args "
                      "adds DXC arguments to every shader. Overridden shaders are compiled at startup / first use, not "
                      "taken from the exe's shader pack");

REXCVAR_DEFINE_BOOL(sst_fast_shaders, false, "SST",
                    "SHADER PRECISION: FAST (VIDEO page): plain IEEE multiplies instead of the Xbox 360 / D3D9 rule "
                    "(0 x anything = 0). Cheaper on weak GPUs. Read at startup (the exe carries a shader pack for each)");

namespace sst::native
{
  namespace
  {
    // Shared with the build-time baker (shader_pack.h): bump it there when the translator, the prelude or the compile options change.
    // ShaderCache::version_ is this or pack::kCacheVersionFast (SHADER PRECISION: FAST)
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

    std::vector<std::wstring> SplitArgs(const std::string &text)
    {
      std::vector<std::wstring> out;
      std::string cur;

      auto flush = [&]
      {
        if (!cur.empty())
          out.emplace_back(cur.begin(), cur.end());

        cur.clear();
      };

      for (char c : text)
      {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
          flush();
        else
          cur += c;
      }

      flush();
      return out;
    }

    std::string ReadText(const std::filesystem::path &path)
    {
      std::ifstream in(path, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // A signed DXIL container has a non-zero digest (bytes 4..19). dxcompiler.dll signs only when dxil.dll is loadable; D3D12 rejects unsigned DXIL
    bool IsSigned(const DxilBlob &dxil)
    {
      if (dxil.size() < 20)
        return false;

      const uint8_t *p = dxil.data();

      for (size_t i = 4; i < 20; ++i)
        if (p[i])
          return true;

      return false;
    }

    // The pack CMake embeds as RCDATA "SST_SHADER_PACK" (see CMakeLists.txt, SST_SHADER_BAKE), normally compressed (shader_pack.h).
    // It is unpacked once into `storage`, which lives as long as the cache, so variants can point into it. An uncompressed pack is
    // used in place (resource memory stays mapped for the life of the process).
    bool LoadEmbeddedPack(pack::View &view, std::vector<uint8_t> &storage, const wchar_t *name, uint32_t version)
    {
#if !defined(_WIN32)
      (void)view, (void)storage, (void)name, (void)version;
      return false; // (the pack is a Windows resource; other platforms compile at run time for now)
#else
      HMODULE module = GetModuleHandleW(nullptr);
      HRSRC res = FindResourceW(module, name, MAKEINTRESOURCEW(10)); // RT_RCDATA

      if (!res)
        return false;

      HGLOBAL mem = LoadResource(module, res);
      const auto *data = static_cast<const uint8_t *>(mem ? LockResource(mem) : nullptr);
      const size_t size = SizeofResource(module, res);

      if (!data || size < sizeof(pack::CompressedHeader))
        return false;

      pack::CompressedHeader ch;
      std::memcpy(&ch, data, sizeof(ch));

      if (ch.magic != pack::kCompressedMagic)
        return view.Open(data, size, version);

      if (ch.raw_size > (1ull << 31))
        return false;

      const auto start = std::chrono::steady_clock::now();
      DECOMPRESSOR_HANDLE decompressor = nullptr;

      if (!CreateDecompressor(ch.algorithm, nullptr, &decompressor))
      {
        REXLOG_ERROR("[sst-native] shader pack: can't create the decompressor ({})", GetLastError());
        return false;
      }

      storage.resize(size_t(ch.raw_size));
      SIZE_T written = 0;
      const bool ok = Decompress(decompressor, const_cast<uint8_t *>(data + sizeof(ch)), size - sizeof(ch), storage.data(), storage.size(), &written) && written == storage.size();
      CloseDecompressor(decompressor);

      if (!ok)
      {
        REXLOG_ERROR("[sst-native] shader pack: decompression failed ({})", GetLastError());
        storage.clear();
        return false;
      }

      REXLOG_INFO("[sst-native] shader pack: {:.1f} MB unpacked from {:.1f} MB in {:.0f} ms", double(storage.size()) / (1 << 20), double(size) / (1 << 20), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
      return view.Open(storage.data(), storage.size(), version);
#endif
    }

    // A pack file next to the exe (the SPIR-V packs: shaders/spirv*.pack), compressed with pack::CompressPlanar or raw. Unpacked into `storage`, which lives as long as the cache
    bool LoadPackFile(pack::View &view, std::vector<uint8_t> &storage, const std::filesystem::path &path, uint32_t version)
    {
      std::ifstream in(path, std::ios::binary);

      if (!in)
        return false;

      const auto start = std::chrono::steady_clock::now();
      const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      pack::CompressedHeader ch = {};

      if (file.size() >= sizeof(ch))
        std::memcpy(&ch, file.data(), sizeof(ch));

      if (ch.magic == pack::kCompressedMagic)
      {
        if (ch.algorithm != pack::kAlgorithmLzPlanar || ch.raw_size > (1ull << 31) || !pack::DecompressPlanar(file.data() + sizeof(ch), file.size() - sizeof(ch), size_t(ch.raw_size), storage))
        {
          REXLOG_ERROR("[sst-native] shader pack {}: can't unpack it", path.string());
          storage.clear();
          return false;
        }
      }
      else
      {
        storage = file;
      }

      if (!view.Open(storage.data(), storage.size(), version))
      {
        REXLOG_WARN("[sst-native] shader pack {} is from another build (version): compiling shaders on first use instead", path.string());
        storage.clear();
        return false;
      }

      REXLOG_INFO("[sst-native] shader pack: {:.1f} MB unpacked from {} in {:.0f} ms", double(storage.size()) / (1 << 20), path.string(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
      return true;
    }

    // Compile threads run below normal priority, at normal priority while a draw waits for their shader
    void SetCompilePriority(bool urgent)
    {
#if defined(_WIN32)
      SetThreadPriority(GetCurrentThread(), urgent ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
#else
      (void)urgent;
#endif
    }
  }

  struct Worker
  {
    dxc::Ref<IDxcCompiler3> compiler;
  };

  bool ShaderCache::Initialize(gfx::ShaderFormat format, const std::filesystem::path &cache_dir)
  {
    format_ = format;
    fast_ = REXCVAR_GET(sst_fast_shaders);
    const bool spirv = format == gfx::ShaderFormat::kSpirv;

    // SHADER PRECISION: FAST needs its own pack; without one (a build with SST_SHADER_BAKE_FAST off) every shader would compile at
    // run time, which is far slower than the accuracy is worth on the PCs that want FAST: use ACCURATE then
    if (spirv)
    {
      // Files next to the exe (the embedded packs hold DXIL). FAST without its pack: ACCURATE, as with DXIL
      const auto exe = rex::filesystem::GetExecutableFolder();

      if (fast_ && !LoadPackFile(pack_, pack_storage_, exe / pack::kSpirvPackFileFast, pack::kCacheVersionSpirvFast))
      {
        if (LoadPackFile(pack_, pack_storage_, exe / pack::kSpirvPackFile, pack::kCacheVersionSpirv))
        {
          REXLOG_WARN("[sst-native] shader precision FAST: no fast SPIR-V shader pack next to the exe; using ACCURATE");
          fast_ = false;
        }
      }
      else if (!fast_)
      {
        LoadPackFile(pack_, pack_storage_, exe / pack::kSpirvPackFile, pack::kCacheVersionSpirv);
      }
    }
    else if (fast_ && !LoadEmbeddedPack(pack_, pack_storage_, pack::kResourceNameFast, pack::kCacheVersionFast))
    {
      if (LoadEmbeddedPack(pack_, pack_storage_, pack::kResourceName, pack::kCacheVersion))
      {
        REXLOG_WARN("[sst-native] shader precision FAST: this exe has no fast shader pack (SST_SHADER_BAKE_FAST off); using ACCURATE");
        fast_ = false;
      }
    }
    else if (!fast_)
    {
      LoadEmbeddedPack(pack_, pack_storage_, pack::kResourceName, pack::kCacheVersion);
    }

    version_ = pack::CacheVersion(fast_, spirv);
    const std::filesystem::path base = spirv ? cache_dir / "spirv" : cache_dir;
    cache_dir_ = fast_ ? base / "fast" : base; // FAST shaders kept apart, so switching back and forth keeps both caches
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);
    std::filesystem::create_directories(cache_dir_ / "failed", ec);
    containers_dir_ = cache_dir.parent_path() / "containers";
    std::filesystem::create_directories(containers_dir_, ec);

    REXLOG_INFO("[sst-native] shader precision: {}", fast_ ? "FAST (plain multiplies)" : "ACCURATE (Xbox 360 multiply rule)");

    if (pack_.count())
      REXLOG_INFO("[sst-native] shader pack: {} precompiled shader variants{}", pack_.count(), spirv ? " (SPIR-V)" : " built into the exe");
    else if (spirv)
      REXLOG_INFO("[sst-native] Vulkan: no SPIR-V shader pack next to the exe (shaders/spirv*.pack): shaders compile on first use (kept in {})", cache_dir_.string());
    else
      REXLOG_INFO("[sst-native] no shader pack in the exe (built without SST_SHADER_BAKE or game files): shaders compile on first use");

    // Debug overrides
    if (const std::string dir = REXCVAR_GET(sst_shader_override); !dir.empty())
    {
      override_dir_ = std::filesystem::path(dir);

      if (override_dir_.is_relative())
        override_dir_ = rex::filesystem::GetExecutableFolder() / override_dir_;

      if (!std::filesystem::is_directory(override_dir_, ec))
        REXLOG_WARN("[sst-native] shader override: folder {} not found", override_dir_.string());

      for (const auto &file : std::filesystem::directory_iterator(override_dir_, ec))
      {
        const auto name = file.path().filename().string();

        if (name == "all.args")
        {
          override_all_ = true;
          override_all_args_ = SplitArgs(ReadText(file.path()));
          REXLOG_INFO("[sst-native] shader override: every shader compiled with extra DXC arguments \"{}\"", ReadText(file.path()));
          continue;
        }

        if (file.path().extension() != ".hlsl" || name.size() < 16)
          continue;

        uint64_t hash = 0;

        if (std::sscanf(name.substr(0, 16).c_str(), "%16llx", reinterpret_cast<unsigned long long *>(&hash)) != 1)
          continue;

        Override o;
        o.source = ReadText(file.path());

        if (o.source.rfind("// dxc:", 0) == 0)
          o.args = SplitArgs(o.source.substr(7, o.source.find('\n') == std::string::npos ? std::string::npos : o.source.find('\n') - 7));

        REXLOG_INFO("[sst-native] shader override: {:016X} from {} ({} extra DXC arguments)", hash, name, o.args.size());
        overrides_[hash] = std::move(o);
      }
    }

    // Check DXC is usable before starting workers
    dxc::Ref<IDxcCompiler3> probe;

    if (FAILED(dxc::CreateInstance(format_, CLSID_DxcCompiler, __uuidof(IDxcCompiler3), probe.put())))
    {
      if (spirv)
        REXLOG_ERROR("[sst-native] {} not found: shaders can't be compiled for Vulkan. It comes from a DirectXShaderCompiler release (the build copies it next to the exe)", dxc::LibraryName(format_));
      else
        REXLOG_ERROR("[sst-native] dxcompiler.dll not found: shaders can't be compiled. Copy dxcompiler.dll and dxil.dll from the Windows SDK (Windows Kits\\10\\bin\\<version>\\x64) next to the exe.");
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
      urgent_.clear();
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
      REXLOG_INFO("[sst-native] shader {:016X}: memory-export vertex shader, not translated yet", hash);
      return raw;
    }

    // Baked by the build: the spec mask is known without translating, so draw-time variants can come from the pack too
    if (const pack::Entry *e = pack_.Find(hash, 0))
    {
      raw->spec_mask.store(e->spec_mask, std::memory_order_relaxed);
      raw->translated.store(true, std::memory_order_release);
    }

    GetVariant(raw, 0);
    return raw;
  }

  const ShaderCache::Override *ShaderCache::FindOverride(uint64_t hash) const
  {
    auto it = overrides_.find(hash);
    return it == overrides_.end() ? nullptr : &it->second;
  }

  bool ShaderCache::FromPack(ShaderEntry *entry, uint32_t spec, ShaderVariant *variant)
  {
    if (override_all_ || FindOverride(entry->hash))
      return false;

    const pack::Entry *e = pack_.Find(entry->hash, spec);

    if (!e)
      return false;

    stats_.from_pack.fetch_add(1);

    if (!e->size)
    { // failed at build time: the same translator would fail again
      stats_.failed.fetch_add(1);
      variant->state.store(ShaderVariant::State::kFailed, std::memory_order_release);
      return true;
    }

    variant->dxil.set_view(pack_.Dxil(*e), e->size);
    variant->state.store(ShaderVariant::State::kReady, std::memory_order_release);
    return true;
  }

  void ShaderCache::Enqueue(const Job &job)
  {
    {
      std::lock_guard lock(queue_mutex_);
      (job.urgent ? urgent_ : queue_).push_back(job);
    }

    queue_cv_.notify_one();
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

  const ShaderVariant *ShaderCache::GetVariant(ShaderEntry *entry, uint32_t spec, bool urgent)
  {
    if (!entry || entry->memexport)
      return nullptr;

    if (spec != 0)
    {
      // Until variant 0 is translated we don't know which bits matter: hurry variant 0 along if a draw is waiting
      if (!entry->translated.load(std::memory_order_acquire))
      {
        if (urgent)
          GetVariant(entry, 0, true);

        return nullptr;
      }

      spec &= entry->spec_mask.load(std::memory_order_relaxed);
    }

    ShaderVariant *variant;
    bool fresh = false;
    {
      std::lock_guard lock(entry->variants_mutex);
      auto &slot = entry->variants[spec];

      if (!slot && spec != 0 && format_ == gfx::ShaderFormat::kSpirv)
      {
        // SPIR-V: every variant is variant 0's module with another specialization constant
        auto base = entry->variants.find(0);
        const auto base_state = base != entry->variants.end() && base->second ? base->second->state.load(std::memory_order_acquire) : ShaderVariant::State::kQueued;

        if (base_state == ShaderVariant::State::kQueued)
        {
          entry->variants.erase(spec);
          return nullptr;
        }

        slot = std::make_unique<ShaderVariant>();
        slot->spec = spec;

        if (base_state == ShaderVariant::State::kReady)
          slot->dxil.set_view(base->second->dxil.data(), base->second->dxil.size());

        slot->state.store(base_state, std::memory_order_release);
      }

      if (!slot)
      {
        slot = std::make_unique<ShaderVariant>();
        fresh = true;

        // Precompiled by the build: ready right away (under the lock, so no other thread sees it half set up)
        if (FromPack(entry, spec, slot.get()))
          fresh = false;
      }

      variant = slot.get();
    }

    if (fresh)
    {
      pending_.fetch_add(1);
      Enqueue({entry, spec, variant, urgent});
      return nullptr;
    }

    const auto state = variant->state.load(std::memory_order_acquire);

    // Still waiting in the creation-order queue: put a second job at the front (the worker skips whichever copy comes second)
    if (state == ShaderVariant::State::kQueued && urgent && !variant->claimed.load(std::memory_order_relaxed) && !variant->boosted.exchange(true))
    {
      stats_.boosted.fetch_add(1);
      Enqueue({entry, spec, variant, true});
    }

    return state == ShaderVariant::State::kReady ? variant : nullptr;
  }

  void ShaderCache::WorkerMain(unsigned index)
  {
    SetCompilePriority(false); // background
#ifdef _WIN32
    // SetThreadDescription is Windows 10 1607+; look it up so older systems (and older SDK headers) still work
    using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);

    if (auto fn = reinterpret_cast<SetThreadDescriptionFn>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
    {
      wchar_t name[32];
      swprintf(name, 32, L"sst shader compiler %u", index);
      fn(GetCurrentThread(), name);
    }
#endif
    Worker worker;
    bool raised = false;

    for (;;)
    {
      Job job;
      {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [this]
                       { return stopping_ || !queue_.empty() || !urgent_.empty(); });

        if (stopping_)
          return;

        auto &q = urgent_.empty() ? queue_ : urgent_;
        job = q.front();
        q.pop_front();
      }

      // A boosted variant is queued twice: only the first job to get here builds it
      if (job.variant->claimed.exchange(true, std::memory_order_acq_rel))
        continue;

      // A draw is waiting for this one: compete with the game at normal priority (low-core machines otherwise starve it for seconds)
      if (job.urgent != raised)
      {
        SetCompilePriority(job.urgent);
        raised = job.urgent;
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

    // Not in the exe's shader pack (the game generated it at runtime, or the exe was built without one): keep the container, so the
    // next build can bake it (CMake passes <exe>/cache/containers to sst_shader_bake). Once per shader, a few KB each.
    if (job.spec == 0)
    {
      char name[24];
      std::snprintf(name, sizeof(name), "%016llX.bin", static_cast<unsigned long long>(entry.hash));
      const auto keep = containers_dir_ / name;
      std::error_code ec;

      if (!std::filesystem::exists(keep, ec))
      {
        auto tmp = keep;
        tmp += ".tmp";
        std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char *>(entry.container.data()), std::streamsize(entry.container.size()));
        std::filesystem::rename(tmp, keep, ec);
      }
    }

    auto finish = [&](ShaderVariant::State state, uint32_t mask)
    {
      if (job.spec == 0)
      {
        entry.spec_mask.store(mask, std::memory_order_relaxed);
        entry.translated.store(true, std::memory_order_release);
      }

      variant.state.store(state, std::memory_order_release);
    };

    const Override *over = FindOverride(entry.hash);
    const bool overridden = over || override_all_;

    // Disk cache
    std::ifstream in;

    if (!overridden)
      in.open(path, std::ios::binary);

    if (in.is_open())
    {
      FileHeader header{};

      if (in.read(reinterpret_cast<char *>(&header), sizeof(header)) && header.magic == kFileMagic && header.version == version_ && header.dxil_size > 0 && header.dxil_size < (16u << 20))
      {
        variant.dxil.resize(header.dxil_size);

        if (in.read(reinterpret_cast<char *>(variant.dxil.writable()), header.dxil_size))
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
      std::snprintf(name, sizeof(name), "%016llX_%u", static_cast<unsigned long long>(entry.hash), job.spec);
      std::ofstream(cache_dir_ / "failed" / (std::string(name) + ".bin"), std::ios::binary).write(reinterpret_cast<const char *>(entry.container.data()), std::streamsize(entry.container.size()));

      if (!source.empty())
        std::ofstream(cache_dir_ / "failed" / (std::string(name) + ".hlsl")) << source;

      std::ofstream(cache_dir_ / "failed" / (std::string(name) + ".txt")) << why;
      finish(ShaderVariant::State::kFailed, tr.spec_constants_mask);
    };

    if (!tr.ok)
      return fail("translation failed: " + tr.error, {});

    // DXIL: the variant's spec constants are compiled in. SPIR-V: g_SpecConstants is specialization constant 0 (set per pipeline)
    const bool spirv = format_ == gfx::ShaderFormat::kSpirv;
    const std::string source = (over && !over->source.empty() ? over->source : tr.hlsl) + (spirv ? std::string() : sst::xenos::SpecConstantsDefinition(job.spec));

    if (over)
      REXLOG_INFO("[sst-native] shader override: compiling {:016X} variant {} from the override folder", entry.hash, job.spec);

    if (!worker.compiler && FAILED(dxc::CreateInstance(format_, CLSID_DxcCompiler, __uuidof(IDxcCompiler3), worker.compiler.put())))
      return fail(std::string(dxc::LibraryName(format_)) + " not available", source);

    // Same arguments as the build-time baker (shader_pack.h)
    std::vector<const wchar_t *> args = {L"-T", tr.pixel_shader ? L"ps_6_0" : L"vs_6_0"};

    if (spirv)
    {
      args.insert(args.end(), std::begin(pack::kDxcSpirvArgs), std::end(pack::kDxcSpirvArgs));

      if (!tr.pixel_shader)
        args.push_back(pack::kDxcSpirvVsArg);
    }
    else
    {
      args.insert(args.end(), std::begin(pack::kDxcArgs), std::end(pack::kDxcArgs));
    }

    if (fast_)
      args.push_back(pack::kFastMathArg);

    for (const auto &a : override_all_args_)
      args.push_back(a.c_str());

    if (over)
      for (const auto &a : over->args)
        args.push_back(a.c_str());

    std::vector<uint8_t> code;
    std::string errors;

    if (!dxc::Compile(worker.compiler.get(), source, args.data(), uint32_t(args.size()), code, &errors))
      return fail(errors, source);

    variant.dxil.assign(code.begin(), code.end());

    if (!spirv && !IsSigned(variant.dxil) && !stats_.unsigned_warned.exchange(true))
      REXLOG_WARN("[sst-native] DXIL is unsigned: dxil.dll was not found next to dxcompiler.dll. D3D12 will "
                  "reject these shaders; copy dxil.dll from the Windows SDK next to the exe.");

    stats_.compiled.fetch_add(1);
    stats_.compile_us.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()));

    // Store (write to a temp name, then rename, so a crash never leaves a truncated file under the real name). Overrides aren't stored.
    if ((spirv || IsSigned(variant.dxil)) && !overridden)
    {
      const FileHeader header = {kFileMagic, version_, tr.spec_constants_mask, uint32_t(variant.dxil.size())};
      auto tmp = path;
      tmp += ".tmp";
      {
        std::ofstream out(tmp, std::ios::binary);
        out.write(reinterpret_cast<const char *>(&header), sizeof(header));
        out.write(reinterpret_cast<const char *>(variant.dxil.data()), std::streamsize(variant.dxil.size()));
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
    REXLOG_INFO("[sst-native] shaders {}: {} unique ({} memexport skipped), {} from the shader pack, {} from disk cache, {} compiled "
                "(avg {:.1f} ms), {} failed, {} pending ({} moved up for a waiting draw)",
                when, stats_.registered.load(), stats_.memexport.load(), stats_.from_pack.load(), stats_.from_disk.load(), compiled, compiled ? double(us) / compiled / 1000.0 : 0.0, stats_.failed.load(), pending_.load(), stats_.boosted.load());
  }
}