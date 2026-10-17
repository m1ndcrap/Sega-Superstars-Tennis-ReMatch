// Copying the vertex data a draw reads that the game rewrites. Copied with every draw that reads it, as much as it reads; static model data is read live by the render thread.

#include "gpu/native/capture.h"
#include "gpu/native/guest_memory.h"
#include "gpu/native/write_watch.h"

namespace sst::native::capture
{
  namespace
  {
    constexpr uint32_t kVertexFetchConstants = 96;  // 2 dwords each, sharing the texture fetch constants' space
    constexpr uint32_t kFetchTypeVertex = 3;        // dword 0 bits 0-1
    constexpr uint32_t kMaxCopies = 16;             // buffers per draw
    constexpr uint32_t kMaxStride = 64;             // bytes per vertex, for sizing a copy from a vertex count
    constexpr uint32_t kUiVertices = 256;           // unwatched buffers: copied for UI draws of at most this many vertices
    constexpr uint32_t kMaxBuffer = 1u << 20;       // bytes copied per buffer
    constexpr uint32_t kMaxPerDraw = 2u << 20;      // bytes copied per draw
    constexpr uint32_t kMaxScannedIndices = 4096;   // DrawVertices: bigger draws aren't scanned
    constexpr uint32_t kNoisyWrites = 16;           // re-checks of a watched buffer in one frame before it goes back to compares
    constexpr uint64_t kNoisyFrames = 600;          // ... for this long
    constexpr uint32_t kPhysicalView = 0xA0000000u; // where the CPU sees physical memory

    // Index buffer dwords in the guest's byte order (swap mode: 0 none, 1 bytes in 16-bit halves, 2 bytes in dwords, 3 halves), into host order. One loop per mode, without branches inside, so they vectorize
    void SwapIndexWords(uint32_t *words, uint32_t count, uint32_t swap_mode)
    {
      switch (swap_mode)
      {
      case 1:
        for (uint32_t k = 0; k < count; ++k)
          words[k] = ((words[k] & 0x00FF00FFu) << 8) | ((words[k] >> 8) & 0x00FF00FFu);
        break;

      case 2:
        for (uint32_t k = 0; k < count; ++k)
          words[k] = __builtin_bswap32(words[k]);
        break;

      case 3:
        for (uint32_t k = 0; k < count; ++k)
          words[k] = (words[k] << 16) | (words[k] >> 16);
        break;

      default:
        break;
      }
    }

    // The highest index in a list, not counting strip-restart indices (all ones)
    template <typename Index>
    uint32_t MaxIndex(const uint8_t *indices, uint32_t count)
    {
      constexpr Index kRestart = Index(~Index(0));
      Index max = 0;

      for (uint32_t k = 0; k < count; ++k)
      {
        Index v;
        std::memcpy(&v, indices + sizeof(Index) * k, sizeof(Index));
        max = std::max<Index>(max, v == kRestart ? Index(0) : v);
      }

      return max;
    }
  }

  struct Capture::VertexList
  {
    Renderer::GuestCopy items[kMaxCopies];
    uint32_t n = 0, total = 0; // total: bytes newly copied for this draw

    bool Full() const { return n >= kMaxCopies; }

    // Whether this draw already has a copy of [phys, phys + size)
    bool Covers(uint32_t phys, uint32_t size) const
    {
      for (uint32_t i = 0; i < n; ++i)
        if (items[i].begin == phys && items[i].end - items[i].begin >= size)
          return true;

      return false;
    }

    void Add(uint32_t phys, uint32_t size, const uint8_t *data) { items[n++] = {phys, phys + size, data}; }
  };

  uint32_t Capture::DrawVertices(uint32_t dev, const DrawCall &call) const
  {
    if (call.count > kMaxScannedIndices)
      return 0;

    if (!call.indexed)
      return call.start + call.count;

    // The index buffer object: +0 flags (bit 31: 32-bit indices, bits 29-30: swap mode), +0x18 address
    const uint32_t ib = Be32(dev + kDevIndices);

    if (!Plausible(ib))
      return 0;

    const uint32_t flags = Be32(ib);
    const bool index32 = (flags >> 31) & 1;
    const uint32_t swap_mode = (flags >> 29) & 3;
    const uint32_t ib_addr = ToVirtual(Be32(ib + 0x18) & ~3u);
    const uint32_t index_size = index32 ? 4 : 2;

    if (!Plausible(ib_addr) || !GuestReadable(ib_addr + call.start * index_size, call.count * index_size))
      return 0;

    // As the renderer decodes them: whole dwords around the range, swapped, read little-endian
    const uint32_t first_byte = call.start * index_size;
    const uint32_t aligned_begin = first_byte & ~3u;
    const uint32_t aligned_end = (first_byte + call.count * index_size + 3) & ~3u;
    uint32_t words[kMaxScannedIndices + 2];
    const uint32_t nwords = std::min<uint32_t>((aligned_end - aligned_begin) / 4, uint32_t(std::size(words)));
    std::memcpy(words, HostPtr(s_.base, ib_addr + aligned_begin), 4 * size_t(nwords));
    SwapIndexWords(words, nwords, swap_mode);

    const uint8_t *indices = reinterpret_cast<const uint8_t *>(words) + (first_byte - aligned_begin);
    const uint32_t max_index = index32 ? MaxIndex<uint32_t>(indices, call.count) : MaxIndex<uint16_t>(indices, call.count);
    const uint64_t vertices = uint64_t(call.base_vertex) + max_index + 1;
    return vertices <= 16 * kMaxScannedIndices ? uint32_t(vertices) : 0;
  }

  void Capture::Vertices(uint32_t dev, uint32_t vertices, bool ui, const DrawCall &call)
  {
    if (!Plausible(dev))
      return;

    RefreshWatched();
    const auto &watched = s_.watched.vertices_sorted;
    VertexList out;

    // The vertex fetch constants in one read
    uint32_t fetch[2 * kVertexFetchConstants];
    std::memcpy(fetch, HostPtr(s_.base, dev + kDevFetchConstants), sizeof(fetch));

    for (uint32_t slot = 0; slot < kVertexFetchConstants && !out.Full(); ++slot)
    {
      // dword 0: type (bits 0-1) and physical address; dword 1: size in bytes (bits 2-25)
      const uint32_t dw0 = __builtin_bswap32(fetch[2 * slot]);
      const uint32_t phys = dw0 & 0x1FFFFFFCu;

      if ((dw0 & 3) != kFetchTypeVertex || !phys)
        continue;

      const uint32_t fetch_size = __builtin_bswap32(fetch[2 * slot + 1]) & 0x03FFFFFCu;
      const auto w = std::lower_bound(watched.begin(), watched.end(), std::make_pair(phys, 0u));
      const uint32_t watched_size = w != watched.end() && w->first == phys ? w->second : 0; // bytes the renderer saw read

      if (watched_size && !ui && write_watch::Enabled() && CopyWatchedVertices(phys, std::min({watched_size, fetch_size, kMaxBuffer}), watched_size, out))
        continue;

      // How much to copy: UI draws, the vertices they read (at least what the renderer saw read); 3D draws (particles, trails) often draw slices of one big dynamic buffer, so up to the last vertex this one reads. A copy that turns out too short is simply not used (the renderer reads guest memory then)
      uint32_t want = 0;

      if (watched_size && ui)
      {
        want = std::max(watched_size, vertices * kMaxStride);
      }
      else if (watched_size)
      {
        if (!vertices)
          vertices = DrawVertices(dev, call); // (timed as vertex copies)

        want = vertices ? std::min(watched_size, vertices * kMaxStride) : watched_size;
      }
      else if (ui && vertices && vertices <= kUiVertices)
      {
        want = vertices * kMaxStride;
      }

      CopyVertices(phys, std::min({fetch_size, want, kMaxBuffer}), ui, watched_size, out);
    }

    s_.vertices.bytes += out.total;

    if (!out.n)
      return;

    auto *list = reinterpret_cast<Renderer::GuestCopy *>(s_.arena->Alloc(sizeof(Renderer::GuestCopy) * out.n));
    std::copy(out.items, out.items + out.n, list);
    cmd_.verts = list;
    cmd_.vert_count = out.n;
  }

  // A 3D draw's watched buffer, with the write watch: this frame's copy of the whole range, reused while none of its pages were written. False: not handled (the buffer is being compared for a while instead)
  bool Capture::CopyWatchedVertices(uint32_t phys, uint32_t size, uint32_t watched_size, VertexList &out)
  {
    VertexCapture &v = s_.vertices;
    auto [entry, fresh] = v.copies_this_frame.try_emplace(phys, VertexCapture::LastCopy{nullptr, 0});
    VertexCapture::LastCopy &last = entry->second;

    if (!last.health)
      last.health = &v.watch_health[phys];

    VertexCapture::WatchHealth &health = *last.health;

    if (!size || s_.frame < health.quiet_until)
      return false;

    if (out.Covers(phys, size))
      return true;

    if (!fresh && last.armed && last.size >= size && write_watch::Stamp(phys, last.size) == last.stamp)
    {
      out.Add(phys, size, last.data);
      ++v.reused;
      ++v.reused_by_watch;
      return true;
    }

    if (out.total + size > kMaxPerDraw || !GuestPhysicalReadable(phys, size))
      return true;

    // Written since (or first use this frame). Many re-checks in one frame: the buffer is streamed (or shares its pages with busy data), so it goes back to compares for a while
    if (last.armed)
    {
      if (health.frame != s_.frame)
        health.frame = s_.frame, health.writes = 0;

      if (++health.writes > kNoisyWrites)
      {
        health.quiet_until = s_.frame + kNoisyFrames;
        ++v.noisy;
      }
    }

    // Stamp, arm, then read: a write after the stamp is either in the data read or changes the stamp
    const uint32_t stamp = write_watch::Stamp(phys, size);
    write_watch::Arm(phys, size);
    ++v.watch_arms;
    const uint8_t *live = HostPtr(s_.base, phys | kPhysicalView);
    last.stamp = stamp;
    last.armed = true;

    if (!fresh && last.data && last.size >= size && std::memcmp(last.data, live, size) == 0)
    {
      ++v.watch_false; // written, yet unchanged
      out.Add(phys, size, last.data);
      ++v.reused;
      return true;
    }

    uint8_t *copy = s_.arena->Alloc(size);
    std::memcpy(copy, live, size);
    last.data = copy;
    last.size = size;
    out.Add(phys, size, copy);
    out.total += size;
    VertexCapture::Stat &stat = v.stats[phys];
    ++stat.draws;
    stat.bytes += size;
    stat.largest = std::max(stat.largest, size);
    stat.watched = watched_size;
    return true;
  }

  // [phys, phys + size): this frame's earlier copy of the buffer if the guest data hasn't changed since (a compare reads it once, a copy reads and writes it), else a new copy
  void Capture::CopyVertices(uint32_t phys, uint32_t size, bool ui, uint32_t watched_size, VertexList &out)
  {
    if (!size || out.total + size > kMaxPerDraw || out.Covers(phys, size) || !GuestPhysicalReadable(phys, size))
      return;

    VertexCapture &v = s_.vertices;
    const uint8_t *live = HostPtr(s_.base, phys | kPhysicalView);
    auto [entry, fresh] = v.copies_this_frame.try_emplace(phys, VertexCapture::LastCopy{nullptr, 0});
    VertexCapture::LastCopy &last = entry->second;

    if (!fresh && last.size >= size && std::memcmp(last.data, live, size) == 0)
    {
      out.Add(phys, size, last.data);
      ++v.reused;
      return;
    }

    uint8_t *copy = s_.arena->Alloc(size);
    std::memcpy(copy, live, size);
    last = {copy, size, 0, false, last.health}; // not watched: the next 3D draw copies the whole range again
    out.Add(phys, size, copy);
    out.total += size;
    VertexCapture::Stat &stat = v.stats[phys];
    ++stat.draws;
    stat.ui_draws += ui;
    stat.bytes += size;
    stat.largest = std::max(stat.largest, size);

    if (watched_size)
      stat.watched = watched_size;
  }
}