// sst_rematch - frame hand-off between the exe (native renderer) and the
// "sst" GPU plugin (presentation).
//
// The native renderer runs on the game's render thread inside the exe and
// renders each frame into one of a small ring of D3D12 textures. The plugin's
// command processor, at swap, asks for the newest finished frame and copies it
// into the presenter's guest output.
//
// Rules:
// - Frames are in D3D12_RESOURCE_STATE_COPY_SOURCE whenever they are handed out
//   and use the presenter's guest output format (R10G10B10A2_UNORM).
// - Everything is submitted to the D3D12 provider's direct queue, so GPU order
//   follows CPU submission order: the exe submits a frame before publishing it,
//   and the plugin submits its copy before calling release.
// - acquire() pins the frame so the exe will not render into it until
//   release(); the same frame may be acquired again if no newer one exists.

#pragma once

#include <cstdint>

struct ID3D12Resource;

struct SstFrameSource {
  uint32_t struct_size;
  void* user;
  // Newest finished frame, or nullptr if none yet.
  ID3D12Resource* (*acquire)(void* user, uint32_t* slot, uint32_t* width, uint32_t* height);
  void (*release)(void* user, uint32_t slot);
};

// Exported by rexgpu-sst*.dll. Passing nullptr detaches the source (the plugin
// then presents its own placeholder). The struct is copied.
using SstSetFrameSourceFn = void (*)(const SstFrameSource* source);
#define SST_SET_FRAME_SOURCE_EXPORT "sst_gpu_set_frame_source"
