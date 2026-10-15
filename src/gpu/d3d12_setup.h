// sst_rematch - Direct3D 12 setup: GPU choice on dual-GPU PCs

#pragma once

namespace sst::gpu
{
  // Before the runtime creates its device (OnPreSetup). Unless --d3d12_adapter is given: on PCs with two GPUs (laptops with
  // integrated + dedicated graphics) picks the high-performance one. The SDK otherwise takes the first adapter, which there
  // is usually the integrated GPU.
  void ConfigureDevice();
}