// sst_rematch - native D3D12 renderer (exe side)

#include "gpu/native/native.h"

#include "gpu/frame_source.h"
#include "gpu/native/hitch_profiler.h"
#include "gpu/native/present_hook.h"
#include "menu/graphics_menu.h"
#include "gpu/native/renderer.h"
#include "gpu/native/shader_cache.h"
#include "gpu/native/texture_cache.h"
#include "gpu/native/guest_memory.h"

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xmemory.h>
#include <rex/ui/d3d12/d3d12_provider.h>

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <exception>
#include <unordered_set>
#include <bit>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

REXCVAR_DECLARE(int32_t, sst_aspect);

REXCVAR_DEFINE_INT32(sst_native_log_every, 600, "SST",
                     "Native renderer: print a status line every N frames (0 = never)");

REXCVAR_DEFINE_BOOL(sst_native_game_thread_priority, true, "SST",
                    "Native renderer: raise the game's rendering thread (the one calling Swap) to "
                    "above-normal priority, so background work (morphs, shader / pipeline "
                    "compiles) never delays its frame");

REXCVAR_DEFINE_BOOL(sst_native_draw_copies, true, "SST",
                    "Native renderer: copy the textures / vertex data of UI draws when the game issues them, for the render "
                    "thread (false = the render thread reads guest memory as it is when it gets to the draw; for testing)");

REXCVAR_DEFINE_BOOL(sst_native_render_thread, true, "SST",
                    "Native renderer: record and submit the game's draws on a separate render "
                    "thread (the game thread only snapshots each call's state). false = do all "
                    "renderer work on the game thread, as before");

namespace sst::native
{
  using rex::ui::d3d12::D3D12Provider;

  namespace
  {
    uint32_t be32(const uint8_t *base, uint32_t addr)
    {
      uint32_t v;
      std::memcpy(&v, base + addr + rex::memory::detail::PhysicalHostOffset(addr), 4);
      return __builtin_bswap32(v);
    }

    // render thread
    constexpr uint32_t kDevSnapBegin = 0x480, kDevSnapEnd = 0x31A0;
    constexpr uint32_t kDevFetchConstants = 0x480; // 32 texture fetch constants of 6 dwords (renderer.cpp)
    constexpr uint32_t kDevIndices = 0x3094;       // index buffer object (renderer.cpp)
    constexpr uint32_t kDevDepthControl = 0x2934;  // RB_DEPTHCONTROL in the register mirror (renderer.cpp, Reg)
    constexpr uint32_t kDevSnapSize = kDevSnapEnd - kDevSnapBegin;
    // The device block is captured as a delta: 256-byte chunks that changed since the previous call's capture (a draw typically changes a few constants and pointers, not all 11.5 KB). The render thread keeps the block up to date and overlays its own copy
    constexpr uint32_t kDevChunk = 256;
    constexpr uint32_t kDevChunks = (kDevSnapSize + kDevChunk - 1) / kDevChunk; // 46
    static_assert(kDevChunks <= 64);
    constexpr uint32_t ChunkSize(uint32_t i) { return std::min(kDevChunk, kDevSnapSize - i * kDevChunk); }
    constexpr size_t kBatchCommands = 128;

    // Bump allocator for one frame's snapshots (chunks are kept between frames)
    class Arena
    {
    public:
      uint8_t *Alloc(size_t size)
      {
        size = (size + 63) & ~size_t(63);

        while (current_ < chunks_.size())
        {
          Chunk &c = chunks_[current_];

          if (used_ + size <= c.size)
          {
            uint8_t *p = c.data.get() + used_;
            used_ += size;
            bytes_ += size;
            return p;
          }

          ++current_;
          used_ = 0;
        }

        const size_t chunk = std::max(size, size_t(8) << 20);
        chunks_.push_back({std::make_unique<uint8_t[]>(chunk + 64), chunk});
        current_ = chunks_.size() - 1;
        used_ = size;
        bytes_ += size;
        return chunks_.back().data.get();
      }

      void Reset()
      {
        current_ = 0;
        used_ = 0;
        bytes_ = 0;
      }

      size_t bytes() const { return bytes_; }

    private:
      struct Chunk
      {
        std::unique_ptr<uint8_t[]> data;
        size_t size = 0;
      };

      std::vector<Chunk> chunks_;
      size_t current_ = 0, used_ = 0, bytes_ = 0;
    };

    struct TextureSnap
    {
      uint32_t begin, end; // physical
      const uint8_t *data;
    };

    struct Command
    {
      enum Kind : uint8_t
      {
        kDraw,
        kClear,
        kResolve,
        kEvent,
        kShader,
        kSwap
      };

      Kind kind = kDraw;
      uint8_t overlay_count = 0;
      GuestOverlay overlays[8];
      DrawCall draw;
      ResolveCall resolve;
      uint32_t device = 0, flags = 0, color = 0, stencil = 0; // clear (device: also event)
      float z = 0;
      const uint8_t *data = nullptr; // event text / shader container (arena)
      uint32_t size = 0, object = 0;
      // Draw-time copies of volatile textures (arena), handed to the renderer before the draw runs
      const TextureSnap *tex = nullptr;
      uint32_t tex_count = 0;
      // Draw-time copies of the draw's small vertex buffers (arena)
      const Renderer::GuestCopy *verts = nullptr;
      uint32_t vert_count = 0;
      // Device block delta (threaded): chunks set in dev_mask, packed in dev_delta (arena)
      uint32_t dev_addr = 0;
      uint64_t dev_mask = 0;
      const uint8_t *dev_delta = nullptr;
    };

    struct Batch
    {
      std::vector<Command> commands;
      Arena *arena = nullptr; // set on the batch that ends a frame: recycled after it
      uint64_t frame = 0;
    };

    struct State
    {
      std::mutex mutex; // serializes renderer use against Shutdown
      SstSetFrameSourceFn set_frame_source = nullptr;
      ShaderCache shaders;
      Renderer renderer;
      const uint8_t *base = nullptr;

      bool threaded = false;
      // Render thread queue
      std::mutex queue_mutex;
      std::condition_variable queue_cv, done_cv;
      std::deque<Batch> queue;
      std::vector<std::unique_ptr<Arena>> arenas;
      std::vector<Arena *> free_arenas;
      uint64_t frames_done = 0;
      bool stop = false;
      std::thread thread;

      // Producer (game threads; D3D calls come from one thread, shader creation may come from others)
      std::mutex capture_mutex;
      Batch batch;
      Arena *arena = nullptr;
      uint64_t frame = 0;
      struct UcodeCopy
      {
        uint32_t size;
        const uint8_t *data;
      };

      std::unordered_map<uint32_t, UcodeCopy> ucode_copies; // live address -> this frame's copy
      // Volatile textures (texture_cache.h): the set as last copied, and the base pages already copied this frame
      uint64_t watch_version = UINT64_MAX;
      guest_watch::Set watch;
      uint64_t frame_texture_bytes = 0;
      std::unordered_set<uint32_t> textures_copied;
      uint64_t texture_bytes = 0, vertex_bytes = 0;
      // Device block as last captured (game thread) / as of the command being run (render thread)
      uint32_t dev_snap_addr = 0, rt_dev_addr = 0;
      std::unique_ptr<uint8_t[]> dev_snap{new uint8_t[kDevSnapSize]}, rt_dev{new uint8_t[kDevSnapSize]};
      uint64_t dev_chunks_copied = 0, dev_chunks_total = 0;
      // Game-thread cost, per logging period
      double capture_ms = 0, wait_ms = 0;
      double frame_capture_ms = 0, capture_max_ms = 0; // this frame's capture / worst per period
      uint64_t captured = 0, snapshot_bytes = 0, period_frames = 0, ucode_skipped = 0;
      // The game's own frame pacing (Swap to Swap)
      std::chrono::steady_clock::time_point last_swap{};
      double swap_max_ms = 0;
      uint64_t swaps_over_20 = 0;
      bool game_thread_boosted = false;
    };

    State *g_state = nullptr;
    std::atomic<bool> g_active{false};

    // VIDEO > ASPECT RATIO (sst_aspect: 4:3, 16:9, 16:10, 21:9, 32:9)
    void DisplayAspectThunk(void *, uint32_t *x, uint32_t *y)
    {
      static constexpr uint32_t kAspectXY[5][2] = {{4, 3}, {16, 9}, {16, 10}, {21, 9}, {32, 9}};
      // 4:3 is the game's own mode, from startup; picked later it waits for a restart
      int aspect = std::clamp(REXCVAR_GET(sst_aspect), 0, 4);

      if (sst::menu::NativeFourThree())
        aspect = 0;
      else if (aspect == 0)
        aspect = 1;

      const auto &a = kAspectXY[aspect];
      *x = a[0];
      *y = a[1];
    }

    ID3D12Resource *AcquireThunk(void *user, uint32_t *slot, uint32_t *width, uint32_t *height)
    {
      return static_cast<Renderer *>(user)->Acquire(slot, width, height);
    }

    void ReleaseThunk(void *user, uint32_t slot)
    {
      static_cast<Renderer *>(user)->Release(slot);
    }

    HMODULE FindPlugin()
    {
      // rex::system::LoadGpuPlugin's per-config names.
      for (const wchar_t *name : {L"rexgpu-sst.dll", L"rexgpu-sstd.dll", L"rexgpu-sstrd.dll"})
        if (HMODULE h = GetModuleHandleW(name))
          return h;

      return nullptr;
    }

    // Locks the state for a hook; empty when inactive.
    std::unique_lock<std::mutex> Enter()
    {
      if (!g_active.load(std::memory_order_acquire))
        return {};

      std::unique_lock lock(g_state->mutex);

      if (!g_active.load(std::memory_order_acquire))
        return {};

      return lock;
    }
  }

  // render thread

  namespace
  {
    const uint8_t *HostPtr(const uint8_t *base, uint32_t addr)
    {
      return base + addr + rex::memory::detail::PhysicalHostOffset(addr);
    }

    uint32_t ToVirtual(uint32_t addr)
    {
      return addr < 0x20000000u ? (addr | 0xA0000000u) : addr;
    }

    bool Plausible(uint32_t addr)
    {
      return addr >= 0x40000000u;
    }

    // Runs one command (render thread, or the game thread without one)
    // Brings the render thread's copy of the device block up to this command (every command, run or not, so it stays in step with the game thread's)
    void ApplyDevice(State &s, const Command &c)
    {
      if (!c.dev_addr)
        return;

      s.rt_dev_addr = c.dev_addr;
      const uint8_t *src = c.dev_delta;

      for (uint64_t m = c.dev_mask; m; m &= m - 1)
      {
        const uint32_t i = uint32_t(std::countr_zero(m));
        std::memcpy(s.rt_dev.get() + i * kDevChunk, src, ChunkSize(i));
        src += ChunkSize(i);
      }
    }

    void ExecuteUnguarded(State &s, const Command &c);

    // A C++ exception escaping a command (std::bad_alloc / length_error from a draw whose guest data was garbage, e.g. freed while
    // queued) would end the process without a crash report: log it, skip the command and carry on
    void Execute(State &s, const Command &c)
    {
      try
      {
        ExecuteUnguarded(s, c);
      }
      catch (const std::exception &e)
      {
        SetGuestOverlays(nullptr, 0);
        s.renderer.SetDrawVertexCopies(nullptr, 0);
        static std::atomic<uint32_t> logged{0};

        if (logged.fetch_add(1) < 20)
          REXLOG_ERROR("[sst-native] command {} failed and was skipped: {} (draw: prim {} indexed {} start {} count {} base vertex {} UP {:08X})", int(c.kind), e.what(), c.draw.prim, c.draw.indexed, c.draw.start, c.draw.count, c.draw.base_vertex, c.draw.up_data);
      }
      catch (...)
      {
        SetGuestOverlays(nullptr, 0);
        s.renderer.SetDrawVertexCopies(nullptr, 0);
        REXLOG_ERROR("[sst-native] command {} failed with an unknown exception and was skipped", int(c.kind));
      }
    }

    void ExecuteUnguarded(State &s, const Command &c)
    {
      GuestOverlay overlays[9];
      uint32_t count = 0;

      for (uint32_t i = 0; i < c.overlay_count; ++i)
        overlays[count++] = c.overlays[i];

      if (c.dev_addr && c.dev_addr == s.rt_dev_addr)
        overlays[count++] = {c.dev_addr + kDevSnapBegin, c.dev_addr + kDevSnapEnd, s.rt_dev.get()};

      SetGuestOverlays(overlays, count);
      const uint8_t *base = s.base;

      switch (c.kind)
      {
      case Command::kDraw:
        for (uint32_t i = 0; i < c.tex_count; ++i)
          s.renderer.AddTextureSnapshot(c.tex[i].begin, c.tex[i].end, c.tex[i].data);

        s.renderer.SetDrawVertexCopies(c.verts, c.vert_count);
        s.renderer.Draw(base, c.draw);
        s.renderer.SetDrawVertexCopies(nullptr, 0);
        break;

      case Command::kClear:
        s.renderer.Clear(base, c.device, c.flags, c.color, c.z, c.stencil);
        break;

      case Command::kResolve:
        s.renderer.Resolve(base, c.resolve);
        break;

      case Command::kEvent:
        s.renderer.TraceEvent(base, c.device, std::string(reinterpret_cast<const char *>(c.data), c.size));
        break;

      case Command::kShader:
      {
        ShaderEntry *entry = s.shaders.Register(c.data, c.size);
        s.shaders.BindObject(c.object, entry);
        break;
      }

      case Command::kSwap:
      {
        SetGuestOverlays(nullptr, 0);
        s.renderer.EndFrame();
        const uint64_t frame = s.renderer.frames();
        const int every = REXCVAR_GET(sst_native_log_every);

        if (frame == 1 || (every > 0 && frame % uint64_t(every) == 0))
        {
          s.renderer.LogStats(frame);
          s.shaders.LogStats("so far");
        }
        break;
      }
      }

      SetGuestOverlays(nullptr, 0);
    }

    void RenderThreadMain(State *s)
    {
      for (;;)
      {
        Batch batch;
        {
          std::unique_lock lock(s->queue_mutex);
          s->queue_cv.wait(lock, [&]
                           { return s->stop || !s->queue.empty(); });

          if (s->queue.empty())
            return; // stopping
          batch = std::move(s->queue.front());
          s->queue.pop_front();
        }
        {
          std::lock_guard lock(s->mutex);

          const bool active = g_active.load(std::memory_order_acquire);

          for (const Command &c : batch.commands)
          {
            ApplyDevice(*s, c);

            if (active)
              Execute(*s, c);
          }
        }

        if (batch.arena)
        {
          batch.arena->Reset();
          std::lock_guard lock(s->queue_mutex);
          s->free_arenas.push_back(batch.arena);
          s->frames_done = batch.frame;
          s->done_cv.notify_all();
        }
      }
    }

    // capture (game threads, under capture_mutex)

    class Capture
    {
    public:
      explicit Capture(State &s) : s_(s), lock_(s.capture_mutex), start_(Clock::now()) {}
      ~Capture()
      {
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
        s_.capture_ms += ms;
        s_.frame_capture_ms += ms;
      }

      Command &New(Command::Kind kind)
      {
        cmd_ = Command();
        cmd_.kind = kind;
        return cmd_;
      }
      // Copies guest [addr, addr + size) into the frame's arena and overlays it
      void Range(uint32_t addr, uint32_t size)
      {
        if (!size || size > (64u << 20) || !Plausible(addr) || cmd_.overlay_count >= 8)
          return;

        uint8_t *copy = s_.arena->Alloc(size);
        std::memcpy(copy, HostPtr(s_.base, addr), size);
        Overlay(addr, size, copy);
      }

      void Overlay(uint32_t addr, uint32_t size, const uint8_t *data)
      {
        if (cmd_.overlay_count >= 8)
          return;

        cmd_.overlays[cmd_.overlay_count++] = {addr, addr + size, data};
        s_.snapshot_bytes += size;
      }
      // The device state block, plus the bound vertex shader's live microcode
      void Device(uint32_t dev)
      {
        if (!Plausible(dev))
          return;

        DeviceDelta(dev);
        const uint32_t vs = Be32(dev + 0x3198);

        if (!Plausible(vs))
          return;
        // Microcode pointer (D3D may rebind the object to a differently patched copy) and the live microcode: the object keeps a copy of the container header at +0x368 (+8: physical part size)
        Range(vs + 0x20, 4);
        const uint32_t live = ToVirtual(Be32(vs + 0x20));
        const uint32_t size = Be32(vs + 0x368 + 8);

        if (!Plausible(live) || !size || size > 0x10000)
        {
          ++s_.ucode_skipped;
          return;
        }

        const uint8_t *host = HostPtr(s_.base, live);
        auto it = s_.ucode_copies.find(live);

        if (it != s_.ucode_copies.end() && it->second.size == size && std::memcmp(it->second.data, host, size) == 0)
        {
          Overlay(live, size, it->second.data);
          return;
        }

        uint8_t *copy = s_.arena->Alloc(size);
        std::memcpy(copy, host, size);
        s_.ucode_copies[live] = {size, copy};
        Overlay(live, size, copy);
      }

      void RefreshWatch()
      {
        if (const uint64_t v = guest_watch::Version(); v != s_.watch_version)
        {
          guest_watch::Copy(s_.watch);
          s_.watch_version = v;
        }
      }

      // Vertices the draw reads from the start of each vertex buffer (the stream offset is part of the fetch address): start + count,
      // or the highest index + base vertex. 0 when the draw is too big to check cheaply.
      uint32_t DrawVertices(uint32_t dev, const DrawCall &call)
      {
        constexpr uint32_t kMaxIndices = 4096;

        if (!call.indexed)
          return call.count <= kMaxIndices ? call.start + call.count : 0;

        if (call.count > kMaxIndices)
          return 0;

        const uint32_t ib = Be32(dev + kDevIndices);

        if (!Plausible(ib))
          return 0;

        const uint32_t common = Be32(ib);
        const bool index32 = (common >> 31) & 1;
        const uint32_t endian = (common >> 29) & 3;
        const uint32_t ib_addr = ToVirtual(Be32(ib + 0x18) & ~3u);
        const uint32_t isz = index32 ? 4 : 2;

        if (!Plausible(ib_addr) || !GuestReadable(ib_addr + call.start * isz, call.count * isz))
          return 0;

        // As the renderer decodes them: whole dwords around the range, swapped by the index buffer's swap mode, read little-endian
        const uint32_t first_byte = call.start * isz;
        const uint32_t aligned_begin = first_byte & ~3u;
        const uint32_t aligned_end = (first_byte + call.count * isz + 3) & ~3u;
        const uint8_t *src = HostPtr(s_.base, ib_addr + aligned_begin);
        uint32_t words[kMaxIndices + 2];
        const uint32_t nwords = std::min<uint32_t>((aligned_end - aligned_begin) / 4, uint32_t(std::size(words)));

        for (uint32_t k = 0; k < nwords; ++k)
        {
          uint32_t x;
          std::memcpy(&x, src + 4 * k, 4);
          words[k] = endian == 1 ? ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu) : endian == 2 ? __builtin_bswap32(x)
                                                                                     : endian == 3   ? (x << 16) | (x >> 16)
                                                                                                     : x;
        }

        const uint8_t *idx = reinterpret_cast<const uint8_t *>(words) + (first_byte - aligned_begin);
        uint32_t max_index = 0;

        for (uint32_t k = 0; k < call.count; ++k)
        {
          uint32_t v;

          if (index32)
          {
            std::memcpy(&v, idx + 4 * k, 4);

            if (v == 0xFFFFFFFFu)
              continue; // strip restart
          }
          else
          {
            uint16_t h;
            std::memcpy(&h, idx + 2 * k, 2);

            if (h == 0xFFFFu)
              continue;

            v = h;
          }

          max_index = std::max(max_index, v);
        }

        const uint64_t vertices = uint64_t(call.base_vertex) + max_index + 1;
        return vertices <= 16 * kMaxIndices ? uint32_t(vertices) : 0;
      }

      // Textures the draw samples that the game rewrites: copied now (once per frame each), because the game may have written the next
      // frame's data by the time the render thread gets to this draw. Textures seen changing: always. Any known texture: for UI-sized
      // draws (portraits, previews and text pages the game reloads as screens change), within a per-frame budget.
      // A draw that tests depth (anything but ALWAYS) is part of the 3D scene, not UI, however few vertices it has. Small 3D draws
      // (court details, effects) read static data; copying their textures and vertices filled the per-frame budget in matches
      // (16 MB of textures + 12 MB of vertices a frame), slow on low-end CPUs and leaving the real HUD's textures uncopied.
      bool DepthTested(uint32_t dev)
      {
        const uint32_t dc = Be32(dev + kDevDepthControl);
        return (dc & 2) && ((dc >> 4) & 7) != 7;
      }

      void Textures(uint32_t dev, bool ui)
      {
        if (!Plausible(dev))
          return;

        RefreshWatch();

        if (s_.watch.textures.empty())
          return;

        constexpr uint64_t kFrameBudget = 16ull << 20;
        TextureSnap snaps[64];
        uint32_t n = 0;

        auto add = [&](uint32_t addr, uint32_t size)
        {
          if (!addr || !size || uint64_t(addr) + size > 0x20000000u || n >= 64 || !GuestPhysicalReadable(addr, size))
            return;

          uint8_t *copy = s_.arena->Alloc(size);
          std::memcpy(copy, HostPtr(s_.base, addr | 0xA0000000u), size);
          snaps[n++] = {addr, addr + size, copy};
          s_.texture_bytes += size;
          s_.frame_texture_bytes += size;
        };

        for (uint32_t i = 0; i < 32; ++i)
        {
          const uint32_t a = dev + kDevFetchConstants + 24 * i;

          if ((Be32(a) & 3) != 2)
            continue;

          const uint32_t page = (Be32(a + 4) >> 12) & 0x1FFFF;
          auto it = s_.watch.textures.find(page);

          if (it == s_.watch.textures.end())
            continue;

          const guest_watch::TextureRange &r = it->second;

          if (!r.changing && (!ui || s_.frame_texture_bytes + r.base_size + r.mip_size > kFrameBudget))
            continue;

          if (!s_.textures_copied.insert(page).second)
            continue;

          add(r.base_addr, r.base_size);
          add(r.mip_addr, r.mip_size);
        }

        if (!n)
          return;

        auto *list = reinterpret_cast<TextureSnap *>(s_.arena->Alloc(sizeof(TextureSnap) * n));
        std::copy(snaps, snaps + n, list);
        cmd_.tex = list;
        cmd_.tex_count = n;
      }

      // Vertex data the draw reads that the game rewrites between draws (UI sprites and text: one buffer reused for the next sprite, or
      // data seen changing between frames): copied with every draw, as much as it reads. Static model data is read live.
      void Vertices(uint32_t dev, uint32_t vertices, bool ui)
      {
        if (!Plausible(dev))
          return;

        RefreshWatch();
        constexpr uint32_t kMaxStride = 64, kUiVertices = 256, kMaxBuffer = 1u << 20, kMaxPerDraw = 2u << 20;
        Renderer::GuestCopy copies[16];
        uint32_t n = 0, total = 0;

        for (uint32_t slot = 0; slot < 96 && n < 16; ++slot)
        {
          const uint32_t dw0 = Be32(dev + kDevFetchConstants + 8 * slot);

          if ((dw0 & 3) != 3)
            continue;

          const uint32_t phys = dw0 & 0x1FFFFFFCu;

          if (!phys)
            continue;

          uint32_t want = 0;

          if (auto it = s_.watch.vertices.find(phys); it != s_.watch.vertices.end())
            want = std::max(it->second, vertices * kMaxStride);
          else if (ui && vertices && vertices <= kUiVertices)
            want = vertices * kMaxStride;

          const uint32_t size = std::min({Be32(dev + kDevFetchConstants + 8 * slot + 4) & 0x03FFFFFCu, want, kMaxBuffer});

          if (!size || total + size > kMaxPerDraw)
            continue;

          bool dup = false;

          for (uint32_t i = 0; i < n && !dup; ++i)
            dup = copies[i].begin == phys && copies[i].end - copies[i].begin >= size;

          if (dup || !GuestPhysicalReadable(phys, size))
            continue;

          uint8_t *copy = s_.arena->Alloc(size);
          std::memcpy(copy, HostPtr(s_.base, phys | 0xA0000000u), size);
          copies[n++] = {phys, phys + size, copy};
          total += size;
        }

        s_.vertex_bytes += total;

        if (!n)
          return;

        auto *list = reinterpret_cast<Renderer::GuestCopy *>(s_.arena->Alloc(sizeof(Renderer::GuestCopy) * n));
        std::copy(copies, copies + n, list);
        cmd_.verts = list;
        cmd_.vert_count = n;
      }

      // The device block's chunks that changed since the last capture
      void DeviceDelta(uint32_t dev)
      {
        const uint8_t *host = HostPtr(s_.base, dev + kDevSnapBegin);
        uint8_t *snap = s_.dev_snap.get();
        uint64_t mask = 0;
        uint32_t bytes = 0;

        if (s_.dev_snap_addr != dev)
        {
          mask = kDevChunks == 64 ? ~uint64_t(0) : (uint64_t(1) << kDevChunks) - 1;
          bytes = kDevSnapSize;
          s_.dev_snap_addr = dev;
        }
        else
        {
          for (uint32_t i = 0; i < kDevChunks; ++i)
            if (std::memcmp(snap + i * kDevChunk, host + i * kDevChunk, ChunkSize(i)) != 0)
            {
              mask |= uint64_t(1) << i;
              bytes += ChunkSize(i);
            }
        }

        cmd_.dev_addr = dev;
        cmd_.dev_mask = mask;
        s_.dev_chunks_total += kDevChunks;

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
          ++s_.dev_chunks_copied;
        }

        cmd_.dev_delta = delta;
        s_.snapshot_bytes += bytes;
      }

      const uint8_t *Bytes(const void *data, size_t size)
      {
        uint8_t *copy = s_.arena->Alloc(size);
        std::memcpy(copy, data, size);
        return copy;
      }

      void Submit()
      {
        s_.batch.commands.push_back(cmd_);
        ++s_.captured;

        if (s_.batch.commands.size() >= kBatchCommands)
          Flush();
      }

      void Flush()
      {
        if (s_.batch.commands.empty() && !s_.batch.arena)
          return;

        {
          std::lock_guard lock(s_.queue_mutex);
          s_.queue.push_back(std::move(s_.batch));
        }

        s_.queue_cv.notify_one();
        s_.batch = Batch();
        s_.batch.commands.reserve(kBatchCommands);
      }
      // Ends the frame: hands its arena over and waits until the render thread is at most one frame behind, with an arena free for the next frame
      void EndFrame()
      {
        s_.capture_max_ms = std::max(s_.capture_max_ms, s_.frame_capture_ms);
        s_.frame_capture_ms = 0;
        s_.batch.arena = s_.arena;
        s_.batch.frame = ++s_.frame;
        Flush();
        s_.ucode_copies.clear();
        s_.textures_copied.clear();
        s_.frame_texture_bytes = 0;
        const auto wait_start = Clock::now();
        {
          std::unique_lock lock(s_.queue_mutex);
          s_.done_cv.wait(lock, [&]
                          { return s_.stop || (!s_.free_arenas.empty() && s_.frames_done + 1 >= s_.frame); });

          if (s_.free_arenas.empty())
          { // stopping
            s_.arenas.push_back(std::make_unique<Arena>());
            s_.free_arenas.push_back(s_.arenas.back().get());
          }

          s_.arena = s_.free_arenas.back();
          s_.free_arenas.pop_back();
        }

        const auto waited = Clock::now() - wait_start;
        s_.wait_ms += std::chrono::duration<double, std::milli>(waited).count();
        start_ += waited; // not capture time
        ++s_.period_frames;
      }

    private:
      using Clock = std::chrono::steady_clock;
      uint32_t Be32(uint32_t addr) const
      {
        uint32_t v;
        std::memcpy(&v, HostPtr(s_.base, addr), 4);
        return __builtin_bswap32(v);
      }

      State &s_;
      std::lock_guard<std::mutex> lock_;
      Clock::time_point start_;
      Command cmd_;
    };

    // Hands a command to the render thread (with snapshots), or runs it now. fill(Command&) sets the call's arguments; snap(Capture&, Command&) adds the snapshots (threaded only)
    template <typename Fill, typename Snap>
    void Issue(const uint8_t *base, Command::Kind kind, Fill &&fill, Snap &&snap)
    {
      State &s = *g_state;

      if (!s.threaded)
      {
        auto lock = Enter();

        if (!lock)
          return;

        s.base = base;
        Command c;
        c.kind = kind;
        fill(c);
        Execute(s, c);
        return;
      }

      if (!g_active.load(std::memory_order_acquire))
        return;

      Capture cap(s);

      if (!s.base)
        s.base = base;

      Command &c = cap.New(kind);
      fill(c);
      snap(cap, c);
      cap.Submit();
    }

    void StartRenderThread(State &s)
    {
      s.threaded = true;
      s.dev_snap_addr = s.rt_dev_addr = 0; // the first capture sends the whole device block
      s.renderer.SetThreaded(true);

      for (int i = 0; i < 3; ++i)
      {
        s.arenas.push_back(std::make_unique<Arena>());
        // A frame snapshots ~15-20 MB: allocate (and touch) it up front so the game thread doesn't page-fault fresh chunks mid-match
        s.arenas.back()->Alloc(size_t(32) << 20);
        s.arenas.back()->Reset();
        s.free_arenas.push_back(s.arenas.back().get());
      }

      s.arena = s.free_arenas.back();
      s.free_arenas.pop_back();
      s.batch.commands.reserve(kBatchCommands);
      s.thread = std::thread(RenderThreadMain, &s);
    }

    void StopRenderThread(State &s)
    {
      if (!s.thread.joinable())
        return;

      {
        std::lock_guard lock(s.queue_mutex);
        s.stop = true;
        s.queue.clear();
      }

      s.queue_cv.notify_all();
      s.done_cv.notify_all();
      s.thread.join();
    }
  }

  // lifetime

  void Initialize(rex::Runtime *runtime)
  {
    HMODULE plugin = FindPlugin();

    if (!plugin)
      return; // another GPU plugin (xenos): stay inactive

    auto set_source = reinterpret_cast<SstSetFrameSourceFn>(reinterpret_cast<void *>(GetProcAddress(plugin, SST_SET_FRAME_SOURCE_EXPORT)));
    auto *gs = runtime ? runtime->graphics_system() : nullptr;
    auto *provider = gs ? static_cast<const D3D12Provider *>(gs->provider()) : nullptr;

    if (!set_source || !provider || !provider->GetDevice())
    {
      REXLOG_ERROR("[sst-native] sst GPU plugin is loaded but {} - native renderer disabled", !set_source ? "has no frame-source export (old build?)" : "has no D3D12 device");
      return;
    }

    InstallPresentHook(*provider);
    {
      // First boot: the VIDEO page's quality preset from the GPU's memory
      DXGI_ADAPTER_DESC1 ad = {};
      Microsoft::WRL::ComPtr<IDXGIFactory4> factory4;
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;

      if (provider->GetDXGIFactory() && SUCCEEDED(provider->GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&factory4))) && SUCCEEDED(factory4->EnumAdapterByLuid(provider->GetDevice()->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) && SUCCEEDED(adapter->GetDesc1(&ad)))
      {
        char name[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        sst::menu::OnGpuDetected(uint64_t(ad.DedicatedVideoMemory) >> 20, name);
      }
    }

    auto state = std::make_unique<State>();
    state->set_frame_source = set_source;
    state->shaders.Initialize(*provider, rex::filesystem::GetExecutableFolder() / "cache" / "shaders");
    // The game's frame: 1280x720, or 1280x1024 in its own 4:3 mode
    const uint32_t frame_h = sst::menu::NativeFourThree() ? 1024 : 720;

    if (!state->renderer.Initialize(*provider, &state->shaders, runtime->memory()->physical_membase(), 1280, frame_h))
    {
      REXLOG_ERROR("[sst-native] renderer setup failed - native renderer disabled");
      state->shaders.Shutdown();
      return;
    }

    const SstFrameSource source = {sizeof(SstFrameSource), &state->renderer, AcquireThunk, ReleaseThunk, DisplayAspectThunk};
    set_source(&source);

    const bool threaded = REXCVAR_GET(sst_native_render_thread);

    if (threaded)
      StartRenderThread(*state);

    g_state = state.release();
    g_active.store(true, std::memory_order_release);
    REXLOG_INFO("[sst-native] native renderer active (1280x{}), {}", frame_h, threaded ? "draws recorded on a separate render thread" : "draws recorded on the game thread");
  }

  void Shutdown()
  {
    if (!g_active.load())
      return;

    State *s = g_state;
    g_active.store(false); // hooks stop issuing commands
    hitch::Shutdown();
    StopRenderThread(*s);
    std::lock_guard lock(s->mutex); // no hook is inside the renderer now
    s->set_frame_source(nullptr);   // waits for a copy in progress in the plugin
    s->shaders.Shutdown();
    s->renderer.Shutdown();
    // g_state is left allocated: a late hook call only sees g_active == false
  }

  bool Active()
  {
    return g_active.load(std::memory_order_acquire);
  }

  // hooks

  void OnCreateShader(const uint8_t *base, uint32_t container, uint32_t object)
  {
    if (!Active())
      return;

    const uint32_t flags = be32(base, container);

    if ((flags & 0xFFFFFF00u) != 0x102A1100u)
      return;

    const uint32_t size = be32(base, container + 4) + be32(base, container + 8);

    if (size == 0 || size > (1u << 20))
      return;

    const uint8_t *host = base + container + rex::memory::detail::PhysicalHostOffset(container);
    State &s = *g_state;

    if (!s.threaded)
    {
      auto lock = Enter();

      if (!lock)
        return;

      ShaderEntry *entry = s.shaders.Register(host, size);
      s.shaders.BindObject(object, entry);
      return;
    }
    // The container may be a temporary buffer: the command carries a copy
    Capture cap(s);

    if (!s.base)
      s.base = base;

    Command &c = cap.New(Command::kShader);
    c.data = cap.Bytes(host, size);
    c.size = size;
    c.object = object;
    cap.Submit();
  }

  void OnSetStreamSource(const HookArgs &)
  {
    // Streams are read from the device's fetch constants at draw time
  }

  namespace
  {
    void IssueDraw(const uint8_t *base, const DrawCall &call)
    {
      Issue(
          base, Command::kDraw, [&](Command &c)
          { c.draw = call; },
          [&](Capture &cap, Command &)
          {
            cap.Device(call.device);
            const uint32_t vertices = call.up_data ? 0 : cap.DrawVertices(call.device, call);
            const bool ui = !cap.DepthTested(call.device);

            if (REXCVAR_GET(sst_native_draw_copies))
            {
              cap.Textures(call.device, ui && (call.up_data ? call.count <= 256 : vertices && vertices <= 256));

              if (!call.up_data)
                cap.Vertices(call.device, vertices, ui);
            }

            if (call.up_data)
              cap.Range(call.up_data, call.up_stride * (call.start + call.count));
          });
    }
  }

  void OnDrawIndexed(const uint8_t *base, const HookArgs &a)
  {
    if (!Active())
      return;

    DrawCall call;
    call.device = a.r3;
    call.prim = a.r4;
    call.indexed = true;
    call.base_vertex = a.r5;
    call.start = a.r6;
    call.count = a.r7;
    IssueDraw(base, call);
  }

  void OnDraw(const uint8_t *base, const HookArgs &a)
  {
    if (!Active())
      return;

    DrawCall call;
    call.device = a.r3;
    call.prim = a.r4;
    call.start = a.r5;
    call.count = a.r6;
    IssueDraw(base, call);
  }

  void OnDrawUP(const uint8_t *base, const HookArgs &a)
  {
    if (!Active())
      return;

    if (!a.r6 || !a.r7 || a.r7 > 256 || !a.r5 || a.r5 > (1u << 20))
      return;

    DrawCall call;
    call.device = a.r3;
    call.prim = a.r4;
    call.count = a.r5;
    call.up_data = a.r6;
    call.up_stride = a.r7;
    IssueDraw(base, call);
  }

  namespace
  {
    struct PendingVertices
    {
      uint32_t device = 0, prim = 0, count = 0, stride = 0, data = 0;
    };

    thread_local PendingVertices t_pending_vertices;
  }

  void OnBeginVertices(const HookArgs &a, uint32_t data)
  {
    if (!Active())
      return;

    t_pending_vertices = {a.r3, a.r4, a.r5, a.r6, data};
  }

  void OnDrawUPDone()
  {
    t_pending_vertices = {};
  }

  void OnEndVertices(const uint8_t *base, const HookArgs &a)
  {
    const PendingVertices p = t_pending_vertices;
    t_pending_vertices = {};

    if (!Active())
      return;

    if (!p.data || p.device != a.r3 || !p.count || p.count > (1u << 20) || !p.stride || p.stride > 256)
      return;

    static uint32_t logged_kinds[16];
    static uint32_t logged = 0;
    const uint32_t kind = p.prim | p.stride << 8;
    bool seen = false;

    for (uint32_t i = 0; i < logged; ++i)
      seen |= logged_kinds[i] == kind;

    if (!seen && logged < 16)
    {
      logged_kinds[logged++] = kind;
      REXLOG_INFO("[sst-native] immediate-mode draw (BeginVertices/EndVertices): prim {} vertices {} "
                  "stride {}",
                  p.prim, p.count, p.stride);
    }

    DrawCall call;
    call.device = p.device;
    call.prim = p.prim;
    call.count = p.count;
    call.up_data = p.data;
    call.up_stride = p.stride;
    call.immediate = true;
    IssueDraw(base, call);
  }

  void OnClear(const uint8_t *base, const HookArgs &a)
  {
    if (!Active())
      return;

    Issue(
        base, Command::kClear,
        [&](Command &c)
        {
          c.device = a.r3;
          c.flags = a.r6;
          c.color = a.r7;
          c.z = float(a.f1);
          c.stencil = a.r9; // stencil: r9 (z took f1)
        },
        [&](Capture &cap, Command &)
        { cap.Device(a.r3); });
  }

  void OnResolve(const uint8_t *base, const HookArgs &a)
  {
    if (!Active())
      return;

    ResolveCall call;
    call.device = a.r3;
    call.flags = a.r4;
    call.src_rect = a.r5;
    call.dst_texture = a.r6;
    call.dst_point = a.r7;
    call.level = a.r8;
    call.slice = a.r9;
    call.clear_color = a.r10;
    call.clear_z = float(a.f1);
    Issue(
        base, Command::kResolve, [&](Command &c)
        { c.resolve = call; },
        [&](Capture &cap, Command &)
        {
          cap.Device(call.device);
          cap.Range(call.src_rect, 16);
          cap.Range(call.dst_point, 8);
          cap.Range(call.clear_color, 16);

          if (Plausible(call.dst_texture))
            cap.Range(call.dst_texture + 0x1C, 24);
        });
  }

  namespace
  {
    void IssueEvent(const uint8_t *base, uint32_t device, std::string text)
    {
      State &s = *g_state;

      if (!s.threaded)
      {
        auto lock = Enter();

        if (!lock)
          return;

        s.renderer.TraceEvent(base, device, text);
        return;
      }

      Capture cap(s);

      if (!s.base)
        s.base = base;

      Command &c = cap.New(Command::kEvent);
      c.device = device;
      c.data = cap.Bytes(text.data(), text.size());
      c.size = uint32_t(text.size());
      cap.Device(device);
      cap.Submit();
    }
  }

  void OnResolveVariant(const uint8_t *base, const HookArgs &a)
  {
    if (!Active())
      return;

    IssueEvent(base, a.r3, fmt::format("resolve variant sub_82311120 r3-r10 {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
                                       "{:08X} f1 {}",
                                       a.r3, a.r4, a.r5, a.r6, a.r7, a.r8, a.r9, a.r10, a.f1));
  }

  void OnSetPredication(const uint8_t *base, const HookArgs &a)
  {
    if (!Active())
      return;

    IssueEvent(base, a.r3, fmt::format("SetPredication {:08X}", a.r4));
  }

  void OnSwap()
  {
    if (!Active())
      return;

    hitch::OnGameFrame();
    State &s = *g_state;

    if (!s.threaded)
    {
      auto lock = Enter();

      if (!lock)
        return;

      Command c;
      c.kind = Command::kSwap;
      Execute(s, c);
      return;
    }

    Capture cap(s);

    if (!s.game_thread_boosted)
    {
      s.game_thread_boosted = true;

      if (REXCVAR_GET(sst_native_game_thread_priority))
      {
        const BOOL ok = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        REXLOG_INFO("[sst-native] game rendering thread priority raised to above normal{}", ok ? "" : " (failed)");
      }
    }
    {
      const auto t = std::chrono::steady_clock::now();

      if (s.last_swap.time_since_epoch().count())
      {
        const double ms = std::chrono::duration<double, std::milli>(t - s.last_swap).count();
        s.swap_max_ms = std::max(s.swap_max_ms, ms);
        s.swaps_over_20 += ms > 20.0;
      }

      s.last_swap = t;
    }

    cap.New(Command::kSwap);
    cap.Submit();
    cap.EndFrame();
    // Game-thread cost of the hooks, every sst_native_log_every frames
    const int every = REXCVAR_GET(sst_native_log_every);

    if (every > 0 && s.period_frames >= uint64_t(every))
    {
      const double frames = double(s.period_frames);
      REXLOG_INFO("[sst-native] perf: game thread per frame: {:.2f} ms capturing {} calls ({:.1f} MB "
                  "of snapshots), {:.2f} ms waiting for the render thread; game frames over 20 ms: "
                  "{} (longest {:.1f} ms); longest capture in one frame {:.2f} ms; device block chunks "
                  "copied {:.1f}%; textures copied {:.2f} MB ({} known); vertex data copied {:.2f} MB ({} buffers watched){}",
                  s.capture_ms / frames, uint64_t(double(s.captured) / frames + 0.5), double(s.snapshot_bytes) / frames / (1 << 20), s.wait_ms / frames, s.swaps_over_20, s.swap_max_ms, s.capture_max_ms,
                  s.dev_chunks_total ? 100.0 * double(s.dev_chunks_copied) / double(s.dev_chunks_total) : 0.0,
                  double(s.texture_bytes) / frames / (1 << 20), s.watch.textures.size(), double(s.vertex_bytes) / frames / (1 << 20), s.watch.vertices.size(),
                  s.ucode_skipped ? fmt::format("; {} draws without a microcode snapshot", s.ucode_skipped) : std::string());
      s.dev_chunks_copied = s.dev_chunks_total = 0;
      s.capture_ms = s.wait_ms = 0;
      s.captured = s.snapshot_bytes = s.period_frames = s.ucode_skipped = 0;
      s.texture_bytes = s.vertex_bytes = 0;
      s.swap_max_ms = 0;
      s.swaps_over_20 = 0;
      s.capture_max_ms = 0;
    }
  }
}