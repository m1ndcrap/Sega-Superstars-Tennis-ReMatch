// Native renderer's shader constants: how many each shader reads, the chunks the game thread compares, uploads

#include "gpu/native/draw_context.h"
#include "gpu/native/guest_watch.h"
#include "gpu/native/renderer_debug.h"

namespace sst::native
{
  using namespace renderer_detail;

  // Float constants the shader's microcode reads (bit r of used[r / 64] = c[r]); false if unknown (no microcode, relative addressing)
  static bool ShaderConstantsRead(const ShaderEntry *e, uint64_t used[4])
  {
    const std::vector<uint8_t> &ct = e->container;

    auto ct32 = [&](size_t at) -> uint32_t
    {
      return at + 4 <= ct.size() ? (uint32_t(ct[at]) << 24) | (uint32_t(ct[at + 1]) << 16) | (uint32_t(ct[at + 2]) << 8) | ct[at + 3] : 0;
    };

    const uint32_t vsz = ct32(4), shader_off = ct32(0x18);
    const uint32_t phys_off = ct32(shader_off), bytes = ct32(size_t(shader_off) + 4);

    if (!bytes || bytes % 12 != 0 || size_t(vsz) + phys_off + bytes > ct.size())
      return false;

    std::vector<uint32_t> ucode(bytes / 4);

    for (size_t i = 0; i < ucode.size(); ++i)
      ucode[i] = ct32(size_t(vsz) + phys_off + 4 * i);

    return FloatConstantsRead(ucode.data(), uint32_t(ucode.size()), used);
  }

  // Bytes of float constants to upload for the shader: up to its highest register read (all 256 if unknown)
  uint32_t Renderer::ConstantBytes(const ShaderEntry *e)
  {
    if (!e)
      return 16;

    if (auto it = constant_bytes_.find(e); it != constant_bytes_.end())
      return it->second;

    uint32_t result = 4096;
    uint64_t used[4];

    if (ShaderConstantsRead(e, used))
    {
      int top = -1;

      for (int w = 3; w >= 0 && top < 0; --w)
      {
        if (used[w])
          top = 64 * w + 63 - __builtin_clzll(used[w]);
      }

      result = uint32_t(std::max(top + 1, 1)) * 16;
    }

    constant_bytes_.emplace(e, result);
    return result;
  }

  uint32_t Renderer::ConstantChunkMask(const ShaderEntry *e)
  {
    if (!e)
      return 0xFFFF;

    if (auto it = constant_chunks_.find(e); it != constant_chunks_.end())
      return it->second;

    uint32_t result = 0xFFFF;
    uint64_t used[4];

    if (ShaderConstantsRead(e, used))
    {
      result = 0;

      for (uint32_t c = 0; c < 16; ++c)
      {
        if ((used[c / 4] >> (16 * (c % 4))) & 0xFFFF)
          result |= 1u << c;
      }
    }

    constant_chunks_.emplace(e, result);
    return result;
  }

  void Renderer::PublishConstantUse(uint64_t key, uint32_t mask)
  {
    constexpr uint32_t kPublished = 0x80000000u; // (a shader may read no constants: mask 0, still published once)
    uint32_t &m = constant_use_published_[key];

    if ((m & kPublished) && (m | mask) == m)
      return;

    m |= mask | kPublished;
    guest_watch::ConstantUse(key, mask);
  }

  D3D12_GPU_VIRTUAL_ADDRESS Renderer::UploadConstants(const uint8_t *guest, uint32_t bytes)
  {
    bytes = std::clamp(bytes, 16u, 4096u);
    const uint64_t hash = XXH3_64bits(guest, bytes) ^ (uint64_t(bytes) << 52);

    if (auto it = constant_cache_.find(hash); it != constant_cache_.end())
    {
      ++stats_.constants_reused;
      return it->second;
    }

    const Upload up = Allocate(bytes);

    if (!up.cpu)
      return 0;

    SwapVertexData(2, guest, up.cpu, bytes);
    constant_cache_.emplace(hash, up.gpu);
    ++stats_.constants_uploaded;
    return up.gpu;
  }
}