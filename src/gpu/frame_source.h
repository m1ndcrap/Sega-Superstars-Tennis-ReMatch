// sst_rematch - frame hand-off between the exe (native renderer) and the GPU plugin (presentation)

#pragma once

#include <cstdint>

struct ID3D12Resource;

struct SstFrameSource
{
  uint32_t struct_size;
  void *user;

  // Newest finished frame, or nullptr if none yet.
  ID3D12Resource *(*acquire)(void *user, uint32_t *slot, uint32_t *width, uint32_t *height);

  void (*release)(void *user, uint32_t slot);
};

using SstSetFrameSourceFn = void (*)(const SstFrameSource *source);
#define SST_SET_FRAME_SOURCE_EXPORT "sst_gpu_set_frame_source"