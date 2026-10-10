// sst_rematch - native D3D12 renderer (exe side)
//
// Runs on the game's render thread, driven by hooks on the Xbox D3D functions
// (src/gpu/d3d_trace.cpp is the single hook site). Active only when the "sst"
// GPU plugin is loaded (--gpu_plugin=sst); otherwise every entry point is a
// cheap no-op and the game renders through the emulated Xenos backend.
//
// Increment A (current): frame hand-off to the plugin + shader pipeline
// (XenosRecomp translation and DXC compilation of every shader the game
// creates, with a disk cache). The frame is still a clear.

#pragma once

#include <cstdint>

namespace rex {
class Runtime;
}
struct PPCContext;

namespace sst::native {

// Startup / shutdown (main thread, from the app's OnPostSetup / OnShutdown).
void Initialize(rex::Runtime* runtime);
void Shutdown();

// True once Initialize found the sst plugin and the D3D12 device.
bool Active();

// --- hook entry points (game threads) --------------------------------------------
// Create{Vertex,Pixel}Shader: `container` is the guest address of the shader
// container (r3 before the call), `object` the returned shader object.
void OnCreateShader(const uint8_t* base, uint32_t container, uint32_t object);

// Clear(dev, count, rects, flags, color, z, stencil).
void OnClear(uint32_t flags, uint32_t color, float z, uint32_t stencil);

// Swap: the frame is complete.
void OnSwap();

}  // namespace sst::native
