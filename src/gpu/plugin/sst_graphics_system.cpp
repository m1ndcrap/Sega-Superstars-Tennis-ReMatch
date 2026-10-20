// sst_rematch - "sst" GPU plugin: native rendering backend

#include "sst_graphics_system.h"

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/windowed_app_context.h>
#if defined(_WIN32)
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>
#endif
#if REX_HAS_VULKAN
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#endif

#include "gpu/frame_source.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <cstring>
#include <cmath>
#include <mutex>

#if defined(_WIN32)
#include <dwmapi.h>
#else
#include <cerrno>
#include <ctime>
#include <sched.h>
#endif

REXCVAR_DEFINE_INT32(sst_vblank, 2, "SST",
                     "Guest vblank pacing: 0 = the SDK's timer (1 ms sleep loop), 1 = precise "
                     "60 Hz timer, 2 = locked to the display's refresh when it is ~60 Hz "
                     "(else precise timer)");
REXCVAR_DEFINE_INT32(sst_vblank_lead_ms, 8, "SST",
                     "sst_vblank 2: guest vblanks come this many ms before the display's vblank, so each "
                     "new frame reaches the presenter well before the window is repainted at the display "
                     "vblank (0 = on the vblank: frames then race the repaint and some are shown early, "
                     "then repeated)");

namespace sst::gpu
{
  using rex::X_STATUS;
#if defined(_WIN32)
  using Microsoft::WRL::ComPtr;
  using rex::ui::d3d12::D3D12Presenter;
  using rex::ui::d3d12::D3D12Provider;
#endif

  namespace
  {
    // Performance counter: QPC on Windows (the vblank thread's timestamps), steady_clock elsewhere
    int64_t Ticks()
    {
#if defined(_WIN32)
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return t.QuadPart;
#else
      return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
    }

    double TicksPerSecond()
    {
#if defined(_WIN32)
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return double(f.QuadPart);
#else
      return 1e9;
#endif
    }
  }

  // graphics system
  namespace
  {
    std::atomic<uint32_t> g_api{kSstGpuApiD3D12}; // for sst_gpu_api()
  }

  void SstGraphicsSystem::CreateProvider(bool with_presentation)
  {
    // sst_gpu_api is the exe's setting (VIDEO page / config): the cvar registry is shared
    const std::string api = rex::cvar::GetFlagByName("sst_gpu_api");
#if REX_HAS_VULKAN
#if defined(_WIN32)
    if (api == "vulkan")
#else
    // (Vulkan is the only API here)
#endif
    {
      // VIDEO > VSYNC: the presenter picks its swapchain's present mode from these when it creates it (the SDK prefers IMMEDIATE, which tears). VSync 'on' is FIFO, as D3D12's sync interval 1; off = IMMEDIATE.
      SyncPresentMode(); // (sst_vsync is still its default here: sst_settings.ini is read later, and SyncPresentMode follows it then)

      // Without GPU emulation: the native renderer needs none of its features, and the SDK patch enables what it does need
      provider_ = rex::ui::vulkan::VulkanProvider::Create(false, with_presentation);

      if (provider_)
      {
        vulkan_ = true;
        g_api = kSstGpuApiVulkan;
        REXLOG_INFO("[sst-gpu] graphics API: Vulkan");
        return;
      }

#if defined(_WIN32)
      REXLOG_ERROR("[sst-gpu] Vulkan is not available: using Direct3D 12");
#else
      REXLOG_ERROR("[sst-gpu] Vulkan is not available");
      return;
#endif
    }
#else
    if (api == "vulkan")
      REXLOG_WARN("[sst-gpu] sst_gpu_api = vulkan, but this build's SDK has no Vulkan (REXGLUE_USE_VULKAN off): using Direct3D 12");
#endif
    (void)with_presentation;
#if defined(_WIN32)
    // Same as the xenos D3D12 backend: swapchains are created per window by the presenter, so one provider covers headless and windowed use.
    provider_ = D3D12Provider::Create();
    g_api = kSstGpuApiD3D12;
#endif
  }

  void SstGraphicsSystem::SyncPresentMode()
  {
#if REX_HAS_VULKAN
    if (!vulkan_ && present_vsync_.load() >= 0)
      return;

    const int vsync = rex::cvar::GetFlagByName("sst_vsync") != "false" ? 1 : 0; // the exe's cvar
    const int previous = present_vsync_.exchange(vsync);

    if (previous == vsync)
      return;

    // The presenter reads these when it makes a swapchain. VSync on = FIFO, as D3D12's sync interval 1; off = IMMEDIATE
    rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate", vsync ? "false" : "true");
    rex::cvar::SetFlagByName("vulkan_allow_present_mode_mailbox", "false");
    rex::cvar::SetFlagByName("vulkan_allow_present_mode_fifo_relaxed", "false");
    REXLOG_INFO("[sst-gpu] Vulkan present mode: {}", vsync ? "FIFO (vsync)" : "IMMEDIATE (vsync off)");

    // A change after the first swapchain: remake it (a "resize" to the same size reconnects the presenter to the window surface)
    if (previous >= 0 && app_context_)
      app_context_->CallInUIThreadDeferred([this]
                                           {
                                             if (rex::ui::Presenter *p = presenter())
                                               p->OnSurfaceResizeFromUIThread(); });
#endif
  }

  std::unique_ptr<rex::graphics::CommandProcessor> SstGraphicsSystem::CreateCommandProcessor()
  {
    return std::make_unique<SstCommandProcessor>(this, kernel_state_);
  }

  rex::X_STATUS SstGraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher *function_dispatcher, rex::system::KernelState *kernel_state)
  {
    const rex::X_STATUS status = GraphicsSystem::SetupGuestGpu(function_dispatcher, kernel_state);

    if (status != X_STATUS_SUCCESS || REXCVAR_GET(sst_vblank) <= 0 || !REXCVAR_GET(vsync) || !vsync_worker_thread_)
      return status;

    // Stop the SDK's worker (as GraphicsSystem::Shutdown does) and run ours
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
    vsync_worker_running_ = true;
    vsync_worker_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]()
                                                                                                          { return VblankLoop(); }));

    vsync_worker_thread_->set_name("SST VSync");
    vsync_worker_thread_->Create();
    return status;
  }

  // Guest vblanks at the guest refresh rate (60 Hz). (Linux: a precise absolute-deadline timer; there is no compositor timing to lock to, so sst_vblank 2 behaves as 1 and the FIFO swapchain absorbs the drift).
  // With sst_vblank 2 and a display refreshing within 0.6 Hz of it, each tick is placed on a display vblank (DWM's composition timing), so every frame the game produces lines up with one display refresh instead of drifting against it (periodic repeated / skipped frames). Waits use a high-resolution waitable timer and a short spin
  int SstGraphicsSystem::VblankLoop()
  {
    rex::system::X_VIDEO_MODE video_mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
    const double guest_hz = std::clamp(double(float(video_mode.refresh_rate)), 1.0, 240.0);
    const double qpf = TicksPerSecond();

    auto now = []()
    {
      return Ticks();
    };

    const int64_t guest_period = int64_t(qpf / guest_hz);

#if defined(_WIN32)
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    const bool high_resolution = timer != nullptr;

    if (!timer)
      timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    using DwmTimingFn = HRESULT(WINAPI *)(HWND, DWM_TIMING_INFO *);
    DwmTimingFn dwm_timing = nullptr;

    if (REXCVAR_GET(sst_vblank) >= 2)
      if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
        dwm_timing = reinterpret_cast<DwmTimingFn>(reinterpret_cast<void *>(GetProcAddress(dwm, "DwmGetCompositionTimingInfo")));
#else
    const bool high_resolution = true; // clock_nanosleep on CLOCK_MONOTONIC (the clock steady_clock reads)
#endif

    // Sleeps until `target` (QPC): timer to ~1 ms before (high-resolution timers wake within ~0.5 ms; plain ones need the margin), then spins
    auto wait_until = [&](int64_t target)
    {
      const int64_t margin = int64_t(qpf * (high_resolution ? 0.0008 : 0.0021));
      const int64_t ahead = target - now() - margin;

#if defined(_WIN32)
      if (ahead > 0 && timer)
      {
        LARGE_INTEGER due;
        due.QuadPart = -std::max<int64_t>(1, int64_t(double(ahead) * 1e7 / qpf));

        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
          WaitForSingleObject(timer, 100);
      }

      while (now() < target && vsync_worker_running_)
        YieldProcessor();
#else
      if (ahead > 0)
      {
        const int64_t wake = now() + std::min<int64_t>(ahead, 100'000'000); // ticks are ns here; at most 100 ms per sleep
        timespec ts = {time_t(wake / 1'000'000'000), long(wake % 1'000'000'000)};

        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR && vsync_worker_running_)
        {
        }
      }

      while (now() < target && vsync_worker_running_)
        sched_yield();
#endif
    };

    int64_t next = now() + guest_period;
    int64_t last_mark = 0;

    // Stats per logging period
    uint64_t ticks = 0, synced_ticks = 0, caught_up = 0;
    double late_sum = 0, late_max = 0, display_hz = 0;
    bool logged_mode = false;

    while (vsync_worker_running_)
    {
      int64_t target = next, period = guest_period;
      bool synced = false;

#if defined(_WIN32)
      if (dwm_timing)
      {
        DWM_TIMING_INFO ti = {};
        ti.cbSize = sizeof(ti);

        if (SUCCEEDED(dwm_timing(nullptr, &ti)) && ti.qpcRefreshPeriod > 0 && ti.qpcVBlank > 0)
        {
          display_hz = qpf / double(ti.qpcRefreshPeriod);

          if (std::fabs(display_hz - guest_hz) < 0.6)
          {
            period = int64_t(ti.qpcRefreshPeriod);
            // The presenter repaints the window right after each display vblank (UI ticks); the game's swap (and the copy of its frame to the presenter) follows a guest vblank by ~1.2 ms. Ticking at the display vblank put those two within a millisecond of each other, so which frame a repaint showed jittered (one shown a refresh early, then repeated). Ticking ahead of it moves the swap mid-refresh
            const int64_t lead = std::clamp<int64_t>(int64_t(double(std::clamp(REXCVAR_GET(sst_vblank_lead_ms), 0, 14)) * qpf / 1000.0), 0, period * 7 / 8);
            const int64_t vblank = int64_t(ti.qpcVBlank) - lead;
            // The display vblank nearest the planned tick, but never two ticks within half a period
            const int64_t k = int64_t(std::llround(double(next - vblank) / double(period)));
            target = vblank + k * period;

            if (last_mark && target - last_mark < period / 2)
              target += period;

            synced = true;
          }
        }
      }
#endif

      if (!logged_mode)
      {
        logged_mode = true;
        REXLOG_INFO("[sst-gpu] guest vblank: {} ({:.2f} Hz guest{}, {} timer{})", synced ? "locked to the display refresh" : "precise timer", guest_hz, display_hz > 0 ? fmt::format(", display {:.2f} Hz", display_hz) : std::string(), high_resolution ? "high-resolution" : "standard", synced ? fmt::format(", {} ms ahead of the display vblank", std::clamp(REXCVAR_GET(sst_vblank_lead_ms), 0, 14)) : std::string());
      }

      wait_until(target);

      if (!vsync_worker_running_)
        break;

      const int64_t marked = now();
      last_vblank_qpc_.store(marked, std::memory_order_release);
      vblank_ticks_.fetch_add(1, std::memory_order_acq_rel);
      MarkVblank();
      ++ticks;
      synced_ticks += synced;
      const double late = double(marked - target) * 1000.0 / qpf;
      late_sum += late;
      late_max = std::max(late_max, late);
      last_mark = target;
      next = target + period;
      // Far behind (thread starved): count the missed vblanks like the SDK does (the game's clock), at most a few, then continue from now
      int missed = 0;

      while (now() - next > period / 2 && missed < 4 && vsync_worker_running_)
      {
        last_vblank_qpc_.store(now(), std::memory_order_release);
        vblank_ticks_.fetch_add(1, std::memory_order_acq_rel);
        MarkVblank();
        ++missed;
        next += period;
      }

      if (missed)
      {
        caught_up += missed;

        if (now() - next > period)
          next = now() + period;

        last_mark = next - period;
      }

      if (ticks >= 600)
      {
        REXLOG_INFO("[sst-gpu] guest vblank: {} ticks{}, late {:.2f} ms avg / {:.2f} max, {} caught up", ticks,
                    display_hz > 0 ? fmt::format(" ({} on display vblanks, display {:.2f} Hz)", synced_ticks, display_hz) : std::string(),
                    late_sum / double(ticks), late_max, caught_up);
        ticks = synced_ticks = caught_up = 0;
        late_sum = late_max = 0;
      }
    }

#if defined(_WIN32)
    if (timer)
      CloseHandle(timer);
#endif

    return 0;
  }

  // command processor

  namespace
  {
#if defined(_WIN32)
    const D3D12Provider &Provider(rex::graphics::GraphicsSystem *gs)
    {
      return *static_cast<const D3D12Provider *>(gs->provider());
    }

    D3D12_RESOURCE_BARRIER Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = resource;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = before;
      b.Transition.StateAfter = after;
      return b;
    }
#endif

    // Frame source registered by the exe's native renderer (see gpu/frame_source.h).
    // The mutex is held from acquire to release, so detaching waits for a copy in progress.
    std::mutex g_source_mutex;
    SstFrameSource g_source = {};
    bool g_has_source = false;

    // Color of screen before the game draws
    void FrameColor(uint64_t, float out[4])
    {
      out[0] = out[1] = out[2] = 0.0f;
      out[3] = 1.0f;
    }
  }

  SstCommandProcessor::SstCommandProcessor(SstGraphicsSystem *graphics_system, rex::system::KernelState *kernel_state) : CommandProcessor(graphics_system, kernel_state) {}

  SstCommandProcessor::~SstCommandProcessor() = default;

  bool SstCommandProcessor::SetupContext()
  {
    vulkan_ = static_cast<SstGraphicsSystem *>(graphics_system_)->vulkan();
#if REX_HAS_VULKAN && defined(_WIN32)
    const bool ok = vulkan_ ? SetupVulkan() : SetupD3D12();
#elif REX_HAS_VULKAN
    const bool ok = vulkan_ && SetupVulkan();
#else
    const bool ok = SetupD3D12();
#endif

    if (!ok)
      return false;

    // The generic command processor logs every PM4 CONTEXT_UPDATE at info level (~14k lines a minute in matches). Nothing in it matters while emulated rendering is off, so keep the gpu category at warnings and above
    if (spdlog::logger *gpu_log = rex::GetLoggerRaw(rex::log::gpu()); gpu_log && gpu_log->level() < spdlog::level::warn)
      gpu_log->set_level(spdlog::level::warn);

    REXLOG_INFO("[sst-gpu] native backend ready ({}): {}x{} output, emulated draws disabled", vulkan_ ? "Vulkan" : "D3D12", kOutputWidth, kOutputHeight);
    return true;
  }

#if defined(_WIN32)
  bool SstCommandProcessor::SetupD3D12()
  {
    ID3D12Device *device = Provider(graphics_system_).GetDevice();

    if (!device)
    {
      REXLOG_ERROR("[sst-gpu] no D3D12 device");
      return false;
    }

    // Native output target. Same format as the presenter's guest output so a plain copy hands it over at swap.
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kOutputWidth;
    desc.Height = kOutputHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = D3D12Presenter::kGuestOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&output_))))
    {
      REXLOG_ERROR("[sst-gpu] failed to create the output texture");
      return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 1;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap_))))
    {
      REXLOG_ERROR("[sst-gpu] failed to create the RTV heap");
      return false;
    }

    device->CreateRenderTargetView(output_.Get(), nullptr, rtv_heap_->GetCPUDescriptorHandleForHeapStart());

    for (auto &allocator : allocators_)
    {
      if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
      {
        REXLOG_ERROR("[sst-gpu] failed to create a command allocator");
        return false;
      }
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&command_list_))))
    {
      REXLOG_ERROR("[sst-gpu] failed to create the command list");
      return false;
    }

    command_list_->Close();

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
    {
      REXLOG_ERROR("[sst-gpu] failed to create the fence");
      return false;
    }

    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!fence_event_)
    {
      REXLOG_ERROR("[sst-gpu] failed to create the fence event");
      return false;
    }

    return true;
  }

#endif

#if REX_HAS_VULKAN
  bool SstCommandProcessor::SetupVulkan()
  {
    const auto &provider = *static_cast<const rex::ui::vulkan::VulkanProvider *>(graphics_system_->provider());
    const auto *device = provider.vulkan_device();
    const auto &dfn = device->functions();

    vk_copy_image_ = reinterpret_cast<PFN_vkCmdCopyImage>(device->vulkan_instance()->functions().vkGetDeviceProcAddr(device->device(), "vkCmdCopyImage"));

    if (!vk_copy_image_)
      return false;

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
      VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      pi.queueFamilyIndex = device->queue_family_graphics_compute();
      VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount = 1;
      VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

      if (dfn.vkCreateCommandPool(device->device(), &pi, nullptr, &vk_pools_[i]) != VK_SUCCESS || (ai.commandPool = vk_pools_[i], dfn.vkAllocateCommandBuffers(device->device(), &ai, &vk_buffers_[i]) != VK_SUCCESS) || dfn.vkCreateFence(device->device(), &fi, nullptr, &vk_fences_[i]) != VK_SUCCESS)
      {
        REXLOG_ERROR("[sst-gpu] failed to create the Vulkan command buffers");
        return false;
      }
    }

    return true;
  }

  void SstCommandProcessor::ShutdownVulkan()
  {
    const auto *provider = static_cast<const rex::ui::vulkan::VulkanProvider *>(graphics_system_->provider());

    if (!provider || !provider->vulkan_device())
      return;

    const auto *device = provider->vulkan_device();
    const auto &dfn = device->functions();

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
      if (vk_fences_[i])
      {
        dfn.vkWaitForFences(device->device(), 1, &vk_fences_[i], VK_TRUE, UINT64_MAX);
        dfn.vkDestroyFence(device->device(), vk_fences_[i], nullptr);
      }

      if (vk_pools_[i])
        dfn.vkDestroyCommandPool(device->device(), vk_pools_[i], nullptr);

      vk_fences_[i] = VK_NULL_HANDLE;
      vk_pools_[i] = VK_NULL_HANDLE;
      vk_buffers_[i] = VK_NULL_HANDLE;
    }
  }

  bool SstCommandProcessor::RefreshVulkan(rex::ui::Presenter::GuestOutputRefreshContext &context, VkImage native_frame, uint32_t width, uint32_t height)
  {
    using rex::ui::vulkan::VulkanPresenter;
    auto &vk_context = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext &>(context);
    const auto *device = static_cast<const rex::ui::vulkan::VulkanProvider *>(graphics_system_->provider())->vulkan_device();
    const auto &dfn = device->functions();
    const uint32_t slot = uint32_t(frame_ % kFramesInFlight);

    dfn.vkWaitForFences(device->device(), 1, &vk_fences_[slot], VK_TRUE, UINT64_MAX);
    dfn.vkResetFences(device->device(), 1, &vk_fences_[slot]);
    dfn.vkResetCommandPool(device->device(), vk_pools_[slot], 0);
    VkCommandBuffer cb = vk_buffers_[slot];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn.vkBeginCommandBuffer(cb, &bi);

    // The presenter's guest output: from its internal state (or never written) to a copy destination
    const VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_dst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.srcAccessMask = vk_context.image_ever_written_previously() ? VulkanPresenter::kGuestOutputInternalAccessMask : 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = vk_context.image_ever_written_previously() ? VulkanPresenter::kGuestOutputInternalLayout : VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = vk_context.image();
    to_dst.subresourceRange = all;
    dfn.vkCmdPipelineBarrier(cb, vk_context.image_ever_written_previously() ? VkPipelineStageFlags(VulkanPresenter::kGuestOutputInternalStageMask) : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

    if (native_frame)
    {
      // Already a copy source: the renderer's submission (earlier on this queue) ended with that transition
      VkImageCopy region = {};
      region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.extent = {width, height, 1};
      vk_copy_image_(cb, native_frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_context.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    else
    {
      float color[4];
      FrameColor(frame_, color);
      VkClearColorValue clear;
      std::memcpy(clear.float32, color, sizeof(color));
      dfn.vkCmdClearColorImage(cb, vk_context.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &all);
    }

    VkImageMemoryBarrier back = to_dst;
    back.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    back.dstAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    back.newLayout = VulkanPresenter::kGuestOutputInternalLayout;
    dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0, nullptr, 1, &back);
    dfn.vkEndCommandBuffer(cb);

    // Graphics queue 0 (presenter contract; the native renderer submits there too)
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult result;

    {
      auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
      result = dfn.vkQueueSubmit(queue.queue(), 1, &si, vk_fences_[slot]);
    }

    if (result != VK_SUCCESS)
    {
      REXLOG_ERROR("[sst-gpu] Vulkan submit failed ({})", int32_t(result));
      return false;
    }

    context.SetIs8bpc(true);
    return true;
  }
#endif

  void SstCommandProcessor::ShutdownContext()
  {
#if REX_HAS_VULKAN
    if (vulkan_)
    {
      ShutdownVulkan();
      return;
    }
#endif
#if defined(_WIN32)
    ShutdownD3D12();
#endif
  }

#if defined(_WIN32)
  void SstCommandProcessor::ShutdownD3D12()
  {
    // Let in-flight work finish before releasing what it references
    if (fence_ && fence_->GetCompletedValue() < fence_value_ && fence_event_)
    {
      fence_->SetEventOnCompletion(fence_value_, fence_event_);
      WaitForSingleObject(fence_event_, INFINITE);
    }

    if (fence_event_)
    {
      CloseHandle(fence_event_);
      fence_event_ = nullptr;
    }

    command_list_.Reset();

    for (auto &allocator : allocators_)
      allocator.Reset();

    rtv_heap_.Reset();
    output_.Reset();
    fence_.Reset();
  }

  bool SstCommandProcessor::WaitForFrameSlot(uint32_t slot)
  {
    const uint64_t needed = slot_fence_values_[slot];

    if (fence_->GetCompletedValue() >= needed)
      return true;

    if (FAILED(fence_->SetEventOnCompletion(needed, fence_event_)))
      return false;

    WaitForSingleObject(fence_event_, INFINITE);
    return true;
  }

#endif

  void SstCommandProcessor::IssueSwap(uint32_t /*frontbuffer_ptr*/, uint32_t /*frontbuffer_width*/, uint32_t /*frontbuffer_height*/)
  {
    rex::ui::Presenter *presenter = graphics_system_->presenter();

#if defined(_WIN32)
    if (!presenter || (!vulkan_ && !output_))
      return;
#else
    if (!presenter || !vulkan_)
      return;
#endif

    ++frame_;

    // VIDEO > VSYNC on Vulkan (D3D12: the present hook applies it every frame)
    if (vulkan_ && frame_ % 30 == 0)
      static_cast<SstGraphicsSystem *>(graphics_system_)->SyncPresentMode();

    // Pacing: where this swap falls relative to the guest vblanks (only with our vblank thread, which records them)
    const double qpf = TicksPerSecond();
    const int64_t t0 = Ticks();
    auto *sst_gs = static_cast<SstGraphicsSystem *>(graphics_system_);

    if (const uint64_t ticks = sst_gs->vblank_ticks())
    {
      const double phase = double(t0 - sst_gs->last_vblank_qpc()) * 1000.0 / qpf;

      if (last_swap_ticks_)
      {
        ++swaps_;
        swaps_multi_vblank_ += ticks - last_swap_ticks_ >= 2;
        phase_sum_ms_ += phase;
        phase_max_ms_ = std::max(phase_max_ms_, phase);
        swaps_late_phase_ += phase > 14.0;
      }

      last_swap_ticks_ = ticks;
    }

    struct IssueTimer
    {
      SstCommandProcessor *cp;
      int64_t start;
      double freq;
      ~IssueTimer()
      {
        const double ms = double(Ticks() - start) * 1000.0 / freq;
        cp->issue_sum_ms_ += ms;
        cp->issue_max_ms_ = std::max(cp->issue_max_ms_, ms);

        if (cp->swaps_ >= 600)
        {
          REXLOG_INFO("[sst-gpu] swaps: {} ({} came 2+ vblanks after the previous one; game "
                      "work after the vblank {:.2f} ms avg / {:.2f} max, {} over 14 ms); "
                      "presenting {:.2f} ms avg / {:.2f} max",
                      cp->swaps_, cp->swaps_multi_vblank_, cp->phase_sum_ms_ / cp->swaps_, cp->phase_max_ms_, cp->swaps_late_phase_, cp->issue_sum_ms_ / cp->swaps_, cp->issue_max_ms_);
          cp->swaps_ = cp->swaps_multi_vblank_ = cp->swaps_late_phase_ = 0;
          cp->phase_sum_ms_ = cp->phase_max_ms_ = cp->issue_sum_ms_ = cp->issue_max_ms_ = 0;
        }
      }
    } issue_timer{this, t0, qpf};

    // Newest frame from the native renderer, if it is running
    std::unique_lock source_lock(g_source_mutex);
    void *native_frame = nullptr; // ID3D12Resource, or VkImage with Vulkan (gpu/frame_source.h)
    uint32_t native_slot = 0, width = kOutputWidth, height = kOutputHeight;

    if (g_has_source)
    {
      native_frame = g_source.acquire(g_source.user, &native_slot, &width, &height);

      if (!native_frame)
        width = kOutputWidth, height = kOutputHeight;
    }

    if (!native_frame)
      source_lock.unlock();

    if (frame_ == 1 || frame_ % 600 == 0)
      REXLOG_INFO("[sst-gpu] presented frame {} ({})", frame_, native_frame ? "native renderer" : "placeholder");

    // VIDEO > ASPECT RATIO: the 16:9 frame is shown at the chosen aspect (the game's camera and the native renderer's UI fit match it)
    uint32_t aspect_xy[2] = {16, 9};

    if (g_has_source && g_source.struct_size >= sizeof(SstFrameSource) && g_source.display_aspect)
      g_source.display_aspect(g_source.user, &aspect_xy[0], &aspect_xy[1]);

    presenter->RefreshGuestOutput(width, height, aspect_xy[0], aspect_xy[1],
                                  [&](rex::ui::Presenter::GuestOutputRefreshContext &context) -> bool
                                  {
#if REX_HAS_VULKAN
                                    if (vulkan_)
                                      return RefreshVulkan(context, reinterpret_cast<VkImage>(native_frame), width, height);
#endif
#if !defined(_WIN32)
                                    return false;
#else
                                    auto &d3d12_context = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext &>(context);
                                    ID3D12Resource *guest_output = d3d12_context.resource_uav_capable();

                                    const uint32_t slot = uint32_t(frame_ % kFramesInFlight);

                                    if (!WaitForFrameSlot(slot))
                                      return false;

                                    ID3D12CommandAllocator *allocator = allocators_[slot].Get();
                                    allocator->Reset();
                                    command_list_->Reset(allocator, nullptr);

                                    // 1. Source: the native renderer's frame (already COPY_SOURCE), or our placeholder clear while it has nothing yet
                                    ID3D12Resource *source = static_cast<ID3D12Resource *>(native_frame);

                                    if (!source)
                                    {
                                      D3D12_RESOURCE_BARRIER to_rt = Transition(output_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                                      command_list_->ResourceBarrier(1, &to_rt);
                                      float color[4];
                                      FrameColor(frame_, color);
                                      command_list_->ClearRenderTargetView(rtv_heap_->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
                                      D3D12_RESOURCE_BARRIER to_src = Transition(output_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                                      command_list_->ResourceBarrier(1, &to_src);
                                      source = output_.Get();
                                    }

                                    // 2. Copy into the presenter's guest output texture, which must end up back in kGuestOutputInternalState
                                    D3D12_RESOURCE_BARRIER to_copy = Transition(guest_output, D3D12Presenter::kGuestOutputInternalState, D3D12_RESOURCE_STATE_COPY_DEST);
                                    command_list_->ResourceBarrier(1, &to_copy);

                                    D3D12_TEXTURE_COPY_LOCATION dst = {};
                                    dst.pResource = guest_output;
                                    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                    D3D12_TEXTURE_COPY_LOCATION src = {};
                                    src.pResource = source;
                                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                    const D3D12_RESOURCE_DESC dst_desc = guest_output->GetDesc();
                                    const D3D12_RESOURCE_DESC src_desc = source->GetDesc();
                                    D3D12_BOX box = {0, 0, 0, uint32_t(std::min<uint64_t>(src_desc.Width, dst_desc.Width)), std::min<uint32_t>(src_desc.Height, dst_desc.Height), 1};
                                    command_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

                                    D3D12_RESOURCE_BARRIER back = Transition(guest_output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12Presenter::kGuestOutputInternalState);
                                    command_list_->ResourceBarrier(1, &back);
                                    command_list_->Close();

                                    // Must go on the provider's direct queue (presenter contract)
                                    ID3D12CommandQueue *queue = Provider(graphics_system_).GetDirectQueue();
                                    ID3D12CommandList *lists[] = {command_list_.Get()};
                                    queue->ExecuteCommandLists(1, lists);
                                    queue->Signal(fence_.Get(), ++fence_value_);
                                    slot_fence_values_[slot] = fence_value_;

                                    context.SetIs8bpc(true);
                                    return true;
#endif
                                  });

    // The copy (if any) is submitted, so the frame can be reused by the exe
    if (native_frame)
      g_source.release(g_source.user, native_slot);
  }
}

// plugin entry points

// Called by the exe's native renderer (see gpu/frame_source.h)
extern "C" REX_GPU_PLUGIN_EXPORT void sst_gpu_set_frame_source(const SstFrameSource *source)
{
  std::lock_guard lock(sst::gpu::g_source_mutex);

  if (source && source->struct_size >= offsetof(SstFrameSource, display_aspect) && source->acquire && source->release)
  {
    sst::gpu::g_source = {};
    std::memcpy(&sst::gpu::g_source, source, std::min<size_t>(source->struct_size, sizeof(SstFrameSource)));
    sst::gpu::g_source.struct_size = sizeof(SstFrameSource);
    sst::gpu::g_has_source = true;
    REXLOG_INFO("[sst-gpu] native renderer attached");
  }
  else
  {
    sst::gpu::g_source = {};

    if (sst::gpu::g_has_source)
      REXLOG_INFO("[sst-gpu] native renderer detached");

    sst::gpu::g_has_source = false;
  }
}

// Called by the exe's native renderer: which backend to use (gpu/frame_source.h)
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t sst_gpu_api(void)
{
  return sst::gpu::g_api.load();
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void)
{
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem *rex_gpu_create(uint32_t abi_version, const rex::system::GpuCreateInfo *info)
{
  if (abi_version != rex::system::kGpuPluginAbiVersion || !info || info->struct_size < sizeof(rex::system::GpuCreateInfo))
  {
    REXLOG_ERROR("[sst-gpu] incompatible host (ABI {})", abi_version);
    return nullptr;
  }

#if defined(_WIN32)
  if (rex::cvar::GetFlagByName("sst_gpu_api") != "vulkan" && !rex::ui::d3d12::D3D12Provider::IsD3D12APIAvailable())
  {
    REXLOG_ERROR("[sst-gpu] Direct3D 12 is not available");
    return nullptr;
  }
#endif

  return new sst::gpu::SstGraphicsSystem();
}