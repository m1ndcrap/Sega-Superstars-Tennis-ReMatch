// sst_rematch - native renderer: is guest memory there to read? The render thread runs behind the game. While a character or court loads,
// the game can free an index / vertex buffer or a texture that a queued draw still points at, and reuse the memory (the crash reports show
// file-path text where the index buffer object was). Reading a decommitted guest page is an access violation, so the renderer checks before
// following such pointers and skips the draw (the console's GPU would have drawn garbage for a frame at worst).

#pragma once

#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstdint>

namespace sst::native
{
  namespace detail
  {
    inline rex::memory::Memory *GuestMemory()
    {
      rex::system::KernelState *kernel = rex::system::kernel_state();
      return kernel ? kernel->memory() : nullptr;
    }

    // First and last page only: a freed buffer is released whole, and this keeps the check cheap for big vertex buffers (it runs for
    // every draw)
    inline bool RangeReadable(rex::memory::BaseHeap *heap, uint32_t address, uint32_t size)
    {
      const uint32_t last = address + size - 1;
      return heap && heap->QueryRangeAccess(address, address) != rex::memory::PageAccess::kNoAccess && heap->QueryRangeAccess(last, last) != rex::memory::PageAccess::kNoAccess;
    }
  }

  // Physical address (0 - 0x1FFFFFFF, as in fetch constants). Checked in the physical heap, which every physical view (0xA0000000,
  // 0xC0000000, 0xE0000000) allocates from.
  inline bool GuestPhysicalReadable(uint32_t physical, uint32_t size)
  {
    if (!size)
      size = 1;

    if (physical >= 0x20000000u || uint64_t(physical) + size > 0x20000000u)
      return false;

    rex::memory::Memory *memory = detail::GuestMemory();
    return !memory || detail::RangeReadable(memory->GetPhysicalHeap(), physical, size);
  }

  // Guest virtual address (a physical view such as 0xA0000000 + physical is checked as physical memory)
  inline bool GuestReadable(uint32_t address, uint32_t size)
  {
    if (!size)
      size = 1;

    if (uint64_t(address) + size - 1 > 0xFFFFFFFFull)
      return false;

    rex::memory::Memory *memory = detail::GuestMemory();

    if (!memory)
      return true;

    if (address >= 0xA0000000u)
    {
      const uint32_t physical = memory->GetPhysicalAddress(address);
      return physical != UINT32_MAX && GuestPhysicalReadable(physical, size);
    }

    return detail::RangeReadable(memory->LookupHeap(address), address, size);
  }
}