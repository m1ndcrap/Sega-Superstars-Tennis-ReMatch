// The Vulkan backend of gfx (gfx.h), on the SDK's Vulkan device and its graphics queue 0 (the queue the SDK's presenter expects guest output work on)

#pragma once

#include "gpu/native/gfx/gfx.h"

#include <cstdint>
#include <memory>

namespace rex::ui::vulkan
{
  class VulkanProvider;
}

namespace sst::gfx::vulkan
{
  // Null (with the reason logged) when the device lacks what the translated shaders need: Vulkan 1.3 (dynamic rendering, dynamic vertex strides / topology), buffer device addresses, 64-bit integers, bindless descriptor arrays, timeline semaphores.
  std::unique_ptr<Device> CreateDevice(const rex::ui::vulkan::VulkanProvider &provider);

  // The VkImage behind a texture (as an integer: VkImage is a non-dispatchable handle), for the plugin's frame hand-off. 0 when the texture isn't a Vulkan one.
  uint64_t NativeImage(Texture &texture);
}