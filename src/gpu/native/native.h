// sst_rematch - native D3D12 renderer (exe side)

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
  };

  // hook entry points (game threads)
  // Create{Vertex,Pixel}Shader: `container` is the guest address of the shader container (r3 before the call), `object` the returned shader object.
  void OnCreateShader(const uint8_t *base, uint32_t container, uint32_t object);

  // SetStreamSource(dev, stream, vb, offset, stride)
  void OnSetStreamSource(const HookArgs &a);

  // DrawIndexedVertices(dev, prim, base_vertex, start_index, index_count)
  // DrawVertices(dev, prim, start_vertex, vertex_count)
  // DrawVerticesUP(dev, prim, vertex_count, data, stride)
  void OnDrawIndexed(const uint8_t *base, const HookArgs &a);
  void OnDraw(const uint8_t *base, const HookArgs &a);
  void OnDrawUP(const uint8_t *base, const HookArgs &a);

  // Clear(dev, count, rects, flags, color, z, stencil).
  void OnClear(const uint8_t *base, const HookArgs &a);

  // Swap: the frame is complete.
  void OnSwap();

}