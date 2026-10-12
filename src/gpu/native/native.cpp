// sst_rematch - native D3D12 renderer (exe side)

#include "gpu/native/native.h"

#include "gpu/frame_source.h"
#include "gpu/native/hitch_profiler.h"
#include "gpu/native/present_hook.h"
#include "menu/graphics_menu.h"
#include "gpu/native/renderer.h"
#include "gpu/native/shader_cache.h"

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
      const auto &a = kAspectXY[std::clamp(REXCVAR_GET(sst_aspect), 0, 4)];
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
    void Execute(State &s, const Command &c)
    {
      SetGuestOverlays(c.overlays, c.overlay_count);
      const uint8_t *base = s.base;

      switch (c.kind)
      {
      case Command::kDraw:
        s.renderer.Draw(base, c.draw);
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

          if (g_active.load(std::memory_order_acquire))
            for (const Command &c : batch.commands)
              Execute(*s, c);
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

        Range(dev + kDevSnapBegin, kDevSnapEnd - kDevSnapBegin);
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

    if (!state->renderer.Initialize(*provider, &state->shaders, runtime->memory()->physical_membase(), 1280, 720))
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
    REXLOG_INFO("[sst-native] native renderer active (1280x720), {}", threaded ? "draws recorded on a separate render thread" : "draws recorded on the game thread");
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

    IssueEvent(base, a.r3,
               fmt::format("resolve variant sub_82311120 r3-r10 {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
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
                  "{} (longest {:.1f} ms); longest capture in one frame {:.2f} ms{}",
                  s.capture_ms / frames, uint64_t(double(s.captured) / frames + 0.5), double(s.snapshot_bytes) / frames / (1 << 20), s.wait_ms / frames, s.swaps_over_20, s.swap_max_ms, s.capture_max_ms, s.ucode_skipped ? fmt::format("; {} draws without a microcode snapshot", s.ucode_skipped) : std::string());
      s.capture_ms = s.wait_ms = 0;
      s.captured = s.snapshot_bytes = s.period_frames = s.ucode_skipped = 0;
      s.swap_max_ms = 0;
      s.swaps_over_20 = 0;
      s.capture_max_ms = 0;
    }
  }
}