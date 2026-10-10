// sst_rematch - "sst" GPU plugin: native rendering backend

#include "sst_graphics_system.h"

#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "gpu/frame_source.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace sst::gpu
{

  using Microsoft::WRL::ComPtr;
  using rex::ui::d3d12::D3D12Presenter;
  using rex::ui::d3d12::D3D12Provider;

  // graphics system
  void SstGraphicsSystem::CreateProvider(bool /*with_presentation*/)
  {
    // Same as the xenos D3D12 backend: swapchains are created per window by the presenter, so one provider covers headless and windowed use.
    provider_ = D3D12Provider::Create();
  }

  std::unique_ptr<rex::graphics::CommandProcessor> SstGraphicsSystem::CreateCommandProcessor()
  {
    return std::make_unique<SstCommandProcessor>(this, kernel_state_);
  }

  // command processor
  namespace
  {
    const D3D12Provider &Provider(rex::graphics::GraphicsSystem *gs)
    {
      return *static_cast<const D3D12Provider *>(gs->provider());
    }

    D3D12_RESOURCE_BARRIER Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                      D3D12_RESOURCE_STATES after)
    {
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = resource;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = before;
      b.Transition.StateAfter = after;
      return b;
    }

    // Frame source registered by the exe's native renderer (see gpu/frame_source.h).
    // The mutex is held from acquire to release, so detaching waits for a copy in progress.
    std::mutex g_source_mutex;
    SstFrameSource g_source = {};
    bool g_has_source = false;

    // Slowly cycling colour around SST's blue, so a moving image proves frames are being produced and presented. Replaced by real rendering in later milestones.
    void FrameColor(uint64_t frame, float out[4])
    {
      const float t = float(frame % 600) / 600.0f * 6.2831853f;
      out[0] = 0.10f + 0.10f * std::sin(t);
      out[1] = 0.25f + 0.10f * std::sin(t + 2.1f);
      out[2] = 0.60f + 0.15f * std::sin(t + 4.2f);
      out[3] = 1.0f;
    }

  }

  SstCommandProcessor::SstCommandProcessor(SstGraphicsSystem *graphics_system, rex::system::KernelState *kernel_state) : CommandProcessor(graphics_system, kernel_state) {}

  SstCommandProcessor::~SstCommandProcessor() = default;

  bool SstCommandProcessor::SetupContext()
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

    if (spdlog::logger *gpu_log = rex::GetLoggerRaw(rex::log::gpu()); gpu_log && gpu_log->level() < spdlog::level::warn)
      gpu_log->set_level(spdlog::level::warn);

    REXLOG_INFO("[sst-gpu] native backend ready: {}x{} output, emulated draws disabled", kOutputWidth, kOutputHeight);
    return true;
  }

  void SstCommandProcessor::ShutdownContext()
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

  void SstCommandProcessor::IssueSwap(uint32_t /*frontbuffer_ptr*/, uint32_t /*frontbuffer_width*/, uint32_t /*frontbuffer_height*/)
  {
    rex::ui::Presenter *presenter = graphics_system_->presenter();
    if (!presenter || !output_)
      return;

    ++frame_;

    // Newest frame from the native renderer, if it is running
    std::unique_lock source_lock(g_source_mutex);
    ID3D12Resource *native_frame = nullptr;
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

    presenter->RefreshGuestOutput(
        width, height, width, height,
        [&](rex::ui::Presenter::GuestOutputRefreshContext &context) -> bool
        {
          auto &d3d12_context =
              static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext &>(context);
          ID3D12Resource *guest_output = d3d12_context.resource_uav_capable();

          const uint32_t slot = uint32_t(frame_ % kFramesInFlight);
          if (!WaitForFrameSlot(slot))
            return false;
          ID3D12CommandAllocator *allocator = allocators_[slot].Get();
          allocator->Reset();
          command_list_->Reset(allocator, nullptr);

          // Source: the native renderer's frame (already COPY_SOURCE), or our placeholder clear while it has nothing yet.
          ID3D12Resource *source = native_frame;

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

          // Copy into the presenter's guest output texture, which must end up back in kGuestOutputInternalState.
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
          D3D12_BOX box = {0, 0, 0, uint32_t(std::min<uint64_t>(src_desc.Width, dst_desc.Width)),
                           std::min<uint32_t>(src_desc.Height, dst_desc.Height), 1};
          command_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

          D3D12_RESOURCE_BARRIER back = Transition(guest_output, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   D3D12Presenter::kGuestOutputInternalState);
          command_list_->ResourceBarrier(1, &back);
          command_list_->Close();

          // Must go on the provider's direct queue (presenter contract).
          ID3D12CommandQueue *queue = Provider(graphics_system_).GetDirectQueue();
          ID3D12CommandList *lists[] = {command_list_.Get()};
          queue->ExecuteCommandLists(1, lists);
          queue->Signal(fence_.Get(), ++fence_value_);
          slot_fence_values_[slot] = fence_value_;

          context.SetIs8bpc(true);
          return true;
        });

    if (native_frame)
      g_source.release(g_source.user, native_slot);
  }

}

// plugin entry points
extern "C" REX_GPU_PLUGIN_EXPORT void sst_gpu_set_frame_source(const SstFrameSource *source)
{
  std::lock_guard lock(sst::gpu::g_source_mutex);
  if (source && source->struct_size >= sizeof(SstFrameSource) && source->acquire &&
      source->release)
  {
    sst::gpu::g_source = *source;
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

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void)
{
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem *rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo *info)
{
  if (abi_version != rex::system::kGpuPluginAbiVersion || !info ||
      info->struct_size < sizeof(rex::system::GpuCreateInfo))
  {
    REXLOG_ERROR("[sst-gpu] incompatible host (ABI {})", abi_version);
    return nullptr;
  }
  if (!rex::ui::d3d12::D3D12Provider::IsD3D12APIAvailable())
  {
    REXLOG_ERROR("[sst-gpu] Direct3D 12 is not available");
    return nullptr;
  }
  return new sst::gpu::SstGraphicsSystem();
}