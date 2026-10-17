// Copying the textures a draw samples that the game rewrites. They are copied when the game issues the draw (once per frame each), because the game may have written the next frame's data by the time the render thread gets to the draw:
// - textures seen changing: always
// - any other known texture: for UI-sized draws (portraits, previews and text pages the game reloads as screens change), within a per-frame budget, and only when its content hash differs from its last copy

// Small 3D draws (court details, effects) read static data and copy nothing: copying for them filled the budget in matches (16 MB of textures + 12 MB of vertices a frame) and wass slow on low-end CPUs

#include "gpu/native/capture.h"
#include "gpu/native/guest_memory.h"

namespace sst::native::capture
{
  namespace
  {
    constexpr uint32_t kTextureFetchConstants = 32;
    constexpr uint32_t kFetchConstantBytes = 24;     // 6 dwords
    constexpr uint32_t kFetchTypeTexture = 2;        // dword 0 bits 0-1
    constexpr uint32_t kMaxTextures = 64;            // per draw (base and mips count separately)
    constexpr uint64_t kFrameBudget = 16ull << 20;   // known-but-unchanging textures copied per frame
    constexpr uint32_t kPhysicalView = 0xA0000000u;  // where the CPU sees physical memory
    constexpr uint32_t kPhysicalLimit = 0x20000000u; // 512 MB
  }

  struct Capture::TextureList
  {
    TextureSnap items[kMaxTextures];
    uint32_t n = 0;
  };

  void Capture::Textures(uint32_t dev, bool ui)
  {
    if (!Plausible(dev))
      return;

    RefreshWatched();
    const auto &watched = s_.watched.set.textures;

    if (watched.empty())
      return;

    TextureCapture &t = s_.textures;
    TextureList out;

    for (uint32_t i = 0; i < kTextureFetchConstants; ++i)
    {
      const uint32_t fetch = dev + kDevFetchConstants + kFetchConstantBytes * i;

      if ((Be32(fetch) & 3) != kFetchTypeTexture)
        continue;

      const uint32_t page = (Be32(fetch + 4) >> 12) & 0x1FFFF; // base address, in 4 KB pages
      auto it = watched.find(page);

      if (it == watched.end())
        continue;

      const guest_watch::TextureRange &r = it->second;

      if (r.external)
        continue; // the renderer draws this one itself

      if (!r.changing && (!ui || t.frame_bytes + r.base_size + r.mip_size > kFrameBudget))
        continue;

      if (!t.copied_this_frame.insert(page).second)
        continue;

      // Not seen changing: copied only if its content differs from its last copy, else the draw carries the content hash
      if (!r.changing)
      {
        const bool readable = GuestPhysicalReadable(r.base_addr, r.base_size) && (!r.mip_size || GuestPhysicalReadable(r.mip_addr, r.mip_size));

        if (!r.base_addr || !r.base_size || out.n >= kMaxTextures || !readable)
          continue;

        const uint64_t hash = HashTexture(r);
        TextureCapture::Check &check = t.checks[page];
        ++t.checks_hashed;

        if (check.known && check.hash == hash)
        {
          out.items[out.n++] = {r.base_addr, r.base_addr + r.base_size, nullptr, hash};
          ++t.checks_unchanged;
          continue;
        }

        check = {hash, true};
      }

      CopyTextureRange(r.base_addr, r.base_size, out);
      CopyTextureRange(r.mip_addr, r.mip_size, out);
      TextureCapture::Stat &stat = t.stats[page];
      ++stat.copies;
      stat.bytes += uint64_t(r.base_size) + r.mip_size;
      stat.changing = r.changing;
    }

    if (!out.n)
      return;

    auto *list = reinterpret_cast<TextureSnap *>(s_.arena->Alloc(sizeof(TextureSnap) * out.n));
    std::copy(out.items, out.items + out.n, list);
    cmd_.tex = list;
    cmd_.tex_count = out.n;
  }

  // The same hash as the renderer's (TextureCache::HashGuest: base, then mips, one stream)
  uint64_t Capture::HashTexture(const guest_watch::TextureRange &r)
  {
    TextureCapture &t = s_.textures;

    if (!t.hash_state)
      t.hash_state = XXH3_createState();

    XXH3_64bits_reset(t.hash_state);
    XXH3_64bits_update(t.hash_state, HostPtr(s_.base, r.base_addr | kPhysicalView), r.base_size);

    if (r.mip_addr && r.mip_size)
      XXH3_64bits_update(t.hash_state, HostPtr(s_.base, r.mip_addr | kPhysicalView), r.mip_size);

    return XXH3_64bits_digest(t.hash_state);
  }

  void Capture::CopyTextureRange(uint32_t phys, uint32_t size, TextureList &out)
  {
    if (!phys || !size || uint64_t(phys) + size > kPhysicalLimit || out.n >= kMaxTextures || !GuestPhysicalReadable(phys, size))
      return;

    uint8_t *copy = s_.arena->Alloc(size);
    std::memcpy(copy, HostPtr(s_.base, phys | kPhysicalView), size);
    out.items[out.n++] = {phys, phys + size, copy};
    s_.textures.bytes += size;
    s_.textures.frame_bytes += size;
  }
}