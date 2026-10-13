// sst_rematch - PlayStation pad lightbars in the player's colour

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sst::input
{
  struct LightbarPad
  {
    std::string guid; // SDL GUID
    uint32_t slot;    // player slot (XInput user index the game reads it as)
    uint32_t number;  // its player number outside a match (0 = 1P)
  };

  // The connected pads; PlayStation ones get their player's colour. Called whenever the devices change. In a match the game's own players are read (which pad each player uses), and the colours follow them
  void SetLightbars(const std::vector<LightbarPad> &pads);
}