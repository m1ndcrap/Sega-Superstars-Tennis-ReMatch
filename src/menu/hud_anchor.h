// sst_rematch - ultrawide HUD: in-match HUD elements anchored to the screen edges (VIDEO > ASPECT RATIO 21:9 / 32:9)

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sst::menu::hud
{
  // Is `name` (lower-case file name, e.g. "hud.dat") an in-match HUD scene?
  bool IsHudScene(const std::string &name);

  // Moves the left / right elements of a scene by -shift / +shift (fraction of the 1280-wide layout) when `sides` (in-match HUD scenes).
  // In every scene, positions entirely outside the 16:9 frame (elements waiting off-screen) move out by the same amount so they stay off the wider screen.
  // Returns how many elements moved sideways; `log` gets a line per top-level element.
  int Anchor(uint8_t *data, size_t size, float shift, std::vector<std::string> *log, bool sides = true);

  // The shift for a display aspect (0 when not wider than 16:9)
  inline float ShiftFor(float aspect)
  {
    const float s = aspect / (16.0f / 9.0f);
    return s > 1.0f ? 0.5f * (s - 1.0f) : 0.0f;
  }
}