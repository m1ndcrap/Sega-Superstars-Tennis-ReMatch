// A player keeps a history of 132-byte entries (player + 6124, newest at index player + 10216; type byte at entry + 44). This script prevents it from being null to prevent crashes.

#include "menu/guest.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(sst_hit_history_guard, true, "SST", "Match: when a player's latest hit entry has no shot data (a game bug that crashed a tournament match), callers that don't check for that get the previous shot's data instead of reading through a null pointer");

extern "C" void __imp__sub_82140CD8(PPCContext &__restrict, uint8_t *);

namespace
{
  using sst::menu::GuestPtr;
  using sst::menu::Load32;
  using sst::menu::Store32;

  constexpr uint32_t kEntries = 6124, kNewest = 10216, kEntrySize = 132, kType = 44, kData = 88;
  constexpr uint32_t kDummySize = 1024; // zeroed stand-in for the data (fields up to +320 are read)
  constexpr uint32_t kSlots = 4;        // copies handed out in turn (a caller may hold one across another call)

  // Return addresses of the callers that check +88 themselves
  bool ChecksData(uint32_t lr) { return lr == 0x82291E08u || lr == 0x82289C48u; }

  std::atomic<uint32_t> g_block{0}; // kSlots entry copies, then the dummy data
  std::atomic<uint32_t> g_next{0};
  std::atomic<uint32_t> g_logged{0};

  uint32_t Block()
  {
    uint32_t b = g_block.load();

    if (!b)
    {
      b = sst::menu::GuestAlloc(kSlots * kEntrySize + kDummySize);
      g_block.store(b);
    }

    return b;
  }
}

extern "C" REX_FUNC(sub_82140CD8)
{
  const uint32_t player = ctx.r3.u32, lr = ctx.lr;
  __imp__sub_82140CD8(ctx, base);
  const uint32_t entry = ctx.r3.u32;

  if (!REXCVAR_GET(sst_hit_history_guard) || !entry || Load32(base, entry + kData) || ChecksData(lr))
    return;

  const uint32_t block = Block();

  if (!block)
    return;

  const uint32_t dummy = block + kSlots * kEntrySize;

  // The previous type-2 entry with data, searching back from the one found (as the game's loop: index newest .. 0)
  uint32_t data = 0;
  const uint32_t newest = Load32(base, player + kNewest);

  for (uint32_t i = std::min((entry - player - kEntries) / kEntrySize, newest + 1); i-- > 0;)
  {
    const uint32_t e = player + kEntries + i * kEntrySize;

    if (*GuestPtr(base, e + kType) == 2 && (data = Load32(base, e + kData)))
      break;
  }

  if (!data)
  {
    std::memset(GuestPtr(base, dummy), 0, kDummySize);
    data = dummy;
  }

  const uint32_t copy = block + (g_next.fetch_add(1) % kSlots) * kEntrySize;
  std::memmove(GuestPtr(base, copy), GuestPtr(base, entry), kEntrySize);
  Store32(base, copy + kData, data);
  ctx.r3.u64 = copy;

  if (g_logged.fetch_add(1) < 8)
    REXLOG_WARN("[sst-match] player {:08X}: latest hit entry {:08X} has no shot data (caller {:08X}); using {}", player, entry, lr, data == dummy ? std::string("an empty stand-in") : fmt::format("{:08X} from an earlier hit", data));
}