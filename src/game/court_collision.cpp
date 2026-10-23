// The court's collision object (built by sub_821F24F0) keeps a collision world holding the players' and ball's bodies. Each step (sub_821DC8B0) finds the touching pairs; the world's contact listener is what pushes two players apart (sub_821F1D70, with sub_821F20C0 as its pair filter).
// On the 360, the listener is typically set up by the court's set-up message 0x0347FB5E (sub_821F28F8: court+992 = court+468, its listener interface), which a game script sends while a match loads. In the recomp however, some stadiums never get that message, so contacts were found
// but did not really have an affect like not separating two players on the same side. When the court's world steps with no listener, this installs the court's respective listener similar to the setup.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <bit>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(sst_collision_listener_fix, true, "SST", "Player collision: when the court's collision world steps without its contact listener (the court set-up that installs it did not run), install the court's own listener so players push each other apart as on the Xbox 360");

extern "C" void __imp__sub_821DC8B0(PPCContext &__restrict, uint8_t *);

namespace
{
  constexpr uint32_t kCourtWorld = 472;                  // court + 472: its collision world
  constexpr uint32_t kCourtListener = 468;               // court + 468: its contact listener interface
  constexpr uint32_t kCourtListenerSlot = 472 + 520;     // court + 992: the world's listener (world + 520)
  constexpr uint32_t kCourtListenerVtable = 0x821015B4u; // the listener interface's vtable (identifies a court)

  uint8_t *Ptr(uint8_t *base, uint32_t a) { return base + a + rex::memory::detail::PhysicalHostOffset(a); }

  uint32_t Load32(uint8_t *base, uint32_t a)
  {
    uint32_t v;
    std::memcpy(&v, Ptr(base, a), 4);
    return std::byteswap(v);
  }

  void Store32(uint8_t *base, uint32_t a, uint32_t v)
  {
    v = std::byteswap(v);
    std::memcpy(Ptr(base, a), &v, 4);
  }
}

// Collision world step (r3 = the world)
extern "C" REX_FUNC(sub_821DC8B0)
{
  const uint32_t world = ctx.r3.u32;

  if (REXCVAR_GET(sst_collision_listener_fix) && world >= kCourtWorld)
  {
    const uint32_t court = world - kCourtWorld;

    if (Load32(base, court + kCourtListener) == kCourtListenerVtable && Load32(base, court + kCourtListenerSlot) == 0)
    {
      Store32(base, court + kCourtListenerSlot, court + kCourtListener);
      REXLOG_INFO("[sst-coll] court {:08X}: collision world stepped without its contact listener (the court set-up did not run); listener installed", court);
    }
  }

  __imp__sub_821DC8B0(ctx, base);
}