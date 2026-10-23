// Tracks guest physical memory writes using SDK write watches.
// Arm(range) makes pages read-only until written, triggering a callback and restoring write access.
// Stamp(range) changes on writes, allowing cached vertex buffers/microcode to be reused without comparison.
// Direct host writes (e.g., memexport) bypass watches, so callers refresh copies once per frame.

#pragma once

#include <cstdint>

namespace rex::memory
{
  class Memory;
}

namespace sst::native::write_watch
{
  // Registers with the SDK (once). False: not available, and Enabled() stays false
  bool Install(rex::memory::Memory *memory);
  bool Enabled();

  // Changes whenever any page of [phys, phys + size) is written (or memory is released)
  uint32_t Stamp(uint32_t phys, uint32_t size);

  // Counts [phys, phys + size) as written: for the renderer's own host writes into guest memory (memexport output), which the watch can't see
  void Touch(uint32_t phys, uint32_t size);

  // Watch [phys, phys + size) for writes (pages already watched cost a flag check)
  void Arm(uint32_t phys, uint32_t size);

  // Write faults since the last call
  uint64_t TakeFaults();
}