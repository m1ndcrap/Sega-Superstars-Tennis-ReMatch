// sst_rematch - frame hand-off between the exe (native renderer) and the "sst" GPU plugin (presentation)

#pragma once

#include <cstdint>

struct SstFrameSource
{
  uint32_t struct_size;
  void *user;

  // Newest finished frame, or nullptr if none yet: an ID3D12Resource* (D3D12), or a VkImage handle (Vulkan), in the copy-source state (VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL), its format the presenter's guest output format. Written on the provider's graphics queue before it is handed over.
  void *(*acquire)(void *user, uint32_t *slot, uint32_t *width, uint32_t *height);
  void (*release)(void *user, uint32_t slot);

  // Display aspect ratio for the frames (VIDEO > ASPECT RATIO), e.g. 21:9
  void (*display_aspect)(void *user, uint32_t *x, uint32_t *y);
};

// Exported by rexgpu-sst*.dll. Passing nullptr detaches the source (the plugin then presents its own placeholder). The struct is copied.
using SstSetFrameSourceFn = void (*)(const SstFrameSource *source);
#define SST_SET_FRAME_SOURCE_EXPORT "sst_gpu_set_frame_source"

// Exported by rexgpu-sst*.dll: the graphics API its provider uses (sst_gpu_api), so the exe's renderer picks the matching backend
inline constexpr uint32_t kSstGpuApiD3D12 = 0;
inline constexpr uint32_t kSstGpuApiVulkan = 1;
using SstGpuApiFn = uint32_t (*)();
#define SST_GPU_API_EXPORT "sst_gpu_api"