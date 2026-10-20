// D3D12 backend of gfx (SDK's D3D12 device and direct queue)

#pragma once

#include "gpu/native/gfx/gfx.h"

#include <memory>

namespace rex::ui::d3d12
{
  class D3D12Provider;
}

namespace sst::gfx::d3d12
{
  std::unique_ptr<Device> CreateDevice(const rex::ui::d3d12::D3D12Provider &provider);

  // The D3D12 objects behind gfx objects, for D3D12-only code (device-removed diagnostics, F9 stream-output capture, the plugin's frame hand-off). Null when the object isn't a D3D12 one
  ID3D12Device *NativeDevice(Device &device);
  ID3D12CommandQueue *NativeQueue(Device &device);
  ID3D12GraphicsCommandList *NativeList(CommandList &list);
  ID3D12Resource *NativeResource(Texture &texture);
  ID3D12Resource *NativeResource(Buffer &buffer);
  ID3D12RootSignature *NativeRootSignature(Device &device, Layout layout);
}