// Native D3D12 renderer

#pragma once

#include <cstdint>

namespace rex
{
  class Runtime;
}

namespace sst::native
{
  // Startup / shutdown (main thread, from the app's OnPostSetup / OnShutdown)
  void Initialize(rex::Runtime *runtime);
  void Shutdown();

  // True once Initialize found the sst plugin and the D3D12 device
  bool Active();

  // Guest registers captured before a hooked call (r3.. are clobbered by it)
  struct HookArgs
  {
    uint32_t r3, r4, r5, r6, r7, r8;
    double f1;
    uint32_t r9 = 0, r10 = 0;
  };

  // hook entry points (game threads)
  // Create{Vertex,Pixel}Shader: `container` is the guest address of the shader container (r3 before the call), `object` the returned shader object.
  void OnCreateShader(const uint8_t *base, uint32_t container, uint32_t object);

  // DrawIndexedVertices(dev, prim, base_vertex, start_index, index_count)
  // DrawVertices(dev, prim, start_vertex, vertex_count)
  // DrawVerticesUP(dev, prim, vertex_count, data, stride)
  void OnDrawIndexed(const uint8_t *base, const HookArgs &a);
  void OnDraw(const uint8_t *base, const HookArgs &a);
  void OnDrawUP(const uint8_t *base, const HookArgs &a);

  // BeginVertices(dev, prim, vertex_count, stride) -> pointer the caller fills with vertex data, then EndVertices(dev). The game's immediate-mode helpers (trails, particles) call these directly instead of DrawVerticesUP; the draw is issued at EndVertices, when the data is complete. data: Begin's return
  void OnBeginVertices(const HookArgs &a, uint32_t data);
  void OnEndVertices(const uint8_t *base, const HookArgs &a);

  // DrawVerticesUP's wrapper calls BeginVertices itself and inlines the end: forget the pending Begin after it
  void OnDrawUPDone();

  // Clear(dev, count, rects, flags, color, z, stencil)
  void OnClear(const uint8_t *base, const HookArgs &a);

  // Resolve(dev, flags, src_rect*, dst_texture, dst_point*, level, slice, clear_color*, clear_z (f1), ...): EDRAM render target -> texture copy
  void OnResolve(const uint8_t *base, const HookArgs &a);

  // Other render-target events (render-target trace only): the end-of-tiling resolve variant sub_82311120 and SetPredication
  void OnResolveVariant(const uint8_t *base, const HookArgs &a);
  void OnSetPredication(const uint8_t *base, const HookArgs &a);

  // Swap: the frame is complete
  void OnSwap();
}