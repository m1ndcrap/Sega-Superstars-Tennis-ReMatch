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
#include <vector>

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

  // SPIR-V packs (Vulkan) and their disk cache: their own versions, so a DXIL pack is never read as SPIR-V
  inline constexpr uint32_t kCacheVersionSpirv = kCacheVersion + 2000;
  inline constexpr uint32_t kCacheVersionSpirvFast = kCacheVersion + 3000;
  inline constexpr uint32_t CacheVersion(bool fast, bool spirv) { return spirv ? (fast ? kCacheVersionSpirvFast : kCacheVersionSpirv) : CacheVersion(fast); }

  // SPIR-V packs are files next to the exe (not resources: they serve Vulkan only, and Linux has no resource section)
  inline constexpr const char *kSpirvPackFile = "shaders/spirv.pack";
  inline constexpr const char *kSpirvPackFileFast = "shaders/spirv_fast.pack";

  // DXC arguments for every translated shader (after "-T <profile>")
  inline constexpr const wchar_t *kDxcArgs[] = {
      L"-HV",
      L"2021",
      L"-all-resources-bound",
      L"-Wno-ignored-attributes",
      L"-Qstrip_reflect",
      L"-Qstrip_debug",
  };

  // SPIR-V (Vulkan) instead of kDxcArgs: one module per shader, its variant is specialization constant 0 (XenosRecomp's __spirv__ layout: push-constant buffer addresses, descriptor sets = register spaces)
  // Vertex shaders also get kDxcSpirvVsArg (Vulkan's clip space is y-down: flipping in the shader keeps viewports and winding as in D3D12)
  inline constexpr const wchar_t *kDxcSpirvArgs[] = {
      L"-HV",
      L"2021",
      L"-all-resources-bound",
      L"-Wno-ignored-attributes",
      L"-spirv",
      L"-fvk-use-dx-layout",
      L"-fspv-target-env=vulkan1.2",
      L"-Qstrip_debug",
  };

  inline constexpr const wchar_t *kDxcSpirvVsArg = L"-fvk-invert-y";

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
  inline constexpr uint32_t kAlgorithmLzPlanar = 0x504C;   // "LP": lz below over the 4 byte planes of the 32-bit words (portable; SPIR-V packs)

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
      {
        if (size_t(entries[i].offset) + entries[i].size > size)
          return false;
      }

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

  // A small LZ77 codec with no dependencies so that packs can be unpacked on every platform.
  // Stream of sequences: varint literal count, the literals, varint match length - 4 (all ones = end of stream), varint match offset - 1
  namespace lz
  {
    inline void PutVarint(std::vector<uint8_t> &out, uint64_t v)
    {
      while (v >= 0x80)
      {
        out.push_back(uint8_t(v) | 0x80);
        v >>= 7;
      }

      out.push_back(uint8_t(v));
    }

    inline bool GetVarint(const uint8_t *&p, const uint8_t *end, uint64_t &v)
    {
      v = 0;

      for (int shift = 0; shift < 64; shift += 7)
      {
        if (p >= end)
          return false;

        const uint8_t b = *p++;
        v |= uint64_t(b & 0x7F) << shift;

        if (!(b & 0x80))
          return true;
      }

      return false;
    }

    inline constexpr uint64_t kEnd = ~uint64_t(0) >> 1;

    inline std::vector<uint8_t> Compress(const uint8_t *in, size_t size)
    {
      std::vector<uint8_t> out;
      out.reserve(size / 3 + 16);
      constexpr int kBits = 20;
      std::vector<uint32_t> table(size_t(1) << kBits, 0xFFFFFFFFu);

      auto hash = [&](size_t i)
      {
        uint32_t v;
        std::memcpy(&v, in + i, 4);
        return (v * 2654435761u) >> (32 - kBits);
      };

      size_t pos = 0, literal_start = 0;

      while (pos + 4 <= size)
      {
        const uint32_t h = hash(pos);
        const uint32_t candidate = table[h];
        table[h] = uint32_t(pos);

        if (candidate != 0xFFFFFFFFu && std::memcmp(in + candidate, in + pos, 4) == 0)
        {
          size_t len = 4;

          while (pos + len < size && in[candidate + len] == in[pos + len])
            ++len;

          PutVarint(out, pos - literal_start);
          out.insert(out.end(), in + literal_start, in + pos);
          PutVarint(out, len - 4);
          PutVarint(out, pos - candidate - 1);

          // Index the matched bytes too (sparsely: every other position keeps it fast)
          for (size_t i = pos + 1; i + 4 <= size && i < pos + len; i += 2)
            table[hash(i)] = uint32_t(i);

          pos += len;
          literal_start = pos;
        }
        else
        {
          ++pos;
        }
      }

      PutVarint(out, size - literal_start);
      out.insert(out.end(), in + literal_start, in + size);
      PutVarint(out, kEnd);
      return out;
    }

    // Decodes into `out` (resized to raw_size); false on corrupt input
    inline bool Decompress(const uint8_t *in, size_t size, std::vector<uint8_t> &out, size_t raw_size)
    {
      out.resize(raw_size);
      const uint8_t *p = in, *end = in + size;
      size_t pos = 0;

      for (;;)
      {
        uint64_t literals, length, offset;

        if (!GetVarint(p, end, literals) || literals > size_t(end - p) || pos + literals > raw_size)
          return false;

        std::memcpy(out.data() + pos, p, size_t(literals));
        p += literals;
        pos += size_t(literals);

        if (!GetVarint(p, end, length))
          return false;

        if (length == kEnd)
          return pos == raw_size;

        length += 4;

        if (!GetVarint(p, end, offset) || offset + 1 > pos || pos + length > raw_size)
          return false;

        const uint8_t *src = out.data() + pos - (offset + 1);
        uint8_t *dst = out.data() + pos;

        // Overlapping copies (offset < length) repeat the pattern, as LZ77 intends
        for (uint64_t i = 0; i < length; ++i)
          dst[i] = src[i];

        pos += size_t(length);
      }
    }
  }

  // A pack compressed with lz over byte planes (SPIR-V is a stream of 32-bit words whose high bytes are mostly zero: split into planes of byte 0, 1, 2, 3 of each word it compresses ~3x, against ~1.4x as it is)
  inline std::vector<uint8_t> CompressPlanar(const std::vector<uint8_t> &raw)
  {
    const size_t words = raw.size() / 4;
    std::vector<uint8_t> planes(raw.size());

    for (size_t i = 0; i < words; ++i)
    {
      for (size_t b = 0; b < 4; ++b)
        planes[b * words + i] = raw[i * 4 + b];
    }

    std::memcpy(planes.data() + words * 4, raw.data() + words * 4, raw.size() - words * 4);
    const std::vector<uint8_t> packed = lz::Compress(planes.data(), planes.size());
    std::vector<uint8_t> out(sizeof(CompressedHeader));
    const CompressedHeader ch = {kCompressedMagic, kAlgorithmLzPlanar, raw.size()};
    std::memcpy(out.data(), &ch, sizeof(ch));
    out.insert(out.end(), packed.begin(), packed.end());
    return out;
  }

  // The inverse, for data after the CompressedHeader
  inline bool DecompressPlanar(const uint8_t *data, size_t size, size_t raw_size, std::vector<uint8_t> &out)
  {
    std::vector<uint8_t> planes;

    if (!lz::Decompress(data, size, planes, raw_size))
      return false;

    const size_t words = raw_size / 4;
    out.resize(raw_size);

    for (size_t i = 0; i < words; ++i)
      for (size_t b = 0; b < 4; ++b)
        out[i * 4 + b] = planes[b * words + i];

    std::memcpy(out.data() + words * 4, planes.data() + words * 4, raw_size - words * 4);
    return true;
  }
}