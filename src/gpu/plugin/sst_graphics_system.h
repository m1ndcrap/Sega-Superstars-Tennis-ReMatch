// sst_rematch - "sst" GPU plugin: native rendering backend
//
// Built from the SDK's generic GPU sources (PM4 command stream, registers,
// fences, interrupts, vsync, swap) plus this backend. The original Xbox D3D
// code keeps running and producing PM4, so everything the game waits on still
// happens, but this backend does not emulate the GPU: draws and resolves in the
// PM4 stream are dropped, and at swap it presents its own native output.
//
// At swap it presents the newest frame from the exe's native renderer (handed
// over through sst_gpu_set_frame_source, see gpu/frame_source.h), or a
// colour-cycling blue placeholder while there is none.

#pragma once

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/ui/presenter.h>

#include <array>
#include <atomic>
#include <cstdint>

#if defined(_WIN32)
#include <d3d12.h>
#include <wrl/client.h>
#endif

#if REX_HAS_VULKAN
#include <rex/ui/vulkan/api.h>
#endif

namespace sst::gpu
{
  class SstGraphicsSystem final : public rex::graphics::GraphicsSystem
  {
  public:
    std::string name() const override { return vulkan_ ? "SST native (Vulkan)" : "SST native (D3D12)"; }

    // The provider's API (sst_gpu_api, falling back to D3D12 when Vulkan can't start)
    bool vulkan() const { return vulkan_; }

    // Vulkan: VIDEO > VSYNC picks the swapchain's present mode, fixed when the swapchain is created. Called by the command processor every few swaps: when sst_vsync differs from the mode in use (changed in the menu, or read from sst_settings.ini after the first swapchain), the presenter makes its swapchain again with the new mode
    void SyncPresentMode();

    // Replaces the SDK's guest vblank timer (a 1 ms Sleep loop, so its ticks follow the Windows timer resolution, up to 15.6 ms late) with a precise one locked to the display's refresh when it matches the guest's 60 Hz
    rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher *function_dispatcher, rex::system::KernelState *kernel_state) override;

    // Guest vblank ticks so far and the QPC time of the last one (VblankLoop)
    uint64_t vblank_ticks() const { return vblank_ticks_.load(std::memory_order_acquire); }
    int64_t last_vblank_qpc() const { return last_vblank_qpc_.load(std::memory_order_acquire); }

  protected:
    void CreateProvider(bool with_presentation) override;
    std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override;

  private:
    int VblankLoop();
    bool vulkan_ = false;
    std::atomic<int> present_vsync_{-1}; // VSync of the present mode set (-1: none yet)
    std::atomic<uint64_t> vblank_ticks_{0};
    std::atomic<int64_t> last_vblank_qpc_{0};
  };

  class SstCommandProcessor final : public rex::graphics::CommandProcessor
  {
  public:
    SstCommandProcessor(SstGraphicsSystem *graphics_system, rex::system::KernelState *kernel_state);
    ~SstCommandProcessor() override;

    void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height) override;

  protected:
    bool SetupContext() override;
    void ShutdownContext() override;

    // Emulated shaders are never translated: the native renderer uses the XenosRecomp output keyed by the D3D Create*Shader hooks instead
    rex::graphics::Shader *LoadShader(rex::graphics::xenos::ShaderType, uint32_t, const uint32_t *, uint32_t) override { return nullptr; }

    // Emulated draws and resolves are dropped
    bool IssueDraw(rex::graphics::xenos::PrimitiveType, uint32_t, IndexBufferInfo *, bool) override { return true; }
    bool IssueCopy() override { return true; }

  private:
    static constexpr uint32_t kOutputWidth = 1280;
    static constexpr uint32_t kOutputHeight = 720;
    static constexpr uint32_t kFramesInFlight = 3;

#if defined(_WIN32)
    bool WaitForFrameSlot(uint32_t slot);
    bool SetupD3D12();
    void ShutdownD3D12();
#endif

    bool vulkan_ = false;
#if REX_HAS_VULKAN
    // Vulkan: one command buffer + fence per frame slot, copying the native frame into the presenter's guest output image
    bool SetupVulkan();
    void ShutdownVulkan();
    bool RefreshVulkan(rex::ui::Presenter::GuestOutputRefreshContext &context, VkImage native_frame, uint32_t width, uint32_t height);

    std::array<VkCommandPool, kFramesInFlight> vk_pools_{};
    std::array<VkCommandBuffer, kFramesInFlight> vk_buffers_{};
    std::array<VkFence, kFramesInFlight> vk_fences_{};
    PFN_vkCmdCopyImage vk_copy_image_ = nullptr;
#endif

#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12Resource> output_; // our native frame, RT-capable
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kFramesInFlight> allocators_;
    std::array<uint64_t, kFramesInFlight> slot_fence_values_{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    uint64_t fence_value_ = 0;
#endif
    uint64_t frame_ = 0;
    
    // Swap pacing stats (per 600 swaps): guest vblanks between swaps, time from the last vblank to the swap (the game's frame work), time inside IssueSwap
    uint64_t last_swap_ticks_ = 0;
    uint32_t swaps_ = 0, swaps_multi_vblank_ = 0, swaps_late_phase_ = 0;
    double phase_sum_ms_ = 0, phase_max_ms_ = 0, issue_sum_ms_ = 0, issue_max_ms_ = 0;
  };
}