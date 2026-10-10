// sst_rematch - native D3D12 renderer (exe side)

#include "gpu/native/native.h"

#include "gpu/frame_source.h"
#include "gpu/native/renderer.h"
#include "gpu/native/shader_cache.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xmemory.h>
#include <rex/ui/d3d12/d3d12_provider.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>

REXCVAR_DEFINE_INT32(sst_native_log_every, 600, "SST", "Native renderer: print a status line every N frames (0 = never)");

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

    struct State
    {
      std::mutex mutex; // serializes hook entry points against Shutdown
      SstSetFrameSourceFn set_frame_source = nullptr;
      ShaderCache shaders;
      Renderer renderer;
    };

    State *g_state = nullptr;
    std::atomic<bool> g_active{false};

    ID3D12Resource *AcquireThunk(void *user, uint32_t *slot, uint32_t *width, uint32_t *height)
    {
      return static_cast<Renderer *>(user)->Acquire(slot, width, height);
    }
    void ReleaseThunk(void *user, uint32_t slot) { static_cast<Renderer *>(user)->Release(slot); }

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

  // lifetime
  void Initialize(rex::Runtime *runtime)
  {
    HMODULE plugin = FindPlugin();
    if (!plugin)
      return;

    auto set_source = reinterpret_cast<SstSetFrameSourceFn>(
        reinterpret_cast<void *>(GetProcAddress(plugin, SST_SET_FRAME_SOURCE_EXPORT)));
    auto *gs = runtime ? runtime->graphics_system() : nullptr;
    auto *provider = gs ? static_cast<const D3D12Provider *>(gs->provider()) : nullptr;
    if (!set_source || !provider || !provider->GetDevice())
    {
      REXLOG_ERROR("[sst-native] sst GPU plugin is loaded but {} - native renderer disabled",
                   !set_source ? "has no frame-source export (old build?)" : "has no D3D12 device");
      return;
    }

    auto state = std::make_unique<State>();
    state->set_frame_source = set_source;
    state->shaders.Initialize(*provider,
                              rex::filesystem::GetExecutableFolder() / "cache" / "shaders");
    if (!state->renderer.Initialize(*provider, &state->shaders,
                                    runtime->memory()->physical_membase(), 1280, 720))
    {
      REXLOG_ERROR("[sst-native] renderer setup failed - native renderer disabled");
      state->shaders.Shutdown();
      return;
    }

    const SstFrameSource source = {sizeof(SstFrameSource), &state->renderer, AcquireThunk,
                                   ReleaseThunk};
    set_source(&source);

    g_state = state.release();
    g_active.store(true, std::memory_order_release);
    REXLOG_INFO("[sst-native] native renderer active (1280x720)");
  }

  void Shutdown()
  {
    if (!g_active.load())
      return;
    State *s = g_state;
    std::lock_guard lock(s->mutex);
    g_active.store(false);
    s->set_frame_source(nullptr); // waits for a copy in progress in the plugin
    s->shaders.Shutdown();
    s->renderer.Shutdown();
  }

  bool Active() { return g_active.load(std::memory_order_acquire); }

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

    auto lock = Enter();
    if (!lock)
      return;
    const uint8_t *host = base + container + rex::memory::detail::PhysicalHostOffset(container);
    ShaderEntry *entry = g_state->shaders.Register(host, size);
    g_state->shaders.BindObject(object, entry);
  }

  void OnSetStreamSource(const HookArgs &)
  {
    // Streams are read from the device's fetch constants at draw time
  }

  void OnDrawIndexed(const uint8_t *base, const HookArgs &a)
  {
    auto lock = Enter();
    if (!lock)
      return;
    DrawCall call;
    call.device = a.r3;
    call.prim = a.r4;
    call.indexed = true;
    call.base_vertex = a.r5;
    call.start = a.r6;
    call.count = a.r7;
    g_state->renderer.Draw(base, call);
  }

  void OnDraw(const uint8_t *base, const HookArgs &a)
  {
    auto lock = Enter();
    if (!lock)
      return;
    DrawCall call;
    call.device = a.r3;
    call.prim = a.r4;
    call.start = a.r5;
    call.count = a.r6;
    g_state->renderer.Draw(base, call);
  }

  void OnDrawUP(const uint8_t *base, const HookArgs &a)
  {
    auto lock = Enter();
    if (!lock)
      return;
    if (!a.r6 || !a.r7 || a.r7 > 256)
      return;
    DrawCall call;
    call.device = a.r3;
    call.prim = a.r4;
    call.count = a.r5;
    call.up_data = a.r6;
    call.up_stride = a.r7;
    g_state->renderer.Draw(base, call);
  }

  void OnClear(const uint8_t *base, const HookArgs &a)
  {
    auto lock = Enter();
    if (!lock)
      return;
    g_state->renderer.Clear(base, a.r3, a.r6, a.r7, float(a.f1), a.r8);
  }

  void OnSwap()
  {
    auto lock = Enter();
    if (!lock)
      return;
    State &s = *g_state;
    s.renderer.EndFrame();

    const uint64_t frame = s.renderer.frames();
    const int every = REXCVAR_GET(sst_native_log_every);
    if (frame == 1 || (every > 0 && frame % uint64_t(every) == 0))
    {
      s.renderer.LogStats(frame);
      s.shaders.LogStats("so far");
    }
  }

}