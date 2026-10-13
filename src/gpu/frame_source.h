// sst_rematch - frame hand-off between the exe (native renderer) and the "sst" GPU plugin (presentation)

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

  // Display aspect ratio for the frames (VIDEO > ASPECT RATIO), e.g. 21:9
  void (*display_aspect)(void *user, uint32_t *x, uint32_t *y);
};

// Exported by rexgpu-sst*.dll. Passing nullptr detaches the source (the plugin then presents its own placeholder). The struct is copied.
using SstSetFrameSourceFn = void (*)(const SstFrameSource *source);
#define SST_SET_FRAME_SOURCE_EXPORT "sst_gpu_set_frame_source"