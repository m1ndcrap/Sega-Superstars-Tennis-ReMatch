// sst_rematch - native renderer: shader pack shared by the build-time baker (tools/shader_bake) and the runtime shader cache.
//
// The build translates and compiles every shader container in the game's ShaderCache files (plus optional dumps) ahead of time and embeds the
// resulting DXIL in the exe as a resource. At runtime ShaderCache looks shaders up there first, so players never compile them.
// Header-only and free of Windows / D3D dependencies so the baker and tests can include it anywhere.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace sst::native::pack
{
  // Bump when the translator, the prelude or the compile options change: stale disk cache files are rebuilt and an embedded pack built
  // with another version is ignored.
  // 2: D3D9 multiply rule; 3: vertex index in r0.x; 4: 3D fetch offsets; 5: screen position / render scale; 6: clip transform (c18);
  // 7: render scale per axis (c17.zw); 8: loop-form shaders as an if-chain, alpha test after the loop;
  // 9: reversed depth flipped in the vertex shader (g_SstDepthFlip, c19.x); 10: loop-form shaders stop after 4096 jumps;
  // 11: ... after 256 jumps; 12: vertices with non-finite / absurd positions are clipped; 13: loop form back to switch
  // (not the cause of the Intel hangs after all); 14: cube map directions in two named fields, not a dynamically indexed array (scratch
  // memory, which hung Intel GPUs in the loop-form character lighting shaders)
  inline constexpr uint32_t kCacheVersion = 14;
  // SHADER PRECISION: FAST (plain multiplies, see SST_FAST_MATH in shader_recompiler.cpp) has its own pack and version, so the two never mix
  inline constexpr uint32_t kCacheVersionFast = kCacheVersion + 1000;
  inline constexpr uint32_t CacheVersion(bool fast) { return fast ? kCacheVersionFast : kCacheVersion; }

  // DXC arguments for every translated shader (after "-T <profile>")
  inline constexpr const wchar_t *kDxcArgs[] = {
      L"-HV",
      L"2021",
      L"-all-resources-bound",
      L"-Wno-ignored-attributes",
      L"-Qstrip_reflect",
      L"-Qstrip_debug",
  };
  // Added after kDxcArgs for SHADER PRECISION: FAST
  inline constexpr const wchar_t *kFastMathArg = L"-DSST_FAST_MATH";
  // Embedded resource names (RCDATA)
  inline constexpr const wchar_t *kResourceName = L"SST_SHADER_PACK";
  inline constexpr const wchar_t *kResourceNameFast = L"SST_SHADER_PACK_FAST";

  // Spec-constant bits the renderer can request at draw time (renderer.cpp, Draw: vs_spec is always 0, ps_spec may set kSpecAlphaTest).
  // The baker compiles every subset of (shader's spec mask & these bits). Keep in sync with the renderer.
  inline constexpr uint32_t kRuntimeSpecBitsVS = 0;
  inline constexpr uint32_t kRuntimeSpecBitsPS = 1u << 1; // sst::xenos::kSpecAlphaTest

  inline uint32_t RuntimeSpecBits(bool pixel) { return pixel ? kRuntimeSpecBitsPS : kRuntimeSpecBitsVS; }

  // Pack layout (little-endian, host):
  //   Header
  //   Entry[count], sorted by (hash, spec)
  //   DXIL blobs, each 16-byte aligned, at Entry::offset from the start of the pack
  inline constexpr uint32_t kMagic = 0x4B505353; // "SSPK"
  inline constexpr uint32_t kFormat = 1;

  struct Header
  {
    uint32_t magic;
    uint32_t format;
    uint32_t cache_version;
    uint32_t count;
  };

  struct Entry
  {
    uint64_t hash;      // XXH3-64 of the complete container (as ShaderCache::Register hashes it)
    uint32_t spec;      // spec value, already masked to the bits the shader reads
    uint32_t spec_mask; // SPEC_CONSTANT_* bits the shader reads
    uint32_t offset;    // DXIL offset from the start of the pack
    uint32_t size;      // DXIL size; 0 = translation or compilation failed at build time
  };

  static_assert(sizeof(Header) == 16 && sizeof(Entry) == 24);

  // The pack as embedded in the exe is normally compressed with the Windows Compression API (cabinet.dll, buffer mode):
  // CompressedHeader, then the compressed bytes of the whole pack above. A pack that starts with kMagic is uncompressed.
  inline constexpr uint32_t kCompressedMagic = 0x5A505353; // "SSPZ"
  inline constexpr uint32_t kAlgorithmXpressHuff = 4;      // COMPRESS_ALGORITHM_XPRESS_HUFF: fast to decompress at startup

  struct CompressedHeader
  {
    uint32_t magic;
    uint32_t algorithm;
    uint64_t raw_size;
  };

  static_assert(sizeof(CompressedHeader) == 16);

  inline bool EntryLess(const Entry &a, const Entry &b) { return a.hash != b.hash ? a.hash < b.hash : a.spec < b.spec; }

  // Read-only view of a pack in memory (the exe's resource, or a file)
  class View
  {
  public:
    // Returns false (and stays empty) if the data isn't a valid pack with cache version `version`
    bool Open(const void *data, size_t size, uint32_t version = kCacheVersion)
    {
      data_ = nullptr;
      count_ = 0;

      if (!data || size < sizeof(Header))
        return false;

      Header h;
      std::memcpy(&h, data, sizeof(h));

      if (h.magic != kMagic || h.format != kFormat || h.cache_version != version || size < sizeof(Header) + size_t(h.count) * sizeof(Entry))
        return false;

      const auto *entries = reinterpret_cast<const Entry *>(static_cast<const uint8_t *>(data) + sizeof(Header));

      for (uint32_t i = 0; i < h.count; ++i)
        if (size_t(entries[i].offset) + entries[i].size > size)
          return false;

      data_ = static_cast<const uint8_t *>(data);
      size_ = size;
      entries_ = entries;
      count_ = h.count;
      return true;
    }

    bool empty() const { return count_ == 0; }
    uint32_t count() const { return count_; }

    // Entry for (hash, spec), or nullptr
    const Entry *Find(uint64_t hash, uint32_t spec) const
    {
      const Entry key{hash, spec, 0, 0, 0};
      const Entry *end = entries_ + count_;
      const Entry *it = std::lower_bound(entries_, end, key, EntryLess);
      return it != end && it->hash == hash && it->spec == spec ? it : nullptr;
    }

    const uint8_t *Dxil(const Entry &e) const { return data_ + e.offset; }

  private:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    const Entry *entries_ = nullptr;
    uint32_t count_ = 0;
  };

  inline uint32_t ReadBE32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

  // Size of the Xbox 360 shader container at `p` (virtual + physical part), or 0 if it doesn't look like one or doesn't fit in `avail` bytes
  inline uint32_t ContainerSize(const uint8_t *p, size_t avail)
  {
    constexpr uint32_t kHeaderBytes = 36; // ShaderContainer

    if (avail < kHeaderBytes)
      return 0;

    const uint32_t flags = ReadBE32(p);

    if ((flags & 0xFFFFFF00u) != 0x102A1100u)
      return 0;

    const uint64_t virt = ReadBE32(p + 4), phys = ReadBE32(p + 8);
    const uint64_t len = virt + phys;

    if (virt < kHeaderBytes || len > (1u << 20) || len > avail)
      return 0;

    return uint32_t(len);
  }

  // Calls fn(container, size) for every shader container in a game ShaderCache code-cache file ("<words> u32 len, container" records), or
  // for a single raw container (a --sst_dump_shaders file). Containers are recognised by their magic and a preceding big-endian length
  // that matches the container's own sizes, so the variable-length record headers don't need parsing.
  template <class Fn>
  void ForEachContainer(const uint8_t *data, size_t size, Fn &&fn)
  {
    if (const uint32_t len = ContainerSize(data, size); len && len == size)
    {
      fn(data, len);
      return;
    }

    size_t pos = 4;

    while (pos + 36 <= size)
    {
      if (data[pos] != 0x10 || data[pos + 1] != 0x2A || data[pos + 2] != 0x11)
      {
        ++pos;
        continue;
      }

      const uint32_t len = ContainerSize(data + pos, size - pos);

      if (len && ReadBE32(data + pos - 4) == len)
      {
        fn(data + pos, len);
        pos += len;
        continue;
      }

      ++pos;
    }
  }
}