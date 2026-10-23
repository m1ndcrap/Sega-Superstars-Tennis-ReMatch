// Guest page write tracking (check out write_watch.h)

#include "gpu/native/write_watch.h"

#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <utility>

namespace sst::native::write_watch
{
  namespace
  {
    constexpr uint32_t kPages = 0x20000; // 512 MB of physical memory, 4 KB pages
    std::atomic<uint32_t> g_writes[kPages];
    std::atomic<uint32_t> g_bulk{0}; // ranges too big to count per page (memory released): every range counts as written
    std::atomic<uint64_t> g_faults{0};
    rex::memory::Memory *g_memory = nullptr;

    // The SDK's invalidation callback: [start, start + length) (physical) was written or released
    std::pair<uint32_t, uint32_t> Written(void *, uint32_t start, uint32_t length, bool)
    {
      g_faults.fetch_add(1, std::memory_order_relaxed);

      if (length > (16u << 20))
      {
        g_bulk.fetch_add(1, std::memory_order_relaxed);
      }
      else
      {
        const uint32_t first = start >> 12;
        const uint32_t last = uint32_t(std::min<uint64_t>((uint64_t(start) + std::max(length, 1u) - 1) >> 12, kPages - 1));

        for (uint32_t p = first; p <= last && p < kPages; ++p)
          g_writes[p].fetch_add(1, std::memory_order_relaxed);
      }

      return {start, length}; // unwatch only what was written: neighbouring watched pages must keep reporting
    }
  }

  bool Install(rex::memory::Memory *memory)
  {
    if (g_memory || !memory)
      return g_memory != nullptr;

    if (memory->RegisterPhysicalMemoryInvalidationCallback(Written, nullptr))
      g_memory = memory;

    return g_memory != nullptr;
  }

  bool Enabled()
  {
    return g_memory != nullptr;
  }

  uint32_t Stamp(uint32_t phys, uint32_t size)
  {
    uint32_t sum = g_bulk.load(std::memory_order_relaxed) * 0x9E3779B1u;
    const uint32_t last = std::min((phys + std::max(size, 1u) - 1) >> 12, kPages - 1);

    for (uint32_t p = phys >> 12; p <= last; ++p)
      sum += g_writes[p].load(std::memory_order_relaxed);

    return sum;
  }

  void Touch(uint32_t phys, uint32_t size)
  {
    const uint32_t last = std::min((phys + std::max(size, 1u) - 1) >> 12, kPages - 1);

    for (uint32_t p = phys >> 12; p <= last; ++p)
      g_writes[p].fetch_add(1, std::memory_order_relaxed);
  }

  void Arm(uint32_t phys, uint32_t size)
  {
    if (g_memory)
      g_memory->EnablePhysicalMemoryAccessCallbacks(phys, size, true, false);
  }

  uint64_t TakeFaults()
  {
    return g_faults.exchange(0);
  }
}