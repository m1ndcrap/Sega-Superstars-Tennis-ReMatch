// What the input files share (devices.cpp: devices, players, Esc; key_bindings.cpp: the keyboard's bindings)

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace sst::input
{
  constexpr uint32_t kVkEscape = 0x1B; // VK_ESCAPE

  inline std::string Lower(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                   { return char(std::tolower(c)); });

    return s;
  }

  // key_bindings.cpp: this game's window has the focus; a key (virtual-key code) is down
  bool WindowFocused();
  bool KeyDown(uint32_t vk);

  // devices.cpp: the players' devices again (the keyboard's player setting changed)
  void ReassignPlayers();
}