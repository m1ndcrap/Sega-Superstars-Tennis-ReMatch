// Script modules

#include "menu/script_asm.h"

#include <algorithm>

namespace sst::menu::script
{
  size_t ModuleSize(const uint8_t *m, size_t max_size)
  {
    if (max_size < 0x20)
      return 0;

    const uint32_t code = Rd32(m), functab = Rd32(m + 4), t2 = Rd32(m + 8), t3 = Rd32(m + 12), debug = Rd32(m + 28);

    if (code != 0x20 || !(code <= functab && functab <= t2 && t2 <= t3 && t3 <= debug) || debug + 4 > max_size)
      return 0;

    // The debug string table: offsets of the strings, which follow it
    const uint32_t first = Rd32(m + debug);

    if (first < debug + 4 || ((first - debug) & 3) || first - debug > 64)
      return 0;

    size_t end = first;

    for (uint32_t p = debug; p < first; p += 4)
    {
      size_t s = Rd32(m + p);

      while (s < max_size && m[s])
        ++s;

      if (s >= max_size)
        return 0;

      end = std::max(end, s + 1);
    }

    return (end + 3) & ~size_t(3);
  }
}