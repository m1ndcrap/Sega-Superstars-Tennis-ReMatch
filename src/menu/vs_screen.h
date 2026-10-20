// The VS screen (the "loadingscreen" script: the match's players before it loads) and character select (the "charselect" script, same 3D scene) at ultrawide: their characters and player labels spread across the screen as at 16:9, instead of staying in the middle 16:9 area (cvar sst_vs_spread).
// The labels are moved in the scene files (hud::Spread, loading.dat / charselect.dat); the characters and the VS screen's per-player loading rings here, through the scripts' natives.

#pragma once

#include <rex/ppc/context.h>

#include <cstdint>

namespace sst::menu::vs
{
  // A script starts (sub_821CCA28): note the loadingscreen / charselect module
  void OnScriptStart(uint8_t *base, uint32_t front_end, uint32_t script);

  // Around a game native (the dispatcher, sub_821DFBC0): returns the state AfterNative needs
  struct Pending
  {
    uint32_t id = 0, out = 0;
    uint32_t model = 0, locator = 0; // 1:18's arguments (read before the call: the VM reuses its argument slots for the result)
    bool active = false;
  };

  Pending BeforeNative(PPCContext &ctx, uint8_t *base, uint32_t id);
  void AfterNative(uint8_t *base, const Pending &p);
}