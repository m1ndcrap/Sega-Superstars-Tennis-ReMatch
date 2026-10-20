// Native renderer: startup / shutdown and the D3D hooks' entry points. Each hook turns the game's call into a Command. With the render thread (the default) the Command is captured with copies of the guest memory it reads (capture.h) and queued; the render thread runs it on the Renderer
// (render_thread.cpp). Without it, the Command runs right away on the game thread.

#include "gpu/native/native.h"

#include "gpu/native/capture.h"
#if defined(_WIN32)
#include "gpu/native/gfx/d3d12/d3d12_gfx.h"
#endif
#include "gpu/native/hitch_profiler.h"
#include "gpu/native/present_hook.h"
#include "gpu/native/write_watch.h"
#include "menu/graphics_menu.h"

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xmemory.h>
#if defined(_WIN32)
#include <rex/ui/d3d12/d3d12_provider.h>
#endif
#if REX_HAS_VULKAN
#include <rex/ui/vulkan/provider.h>

#include "gpu/native/gfx/vulkan/vulkan_gfx.h"
#endif

#if defined(_WIN32)
#include <dxgi1_4.h>
#include <wrl/client.h>
#else
#include <dlfcn.h>
#endif

#include <string>

REXCVAR_DECLARE(int32_t, sst_aspect);

REXCVAR_DEFINE_STRING(sst_gpu_api, "d3d12", "SST", "Graphics API: d3d12 or vulkan (read at startup by the sst GPU plugin; Vulkan needs an SDK built with REXGLUE_USE_VULKAN)").lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sst_native_log_every, 600, "SST", "Native renderer: print a status line every N frames (0 = never)");
REXCVAR_DEFINE_BOOL(sst_native_game_thread_priority, true, "SST", "Native renderer: raise the game's rendering thread (the one calling Swap) to above-normal priority, so background work (morphs, shader / pipeline compiles) never delays its frame");
REXCVAR_DEFINE_BOOL(sst_native_write_watch, true, "SST", "Native renderer: watch the game's writes to vertex buffers that many 3D draws read, so an unchanged buffer's copy is reused without comparing it (false = compare every time)");
REXCVAR_DEFINE_BOOL(sst_native_draw_copies, true, "SST", "Native renderer: copy the textures / vertex data of UI draws when the game issues them, for the render thread (false = the render thread reads guest memory as it is when it gets to the draw; for testing)");
REXCVAR_DEFINE_BOOL(sst_native_render_thread, true, "SST", "Native renderer: record and submit the game's draws on a separate render thread (the game thread only snapshots each call's state). false = do all renderer work on the game thread, as before");

namespace sst::native
{
  using namespace capture;

#if defined(_WIN32)
  using rex::ui::d3d12::D3D12Provider;
#endif

  namespace
  {
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

      *x = kAspectXY[aspect][0];
      *y = kAspectXY[aspect][1];
    }

    // The frame for the plugin: an ID3D12Resource, or (Vulkan) a VkImage handle (see gpu/frame_source.h)
    bool g_vulkan = false;

    void *AcquireThunk(void *user, uint32_t *slot, uint32_t *width, uint32_t *height)
    {
      gfx::Texture *output = static_cast<Renderer *>(user)->Acquire(slot, width, height);

      if (!output)
        return nullptr;

#if REX_HAS_VULKAN
      if (g_vulkan)
        return reinterpret_cast<void *>(uintptr_t(gfx::vulkan::NativeImage(*output)));
#endif
#if defined(_WIN32)
      return gfx::d3d12::NativeResource(*output);
#else
      return nullptr;
#endif
    }

    void ReleaseThunk(void *user, uint32_t slot)
    {
      static_cast<Renderer *>(user)->Release(slot);
    }

    // The sst GPU plugin, if it is the one loaded (rex::system::LoadGpuPlugin's per-config names)
#if defined(_WIN32)
    using Plugin = HMODULE;

    Plugin FindPlugin()
    {
      for (const wchar_t *name : {L"rexgpu-sst.dll", L"rexgpu-sstd.dll", L"rexgpu-sstrd.dll"})
        if (HMODULE h = GetModuleHandleW(name))
          return h;

      return nullptr;
    }

    void *PluginSymbol(Plugin plugin, const char *name) { return reinterpret_cast<void *>(GetProcAddress(plugin, name)); }
#else
    using Plugin = void *;

    Plugin FindPlugin()
    {
      for (const char *name : {"librexgpu-sst.so", "librexgpu-sstd.so", "librexgpu-sstrd.so"})
      {
        const std::string path = (rex::filesystem::GetExecutableFolder() / name).string();

        if (void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD)) // only if already loaded (a reference we keep)
          return h;
      }

      return nullptr;
    }

    void *PluginSymbol(Plugin plugin, const char *name) { return dlsym(plugin, name); }
#endif

    // First boot: the VIDEO page's quality preset from the GPU's memory
#if defined(_WIN32)
    void ReportGpu(const D3D12Provider &provider)
    {
      DXGI_ADAPTER_DESC1 desc = {};
      Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;

      if (!provider.GetDXGIFactory() || FAILED(provider.GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(provider.GetDevice()->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&desc)))
        return;

      char name[128] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
      sst::menu::OnGpuDetected(uint64_t(desc.DedicatedVideoMemory) >> 20, name);
    }
#endif

#if REX_HAS_VULKAN
    void ReportGpu(const rex::ui::vulkan::VulkanProvider &provider)
    {
      const auto *device = provider.vulkan_device();
      VkPhysicalDeviceMemoryProperties mp = {};
      device->vulkan_instance()->functions().vkGetPhysicalDeviceMemoryProperties(device->physical_device(), &mp);
      uint64_t local = 0;

      for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
      {
        if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
          local = std::max<uint64_t>(local, mp.memoryHeaps[i].size);
      }

      sst::menu::OnGpuDetected(local >> 20, device->properties().deviceName);
    }
#endif

    // Locks the renderer for a hook (no render thread); empty when inactive
    std::unique_lock<std::mutex> Enter()
    {
      if (!g_active.load(std::memory_order_acquire))
        return {};

      std::unique_lock lock(g_state->mutex);

      if (!g_active.load(std::memory_order_acquire))
        return {};

      return lock;
    }

    // Runs a hooked call, or queues it for the render thread. fill(Command&) sets the call's arguments; copy(Capture&, Command&) adds the copies of guest memory it reads (render thread only: without one, the command runs while the memory is still as it was)
    template <typename Fill, typename Copy>
    void Issue(const uint8_t *base, Command::Kind kind, Fill &&fill, Copy &&copy)
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
      copy(cap, c);
      cap.Submit();
    }

    // A draw's textures and vertex data that the game may rewrite before the render thread gets to the draw
    void CopyDrawData(Capture &cap, const DrawCall &call)
    {
      // UI-sized: few vertices (a sprite, a line of text)
      constexpr uint32_t kUiDrawVertices = 256;
      const bool ui = !cap.DepthTested(call.device);

      // How many vertices a UI draw reads (it sizes their vertex copies); 3D draws copy only watched buffers, whole
      uint32_t vertices = 0;

      if (ui && !call.up_data)
      {
        auto t = cap.Time(CapturePerf::kIndexScan);
        vertices = cap.DrawVertices(call.device, call);
      }

      const bool ui_sized = ui && (call.up_data ? call.count <= kUiDrawVertices : vertices && vertices <= kUiDrawVertices);

      {
        auto t = cap.Time(CapturePerf::kTextures);
        cap.Textures(call.device, ui_sized);
      }

      if (!call.up_data)
      {
        auto t = cap.Time(CapturePerf::kVertices);
        cap.Vertices(call.device, vertices, ui, call);
      }
    }

    void IssueDraw(const uint8_t *base, const DrawCall &call)
    {
      Issue(
          base, Command::kDraw, [&](Command &c)
          { c.draw = call; },
          [&](Capture &cap, Command &)
          {
            cap.Device(call.device);

            if (REXCVAR_GET(sst_native_draw_copies))
              CopyDrawData(cap, call);

            if (call.up_data)
              cap.Range(call.up_data, call.up_stride * (call.start + call.count)); // the vertices themselves
          });
    }

    // Render-target trace text
    void IssueEvent(const uint8_t *base, uint32_t device, const std::string &text)
    {
      Issue(
          base, Command::kEvent,
          [&](Command &c)
          {
            c.device = device;
            c.data = reinterpret_cast<const uint8_t *>(text.data());
            c.size = uint32_t(text.size());
          },
          [&](Capture &cap, Command &c)
          {
            c.data = cap.Bytes(text.data(), text.size());
            cap.Device(device);
          });
    }

    // BeginVertices' arguments, until the matching EndVertices
    struct PendingVertices
    {
      uint32_t device = 0, prim = 0, count = 0, stride = 0, data = 0;
    };

    thread_local PendingVertices t_pending_vertices;

    // Logs each new kind (primitive type, stride) of immediate-mode draw once
    void LogImmediateDraw(const PendingVertices &p)
    {
      static uint32_t logged_kinds[16];
      static uint32_t logged = 0;
      const uint32_t kind = p.prim | p.stride << 8;

      for (uint32_t i = 0; i < logged; ++i)
        if (logged_kinds[i] == kind)
          return;

      if (logged >= 16)
        return;

      logged_kinds[logged++] = kind;
      REXLOG_INFO("[sst-native] immediate-mode draw (BeginVertices/EndVertices): prim {} vertices {} stride {}", p.prim, p.count, p.stride);
    }
  }

  // Lifetime

  void Initialize(rex::Runtime *runtime)
  {
    Plugin plugin = FindPlugin();

    if (!plugin)
      return; // another GPU plugin: stay inactive

    auto set_source = reinterpret_cast<SstSetFrameSourceFn>(PluginSymbol(plugin, SST_SET_FRAME_SOURCE_EXPORT));
    auto gpu_api = reinterpret_cast<SstGpuApiFn>(PluginSymbol(plugin, SST_GPU_API_EXPORT));
    auto *gs = runtime ? runtime->graphics_system() : nullptr;

    if (!set_source || !gs || !gs->provider())
    {
      REXLOG_ERROR("[sst-native] sst GPU plugin is loaded but {} - native renderer disabled", !set_source ? "has no frame-source export (old build?)" : "has no graphics device");
      return;
    }

    // The plugin's provider decides the backend (sst_gpu_api)
    std::unique_ptr<gfx::Device> device;
    g_vulkan = gpu_api && gpu_api() == kSstGpuApiVulkan;

    if (g_vulkan)
    {
#if REX_HAS_VULKAN
      const auto &provider = *static_cast<const rex::ui::vulkan::VulkanProvider *>(gs->provider());
      ReportGpu(provider);
      device = gfx::vulkan::CreateDevice(provider);
#endif
      if (!device)
      {
        REXLOG_ERROR("[sst-native] the Vulkan device can't run the native renderer - native renderer disabled");
        return;
      }

      REXLOG_INFO("[sst-native] graphics API: Vulkan");
    }
    else
    {
#if !defined(_WIN32)
      REXLOG_ERROR("[sst-native] the sst GPU plugin isn't on Vulkan - native renderer disabled");
      return;
#else
      const auto *provider = static_cast<const D3D12Provider *>(gs->provider());

      if (!provider->GetDevice())
      {
        REXLOG_ERROR("[sst-native] sst GPU plugin has no D3D12 device - native renderer disabled");
        return;
      }

      InstallPresentHook(*provider);
      ReportGpu(*provider);
      device = gfx::d3d12::CreateDevice(*provider);
#endif
    }

    auto state = std::make_unique<State>();
    state->set_frame_source = set_source;
    state->shaders.Initialize(device ? device->shader_format() : gfx::ShaderFormat::kDxil, rex::filesystem::GetExecutableFolder() / "cache" / "shaders");

    // The game's frame: 1280x720, or 1280x1024 in its own 4:3 mode
    const uint32_t frame_h = sst::menu::NativeFourThree() ? 1024 : 720;

    if (REXCVAR_GET(sst_native_write_watch) && runtime->memory())
      write_watch::Install(runtime->memory());

    if (!state->renderer.Initialize(std::move(device), &state->shaders, runtime->memory()->physical_membase(), 1280, frame_h))
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

  // Hooks

  void OnCreateShader(const uint8_t *base, uint32_t container, uint32_t object)
  {
    if (!Active())
      return;

    // Container header: +0 magic (0x102A11xx), +4 / +8 the sizes of its two parts
    if ((Be32(base, container) & 0xFFFFFF00u) != 0x102A1100u)
      return;

    const uint32_t size = Be32(base, container + 4) + Be32(base, container + 8);

    if (size == 0 || size > (1u << 20))
      return;

    const uint8_t *host = HostPtr(base, container);
    Issue(
        base, Command::kShader,
        [&](Command &c)
        {
          c.data = host;
          c.size = size;
          c.object = object;
        },
        [&](Capture &cap, Command &c)
        { c.data = cap.Bytes(host, size); }); // the container may be a temporary buffer
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

    LogImmediateDraw(p);
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
          c.stencil = a.r9; // (z took f1)
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
          cap.Range(call.src_rect, 16);    // D3DRECT
          cap.Range(call.dst_point, 8);    // D3DPOINT
          cap.Range(call.clear_color, 16); // D3DVECTOR4

          if (Plausible(call.dst_texture))
            cap.Range(call.dst_texture + 0x1C, 24); // the texture's fetch constant
        });
  }

  void OnResolveVariant(const uint8_t *base, const HookArgs &a)
  {
    if (!Active())
      return;

    IssueEvent(base, a.r3, fmt::format("resolve variant sub_82311120 r3-r10 {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} f1 {}", a.r3, a.r4, a.r5, a.r6, a.r7, a.r8, a.r9, a.r10, a.f1));
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
#if defined(_WIN32)
        const bool ok = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) != 0;
#else
        const bool ok = false; // (raising a thread's priority needs privileges on Linux)
#endif
        REXLOG_INFO("[sst-native] game rendering thread priority raised to above normal{}", ok ? "" : " (failed)");
      }
    }

    NoteSwap(s);
    cap.New(Command::kSwap);
    cap.Submit();
    cap.EndFrame();
    LogCapturePerf(s);
  }
}