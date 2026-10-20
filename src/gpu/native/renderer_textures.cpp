// Native renderer: the textures a draw samples

#include "gpu/native/draw_context.h"
#include "gpu/native/renderer_debug.h"

namespace sst::native
{
  using namespace renderer_detail;

  const Renderer::TextureUsage &Renderer::GetTextureUsage(const ShaderEntry *entry)
  {
    auto [it, fresh] = texture_usage_.try_emplace(entry);

    if (!fresh)
      return it->second;

    TextureUsage &u = it->second;
    const std::vector<uint8_t> &ct = entry->container;

    auto ct32 = [&](size_t at) -> uint32_t
    {
      return at + 4 <= ct.size() ? (uint32_t(ct[at]) << 24) | (uint32_t(ct[at + 1]) << 16) | (uint32_t(ct[at + 2]) << 8) | ct[at + 3] : 0;
    };

    const uint32_t vsz = ct32(4), shader_off = ct32(0x18);
    const uint32_t phys_off = ct32(shader_off), bytes = ct32(size_t(shader_off) + 4);

    if (!bytes || size_t(vsz) + phys_off + bytes > ct.size())
      return u;

    std::vector<uint32_t> ucode(bytes / 4);

    for (size_t i = 0; i < ucode.size(); ++i)
      ucode[i] = ct32(size_t(vsz) + phys_off + 4 * i);

    u.mask = TextureFetchUsage(ucode.data(), uint32_t(ucode.size()), u.uses);

    for (uint32_t i = 0; i < 32; ++i)
    {
      if (!((u.mask >> i) & 1))
        continue;

      if (i >= 16 && LogOnce(0xD000000000000000ull ^ entry->hash))
        REXLOG_WARN("[sst-native] shader {:016X} samples fetch constant {} (>= 16, not bound)", entry->hash, i);

      if (u.uses[i].mixed_dimensions && LogOnce(0xD100000000000000ull ^ entry->hash))
        REXLOG_WARN("[sst-native] shader {:016X} samples fetch constant {} with several dimensions", entry->hash, i);
    }

    return u;
  }

  void Renderer::BindTextures(const uint8_t *base, uint32_t dev, const ShaderEntry *vs_entry, const ShaderEntry *ps_entry, uint32_t *sc)
  {
    if (!textures_)
      return;

    const TextureCache::AllocateFn allocate = [this](size_t size)
    {
      const Upload u = Allocate(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      return TextureCache::UploadSpace{u.cpu, u.buffer, u.offset};
    };

    bool textured = false, placeholder = false;

    for (const ShaderEntry *e : {vs_entry, ps_entry})
    {
      if (!e)
        continue;

      const TextureUsage &u = GetTextureUsage(e);

      for (uint32_t i = 0; i < 16; ++i)
      {
        if (!((u.mask >> i) & 1))
          continue;

        const TextureFetchUse &use = u.uses[i];
        uint32_t fetch[6];

        for (uint32_t k = 0; k < 6; ++k)
          fetch[k] = be32(base, dev + kDevFetchConstants + 24 * i + 4 * k);

        const uint32_t dimension = use.dimension ? use.dimension - 1u : 1u;

        if (!composite_pages_.empty() && composite_pages_.count((fetch[1] >> 12) & 0x1FFFF))
          draw_composite_ = true;

        if (capture_active_ || trace_draw_)
          draw_textures_ += fmt::format(" t{}={:08X}:{}x{} fmt {} dim {} fetchdim {}{}", i, fetch[1] & ~0xFFFu, (fetch[2] & 0x1FFF) + 1, ((fetch[2] >> 13) & 0x1FFF) + 1, fetch[1] & 0x3F, (fetch[5] >> 9) & 3, dimension, composite_pages_.count((fetch[1] >> 12) & 0x1FFFF) ? "c" : "");

        const uint32_t slot = textures_->GetTexture(*list_, allocate, fetch, dimension);

        if (slot != TextureCache::kNone)
        {
          textured = true;

          if (dimension == 2)
            sc[16 + i] = slot;
          else if (dimension == 3)
            sc[32 + i] = slot;
          else
            sc[i] = slot;
        }
        else
        {
          placeholder = true;

          // Stacked texture sampled as 3D: the shadow map array (not rendered yet)
          const bool stacked = ((fetch[5] >> 9) & 3) == 1 && ((fetch[1] >> 10) & 1);

          if (dimension == 2 && stacked)
          {
            sc[16 + i] = kPlaceholder3DBlack;
            ++stats_.shadow_placeholders;
          }
        }

        sc[48 + i] = textures_->GetSampler(fetch, use);
      }
    }

    if (textured)
      ++stats_.textured_draws;

    if (placeholder)
      ++stats_.texture_placeholders;
  }
}