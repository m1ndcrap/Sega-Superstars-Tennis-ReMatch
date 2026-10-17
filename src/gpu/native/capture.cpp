// captures a hooked D3D call (contains commands, the device block delta, & the vertex shader's microcode)

#include "gpu/native/capture.h"
#include "gpu/native/write_watch.h"

#include <rex/logging.h>

#include <bit>

#if defined(__SSE4_1__)
#include <smmintrin.h>
#endif

namespace sst::native::capture
{
  namespace
  {
    // Whether two device-block chunks differ (sizes are multiples of 32 bytes), inline SSE: called ~40 times per draw call, where a library memcmp's call and setup overhead counts
    inline bool ChunkDiffers(const uint8_t *a, const uint8_t *b, uint32_t size)
    {
#if !defined(__SSE4_1__)
      return std::memcmp(a, b, size) != 0;
#else
      __m128i acc = _mm_setzero_si128();

      for (uint32_t k = 0; k < size; k += 32)
      {
        const __m128i x0 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i *>(a + k)), _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + k)));
        const __m128i x1 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i *>(a + k + 16)), _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + k + 16)));
        acc = _mm_or_si128(acc, _mm_or_si128(x0, x1));
      }

      return !_mm_testz_si128(acc, acc);
#endif
    }
  }

  Capture::Capture(State &s) : s_(s), lock_(s.capture_mutex), start_(Ticks()) {}

  Capture::~Capture()
  {
    const uint64_t t = Ticks() - start_;
    s_.perf.capture_ticks += t;
    s_.perf.frame_capture_ticks += t;
  }

  Command &Capture::New(Command::Kind kind)
  {
    cmd_ = Command();
    cmd_.kind = kind;
    return cmd_;
  }

  void Capture::Range(uint32_t addr, uint32_t size)
  {
    if (!size || size > (64u << 20) || !Plausible(addr) || cmd_.overlay_count >= 8)
      return;

    uint8_t *copy = s_.arena->Alloc(size);
    std::memcpy(copy, HostPtr(s_.base, addr), size);
    Overlay(addr, size, copy);
  }

  void Capture::Overlay(uint32_t addr, uint32_t size, const uint8_t *data)
  {
    if (cmd_.overlay_count >= 8)
      return;

    cmd_.overlays[cmd_.overlay_count++] = {addr, addr + size, data};
    s_.perf.snapshot_bytes += size;
  }

  const uint8_t *Capture::Bytes(const void *data, size_t size)
  {
    uint8_t *copy = s_.arena->Alloc(size);
    std::memcpy(copy, data, size);
    return copy;
  }

  void Capture::Submit()
  {
    s_.batch.commands.push_back(cmd_);
    ++s_.perf.calls;

    if (s_.batch.commands.size() >= kBatchCommands)
      Flush();
  }

  void Capture::Flush()
  {
    if (s_.batch.commands.empty() && !s_.batch.arena)
      return;

    {
      std::lock_guard lock(s_.queue.mutex);
      s_.queue.batches.push_back(std::move(s_.batch));
    }

    s_.queue.work_cv.notify_one();
    s_.batch = Batch();
    s_.batch.commands.reserve(kBatchCommands);
  }

  void Capture::EndFrame()
  {
    CapturePerf &perf = s_.perf;
    perf.capture_max_ticks = std::max(perf.capture_max_ticks, perf.frame_capture_ticks);
    perf.frame_capture_ticks = 0;
    s_.batch.arena = s_.arena;
    s_.batch.frame = ++s_.frame;
    Flush();

    // (this frame's copies live in the arena just handed over)
    s_.microcode.copies.clear();
    s_.textures.copied_this_frame.clear();
    s_.textures.frame_bytes = 0;
    s_.vertices.copies_this_frame.clear();

    const auto wait_start = std::chrono::steady_clock::now();
    const uint64_t wait_ticks = Ticks();
    {
      CommandQueue &q = s_.queue;
      std::unique_lock lock(q.mutex);
      q.done_cv.wait(lock, [&]
                     { return q.stop || (!q.free_arenas.empty() && q.frames_done + 1 >= s_.frame); });

      if (q.free_arenas.empty())
      { // stopping
        q.arenas.push_back(std::make_unique<Arena>());
        q.free_arenas.push_back(q.arenas.back().get());
      }

      s_.arena = q.free_arenas.back();
      q.free_arenas.pop_back();
    }

    perf.wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
    start_ += Ticks() - wait_ticks; // not capture time
    ++perf.frames;
  }

  bool Capture::DepthTested(uint32_t dev) const
  {
    const uint32_t dc = Be32(dev + kDevDepthControl);
    return (dc & 2) && ((dc >> 4) & 7) != 7;
  }

  // Device block
  void Capture::Device(uint32_t dev)
  {
    if (!Plausible(dev))
      return;

    {
      auto t = Time(CapturePerf::kDevice);
      DeviceDelta(dev, ConstantChunks(dev));
    }

    auto t = Time(CapturePerf::kMicrocode);

    // PS microcode pointer: the renderer reads it for the constant-use key (ConstantChunks)
    if (const uint32_t ps = Be32(dev + kDevPixelShader); Plausible(ps))
      Range(ps + 0x18, 4);

    if (const uint32_t vs = Be32(dev + kDevVertexShader); Plausible(vs))
      Microcode(vs);
  }

  // Device-block chunks worth comparing for this call: everything but the VS / PS float constant chunks the bound shaders don't read (as the renderer reports it: guest_watch::ConstantUse chunk masks). The compare is ~23 KB of reads per call and 8 KB of it is constants, many of them unused by any one shader. Chunks not compared keep their old value in both the snapshot and the render thread's copy, so they stay in step and are sent as soon as a shader that reads them is drawn
  uint64_t Capture::ConstantChunks(uint32_t dev)
  {
    constexpr uint32_t kVsFirst = (kDevVSConstants - kDevSnapBegin) / kDevChunk, kPsFirst = (kDevPSConstants - kDevSnapBegin) / kDevChunk; // 3, 19
    static_assert((kDevVSConstants - kDevSnapBegin) % kDevChunk == 0 && (kDevPSConstants - kDevSnapBegin) % kDevChunk == 0);
    DeviceCapture &d = s_.device;

    if (const uint64_t v = guest_watch::ConstantUseVersion(); v != d.constant_use_version)
    {
      guest_watch::CopyConstantUse(d.constant_use);
      d.constant_use_version = v;
    }

    // VS object +0x20 / PS object +0x18: the microcode pointer (both snapshotted, so the renderer keys them alike). ~0: unknown
    auto key = [&](uint32_t object, uint32_t ucode_offset) -> uint64_t
    {
      return object && Plausible(object) ? guest_watch::ShaderKey(object, Be32(object + ucode_offset)) : ~0ull;
    };

    const uint64_t vs_key = key(Be32(dev + kDevVertexShader), 0x20), ps_key = key(Be32(dev + kDevPixelShader), 0x18);

    if (vs_key == d.last_vs && ps_key == d.last_ps && d.constant_use_version == d.last_version)
      return d.last_mask;

    // Chunk masks the renderer published (bit i = c[16i .. 16i+15]); unknown shaders: every chunk
    auto mask = [&](uint64_t k) -> uint64_t
    {
      if (k == ~0ull)
        return 0xFFFF;

      auto it = d.constant_use.find(k);
      return it != d.constant_use.end() ? (it->second & 0xFFFF) : 0xFFFF;
    };

    const uint64_t skip = ((~mask(vs_key) & 0xFFFF) << kVsFirst) | ((~mask(ps_key) & 0xFFFF) << kPsFirst);
    d.last_vs = vs_key;
    d.last_ps = ps_key;
    d.last_version = d.constant_use_version;
    d.last_mask = kAllDevChunks & ~skip;
    return d.last_mask;
  }

  // The device block's chunks (of those in `compare`) that changed since the last capture
  void Capture::DeviceDelta(uint32_t dev, uint64_t compare)
  {
    DeviceCapture &d = s_.device;
    const uint8_t *host = HostPtr(s_.base, dev + kDevSnapBegin);
    uint8_t *snap = d.snap.get();
    uint64_t mask = 0;
    uint32_t bytes = 0;

    if (d.snap_addr != dev)
    { // another device (or the first capture): all of it
      mask = kAllDevChunks;
      bytes = kDevSnapSize;
      d.snap_addr = dev;
    }
    else
    {
      for (uint64_t m = compare; m; m &= m - 1)
      {
        const uint32_t i = uint32_t(std::countr_zero(m));

        if (ChunkDiffers(snap + i * kDevChunk, host + i * kDevChunk, ChunkSize(i)))
        {
          mask |= uint64_t(1) << i;
          bytes += ChunkSize(i);
        }
      }

      d.chunks_skipped += kDevChunks - uint32_t(std::popcount(compare));
    }

    cmd_.dev_addr = dev;
    cmd_.dev_mask = mask;
    d.chunks_total += kDevChunks;

    if (!mask)
      return;

    uint8_t *delta = s_.arena->Alloc(bytes);
    uint8_t *out = delta;

    for (uint64_t m = mask; m; m &= m - 1)
    {
      const uint32_t i = uint32_t(std::countr_zero(m));
      std::memcpy(out, host + i * kDevChunk, ChunkSize(i));
      std::memcpy(snap + i * kDevChunk, out, ChunkSize(i));
      out += ChunkSize(i);
      ++d.chunks_copied;
    }

    cmd_.dev_delta = delta;
    s_.perf.snapshot_bytes += bytes;
  }

  // The vertex shader object's microcode pointer (D3D may rebind the object to a differently patched copy) and the live microcode: the object keeps a copy of the container header at +0x368 (+8: physical part size). Reused within the frame while unchanged; microcode in the 0xA0000000 / 0xC0000000 views (physical = address - view base) is write-watched, so an unchanged one isn't compared
  void Capture::Microcode(uint32_t vs)
  {
    MicrocodeCapture &mc = s_.microcode;
    Range(vs + 0x20, 4);
    const uint32_t live = ToVirtual(Be32(vs + 0x20));
    const uint32_t size = Be32(vs + 0x368 + 8);

    if (!Plausible(live) || !size || size > 0x10000)
    {
      ++mc.skipped;
      return;
    }

    const uint8_t *host = HostPtr(s_.base, live);
    auto it = mc.copies.find(live);
    const bool watch = write_watch::Enabled() && !mc.watch_off && live >= 0xA0000000u && live < 0xE0000000u;
    const uint32_t phys = live & 0x1FFFFFFFu;

    if (it != mc.copies.end() && it->second.size == size)
    {
      MicrocodeCapture::Copy &c = it->second;

      if (watch && c.armed && write_watch::Stamp(phys, size) == c.stamp)
      {
        Overlay(live, size, c.data);
        ++mc.reused_by_watch;
        return;
      }

      // Stamp, arm, then read: a write after the stamp is either in the data read or changes the stamp
      const uint32_t stamp = watch ? write_watch::Stamp(phys, size) : 0;

      if (watch)
        write_watch::Arm(phys, size);

      if (std::memcmp(c.data, host, size) == 0)
      {
        if (watch && c.armed)
        { // written, yet unchanged
          if (mc.false_frame != s_.frame)
            mc.false_frame = s_.frame, mc.false_alarms = 0;

          if (++mc.false_alarms > 32)
          {
            mc.watch_off = true;
            REXLOG_INFO("[sst-native] VS microcode write watch off: its pages are written often while the microcode stays the same");
          }
        }

        c.stamp = stamp;
        c.armed = watch;
        Overlay(live, size, c.data);
        return;
      }
    }

    const uint32_t stamp = watch ? write_watch::Stamp(phys, size) : 0;

    if (watch)
      write_watch::Arm(phys, size);

    uint8_t *copy = s_.arena->Alloc(size);
    std::memcpy(copy, host, size);
    mc.copies[live] = {size, copy, stamp, watch};
    Overlay(live, size, copy);
  }

  // The renderer's current list of textures / vertex buffers the game rewrites
  void Capture::RefreshWatched()
  {
    WatchedData &w = s_.watched;

    if (const uint64_t v = guest_watch::Version(); v != w.version)
    {
      guest_watch::Copy(w.set);
      w.version = v;
      w.vertices_sorted.assign(w.set.vertices.begin(), w.set.vertices.end());
      std::sort(w.vertices_sorted.begin(), w.vertices_sorted.end());
    }
  }
}