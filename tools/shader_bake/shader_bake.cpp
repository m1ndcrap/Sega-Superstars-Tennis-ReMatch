// sst_shader_bake - build-time shader compiler for sst_rematch. Finds every Xbox 360 shader container in the game's ShaderCache code-cache
// files (and, optionally, raw containers dumped with --sst_dump_shaders), translates each with the vendored XenosRecomp, compiles every
// spec-constant variant the renderer can request with DXC, and writes one pack (src/gpu/native/shader_pack.h) that CMake embeds in the exe.
// Players then never compile these shaders.

#include <sst_translate.h>

#include "gpu/native/shader_pack.h"

#include <xxhash.h>

#ifndef NOMINMAX
#define NOMINMAX // windows.h would otherwise turn std::max / std::min into macros
#endif
#include <windows.h>

#include <compressapi.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace pack = sst::native::pack;
using Microsoft::WRL::ComPtr;

namespace
{
  std::vector<uint8_t> ReadFile(const fs::path &path)
  {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> data;

    if (!in)
      return data;

    in.seekg(0, std::ios::end);
    data.resize(size_t(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char *>(data.data()), std::streamsize(data.size()));
    return data;
  }

  std::string Lower(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                   { return char(std::tolower(c)); });
    return s;
  }

  bool IsSigned(const uint8_t *dxil, size_t size)
  {
    if (size < 20)
      return false;

    for (size_t i = 4; i < 20; ++i)
      if (dxil[i])
        return true;

    return false;
  }

  struct Shader
  {
    uint64_t hash;
    std::vector<uint8_t> container;
  };

  struct Result
  {
    pack::Entry entry{};
    std::vector<uint8_t> dxil;
  };

  // Work cache file: { magic, source hash, size } + DXIL
  struct WorkHeader
  {
    uint32_t magic;
    uint32_t size;
    uint64_t source_hash;
  };

  constexpr uint32_t kWorkMagic = 0x4B575353; // "SSWK"

  struct Options
  {
    fs::path out, dxc, work;
    std::vector<fs::path> game, dump;
    bool no_compress = false;
    bool fast = false; // SHADER PRECISION: FAST pack (-DSST_FAST_MATH)
  };

  bool ParseArgs(int argc, char **argv, Options &o)
  {
    for (int i = 1; i < argc; ++i)
    {
      const std::string a = argv[i];

      if (a == "--no-compress")
      {
        o.no_compress = true;
        continue;
      }

      if (a == "--fast")
      {
        o.fast = true;
        continue;
      }

      if (i + 1 >= argc)
        return false;

      const fs::path v(argv[++i]);

      if (a == "--out")
        o.out = v;
      else if (a == "--dxc")
        o.dxc = v;
      else if (a == "--work")
        o.work = v;
      else if (a == "--game")
        o.game.push_back(v);
      else if (a == "--dump")
        o.dump.push_back(v);
      else
        return false;
    }

    return !o.out.empty() && !o.dxc.empty();
  }
}

int main(int argc, char **argv)
{
  Options opt;

  if (!ParseArgs(argc, argv, opt))
  {
    std::fprintf(stderr, "usage: sst_shader_bake --out <pack> --dxc <dir> [--fast] [--work <dir>] [--game <dir>]... [--dump <dir>]...\n");
    return 2;
  }

  const auto t0 = std::chrono::steady_clock::now();

  // DXC: dxcompiler.dll loads dxil.dll (the signer) by name later, so its folder goes on the DLL search path
  SetDllDirectoryW(opt.dxc.wstring().c_str());
  HMODULE dxc = LoadLibraryExW((opt.dxc / "dxcompiler.dll").wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  auto create = dxc ? reinterpret_cast<DxcCreateInstanceProc>(reinterpret_cast<void *>(GetProcAddress(dxc, "DxcCreateInstance"))) : nullptr;

  if (!create)
  {
    std::fprintf(stderr, "sst_shader_bake: can't load %s\n", (opt.dxc / "dxcompiler.dll").string().c_str());
    return 1;
  }

  // Collect unique containers
  std::map<uint64_t, Shader> shaders;
  uint32_t files = 0, found = 0, memexport = 0;

  auto scan = [&](const fs::path &root, bool code_cache_only)
  {
    std::error_code ec;

    if (!fs::exists(root, ec))
    {
      std::fprintf(stderr, "sst_shader_bake: warning: %s doesn't exist\n", root.string().c_str());
      return;
    }

    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
      if (!it->is_regular_file(ec))
        continue;

      const std::string name = Lower(it->path().filename().string());

      if (Lower(it->path().extension().string()) != ".bin")
        continue;

      if (code_cache_only && name.find("code_cache") == std::string::npos)
        continue;

      const auto data = ReadFile(it->path());
      ++files;
      pack::ForEachContainer(data.data(), data.size(), [&](const uint8_t *c, uint32_t len)
                             {
                               ++found;

                               if (pack::ReadBE32(c) & 0x10)
                               { // memory-export vertex shader: run on the CPU, never translated
                                 ++memexport;
                                 return;
                               }

                               const uint64_t hash = XXH3_64bits(c, len);
                               shaders.try_emplace(hash, Shader{hash, std::vector<uint8_t>(c, c + len)}); });
    }
  };

  for (const auto &g : opt.game)
    scan(g, true);

  for (const auto &d : opt.dump)
    scan(d, false);

  if (shaders.empty())
  {
    std::fprintf(stderr, "sst_shader_bake: no shader containers found (looked for *code_cache*.bin under the game folder)\n");
    return 1;
  }

  std::printf("sst_shader_bake: %zu unique shaders (%u containers in %u files, %u memory-export skipped)\n", shaders.size(), found, files, memexport);

  if (!opt.work.empty())
  {
    std::error_code ec;
    fs::create_directories(opt.work, ec);
  }

  std::vector<const Shader *> list;
  list.reserve(shaders.size());

  for (const auto &[h, s] : shaders)
    list.push_back(&s);

  std::mutex mutex;
  std::vector<Result> results;
  std::atomic<size_t> next{0};
  std::atomic<uint32_t> compiled{0}, reused{0}, failed{0}, unsigned_dxil{0}, done{0};

  auto work = [&]
  {
    ComPtr<IDxcCompiler3> compiler;

    if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
      return;

    for (size_t i; (i = next.fetch_add(1)) < list.size();)
    {
      const Shader &s = *list[i];
      std::vector<Result> local;
      const auto tr = sst::xenos::Translate(s.container.data());

      if (!tr.ok)
      {
        // Recorded so the runtime doesn't retry: same translator, same result
        Result r;
        r.entry = {s.hash, 0, tr.spec_constants_mask, 0, 0};
        local.push_back(std::move(r));
        ++failed;
        std::fprintf(stderr, "sst_shader_bake: %016llX: translation failed: %s\n", (unsigned long long)s.hash, tr.error.c_str());
      }
      else
      {
        const uint32_t bits = tr.spec_constants_mask & pack::RuntimeSpecBits(tr.pixel_shader);

        // Every subset of `bits`, 0 included
        for (uint32_t spec = bits;; spec = (spec - 1) & bits)
        {
          Result r;
          r.entry = {s.hash, spec, tr.spec_constants_mask, 0, 0};
          const std::string source = tr.hlsl + sst::xenos::SpecConstantsDefinition(spec);
          const wchar_t *profile = tr.pixel_shader ? L"ps_6_0" : L"vs_6_0";
          uint64_t source_hash = XXH3_64bits(source.data(), source.size());
          source_hash = XXH3_64bits_withSeed(profile, std::wcslen(profile) * sizeof(wchar_t), source_hash);

          for (const wchar_t *a : pack::kDxcArgs)
            source_hash = XXH3_64bits_withSeed(a, std::wcslen(a) * sizeof(wchar_t), source_hash);

          if (opt.fast)
            source_hash = XXH3_64bits_withSeed(pack::kFastMathArg, std::wcslen(pack::kFastMathArg) * sizeof(wchar_t), source_hash);

          char name[48];
          std::snprintf(name, sizeof(name), "%016llX_%u.dxil", (unsigned long long)s.hash, spec);
          const fs::path cached = opt.work.empty() ? fs::path() : opt.work / name;

          // Work cache
          if (!cached.empty())
          {
            const auto data = ReadFile(cached);
            WorkHeader h{};

            if (data.size() > sizeof(h))
            {
              std::memcpy(&h, data.data(), sizeof(h));

              if (h.magic == kWorkMagic && h.source_hash == source_hash && h.size == data.size() - sizeof(h) && IsSigned(data.data() + sizeof(h), h.size))
              {
                r.dxil.assign(data.begin() + sizeof(h), data.end());
                ++reused;
              }
            }
          }

          if (r.dxil.empty())
          {
            std::vector<const wchar_t *> args = {L"-T", profile};
            args.insert(args.end(), std::begin(pack::kDxcArgs), std::end(pack::kDxcArgs));

            if (opt.fast)
              args.push_back(pack::kFastMathArg);
            DxcBuffer buffer = {source.data(), source.size(), DXC_CP_UTF8};
            ComPtr<IDxcResult> result;
            HRESULT status = E_FAIL;
            ComPtr<IDxcBlob> object;

            if (SUCCEEDED(compiler->Compile(&buffer, args.data(), UINT32(args.size()), nullptr, IID_PPV_ARGS(&result))) && SUCCEEDED(result->GetStatus(&status)) && SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) && object && object->GetBufferSize())
            {
              const auto *b = static_cast<const uint8_t *>(object->GetBufferPointer());
              r.dxil.assign(b, b + object->GetBufferSize());
              ++compiled;

              if (!IsSigned(r.dxil.data(), r.dxil.size()))
                ++unsigned_dxil;
              else if (!cached.empty())
              {
                const WorkHeader h = {kWorkMagic, uint32_t(r.dxil.size()), source_hash};
                auto tmp = cached;
                tmp += ".tmp";
                {
                  std::ofstream out(tmp, std::ios::binary);
                  out.write(reinterpret_cast<const char *>(&h), sizeof(h));
                  out.write(reinterpret_cast<const char *>(r.dxil.data()), std::streamsize(r.dxil.size()));
                }
                std::error_code ec;
                fs::rename(tmp, cached, ec);
              }
            }
            else
            {
              ++failed;
              std::string errors;
              ComPtr<IDxcBlobUtf8> err;

              if (result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err), nullptr)) && err && err->GetStringLength())
                errors.assign(err->GetStringPointer(), std::min<size_t>(err->GetStringLength(), 400));

              std::fprintf(stderr, "sst_shader_bake: %016llX variant %u: DXC failed: %s\n", (unsigned long long)s.hash, spec, errors.c_str());
            }
          }

          local.push_back(std::move(r));

          if (spec == 0)
            break;
        }
      }

      std::lock_guard lock(mutex);

      for (auto &r : local)
        results.push_back(std::move(r));

      if (const uint32_t n = ++done; n % 250 == 0)
        std::printf("sst_shader_bake: %u / %zu shaders\n", n, list.size());
    }
  };

  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> pool;

  for (unsigned i = 0; i < threads; ++i)
    pool.emplace_back(work);

  for (auto &t : pool)
    t.join();

  if (done.load() != list.size())
  {
    std::fprintf(stderr, "sst_shader_bake: couldn't create the DXC compiler\n");
    return 1;
  }

  if (unsigned_dxil)
  {
    std::fprintf(stderr, "sst_shader_bake: %u shaders came out unsigned: dxil.dll must sit next to dxcompiler.dll in %s. "
                         "D3D12 rejects unsigned DXIL, so no pack was written.\n",
                 unsigned_dxil.load(), opt.dxc.string().c_str());
    return 1;
  }

  // Write the pack
  std::sort(results.begin(), results.end(), [](const Result &a, const Result &b)
            { return pack::EntryLess(a.entry, b.entry); });

  size_t offset = sizeof(pack::Header) + results.size() * sizeof(pack::Entry);
  size_t dxil_bytes = 0;

  for (auto &r : results)
  {
    offset = (offset + 15) & ~size_t(15);
    r.entry.offset = uint32_t(offset);
    r.entry.size = uint32_t(r.dxil.size());
    offset += r.dxil.size();
    dxil_bytes += r.dxil.size();
  }

  if (offset > 0xFFFFFFFFull)
  {
    std::fprintf(stderr, "sst_shader_bake: pack too large\n");
    return 1;
  }

  // The pack in memory, then compressed for the exe (DXIL shrinks a lot; the runtime unpacks it once at startup)
  std::vector<uint8_t> raw(offset, 0);
  {
    const pack::Header h = {pack::kMagic, pack::kFormat, pack::CacheVersion(opt.fast), uint32_t(results.size())};
    std::memcpy(raw.data(), &h, sizeof(h));
    size_t pos = sizeof(h);

    for (const auto &r : results)
    {
      std::memcpy(raw.data() + pos, &r.entry, sizeof(r.entry));
      pos += sizeof(r.entry);
    }

    for (const auto &r : results)
      std::memcpy(raw.data() + r.entry.offset, r.dxil.data(), r.dxil.size());
  }

  std::vector<uint8_t> file;
  COMPRESSOR_HANDLE compressor = nullptr;

  if (!opt.no_compress && CreateCompressor(pack::kAlgorithmXpressHuff, nullptr, &compressor))
  {
    SIZE_T needed = 0;
    Compress(compressor, raw.data(), raw.size(), nullptr, 0, &needed); // asks for the output size

    if (needed)
    {
      file.resize(sizeof(pack::CompressedHeader) + needed);
      SIZE_T written = 0;

      if (Compress(compressor, raw.data(), raw.size(), file.data() + sizeof(pack::CompressedHeader), needed, &written) && written < raw.size())
      {
        const pack::CompressedHeader ch = {pack::kCompressedMagic, pack::kAlgorithmXpressHuff, raw.size()};
        std::memcpy(file.data(), &ch, sizeof(ch));
        file.resize(sizeof(ch) + written);
      }
      else
      {
        file.clear();
      }
    }

    CloseCompressor(compressor);
  }

  if (file.empty())
  {
    if (!opt.no_compress)
      std::fprintf(stderr, "sst_shader_bake: warning: compression failed, embedding the pack uncompressed\n");

    file = std::move(raw);
  }

  std::error_code ec;
  fs::create_directories(opt.out.parent_path(), ec);
  auto tmp = opt.out;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    out.write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));

    if (!out)
    {
      std::fprintf(stderr, "sst_shader_bake: can't write %s\n", tmp.string().c_str());
      return 1;
    }
  }

  fs::remove(opt.out, ec);
  fs::rename(tmp, opt.out, ec);

  if (ec)
  {
    std::fprintf(stderr, "sst_shader_bake: can't write %s: %s\n", opt.out.string().c_str(), ec.message().c_str());
    return 1;
  }

  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("sst_shader_bake: %zu variants of %zu shaders (%u compiled, %u reused, %u failed), %.1f MB of DXIL, %.1f MB in the exe -> %s in %.1f s\n",
              results.size(), list.size(), compiled.load(), reused.load(), failed.load(), double(dxil_bytes) / (1 << 20), double(file.size()) / (1 << 20), opt.out.string().c_str(), s);
  return 0;
}