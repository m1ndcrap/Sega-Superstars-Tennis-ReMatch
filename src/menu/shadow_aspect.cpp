// Ultrawide shadows: the shadow view (sub_822D04B8) fits each cascade's orthographic box around a portion of the main camera's view (sub_821338F0(0)), half-width = half-height x the camera's stored aspect at cam+0xC4. That aspect stays 16:9 while the camera's projection
// (cam+0x80, row-major: +0x80 x scale, +0x94 y scale) is the chosen wider one, so the cascades covered only the middle 16:9 area and shadows past it were stretched from the shadow map's edge (the beach and OutRun courts). The projection's aspect is used for the call (cvar sst_shadow_wide).

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>

#include "menu/guest.h"

REXCVAR_DEFINE_BOOL(sst_shadow_wide, true, "SST", "Ultrawide: the shadow map covers the whole wide view (false: only the middle 16:9 area, as the game sets it; shadows past it stretch)");

extern "C" void __imp__sub_822D04B8(PPCContext &__restrict, uint8_t *);
extern "C" void sub_821338F0(PPCContext &__restrict, uint8_t *);

namespace
{
  float LoadF(uint8_t *base, uint32_t a) { return std::bit_cast<float>(sst::menu::Load32(base, a)); }
  void StoreF(uint8_t *base, uint32_t a, float f) { sst::menu::Store32(base, a, std::bit_cast<uint32_t>(f)); }

  std::atomic<bool> g_logged{false};
}

extern "C" REX_FUNC(sub_822D04B8)
{
  if (!REXCVAR_GET(sst_shadow_wide))
  {
    __imp__sub_822D04B8(ctx, base);
    return;
  }

  // The main camera (the same call the function makes first)
  const PPCContext saved = ctx;
  ctx.r3.u64 = 0;
  sub_821338F0(ctx, base);
  const uint32_t cam = ctx.r3.u32;
  ctx = saved;

  if (cam < 0x40000000u || cam >= 0xFFFF0000u)
  {
    __imp__sub_822D04B8(ctx, base);
    return;
  }

  const float stored = LoadF(base, cam + 0xC4), xs = LoadF(base, cam + 0x80), ys = LoadF(base, cam + 0x94);
  const float projected = std::fabs(xs) > 1e-6f ? ys / xs : 0.0f;

  if (std::fabs(stored - 16.0f / 9.0f) >= 0.01f || projected <= 16.0f / 9.0f + 0.01f || projected >= 10.0f)
  {
    __imp__sub_822D04B8(ctx, base);
    return;
  }

  if (!g_logged.exchange(true))
    REXLOG_INFO("[sst-menu] shadows: cascades fitted to the {:.4f} view (the camera's stored aspect is {:.4f})", projected, stored);

  StoreF(base, cam + 0xC4, projected);
  __imp__sub_822D04B8(ctx, base);
  StoreF(base, cam + 0xC4, stored);
}