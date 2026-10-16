// sst_rematch - Direct3D 12 setup: GPU choice on dual-GPU PCs

#pragma once

#include <cstdint>
#include <string>

struct ID3D12Device;

namespace sst::gpu
{
  // Before the runtime creates its device (OnPreSetup). Unless --d3d12_adapter is given: on PCs with two GPUs (laptops with
  // integrated + dedicated graphics) picks the high-performance one. The SDK otherwise takes the first adapter, which there
  // is usually the integrated GPU.
  void ConfigureDevice();

  // Device-removed diagnostics (DRED: auto-breadcrumbs and page-fault data, enabled by ConfigureDevice). The renderer registers its
  // device and a function that names its draws (`frame`: the number in the command list's name, `draw`: the index of the draw op
  // in that list); DumpDeviceRemoved logs the removal reason, where the GPU stopped and what it was drawing. Once per process.
  using DrawLabelFn = std::string (*)(uint64_t frame, uint32_t draw);
  void SetDiagnosticsDevice(ID3D12Device *device, DrawLabelFn label);
  bool DumpDeviceRemoved(); // false if the device is fine
}