// sst_rematch - QUIT: a seventh main menu category, below OPTIONS

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sst::menu::quit
{
  using Bytes = std::vector<uint8_t>;

  constexpr uint32_t kScriptHash = 0x9E754704; // lookup2("mainmenu")

  // Scene objects and keyframe labels of the new row
  constexpr uint32_t kParent = 0x51554900, kRow = 0x51554901, kHighlight = 0x51554902, kFirstChild = 0x51554910;
  constexpr uint32_t kBarLabel = 0x51554920, kOptionsFrom = 0x51554921, kOptionsTo = 0x51554922;
  constexpr uint32_t kAction = 0x51554930; // its entry in the action table (never matched by the game)

  // Texts (resolved by the port's text hook)
  constexpr uint32_t kTextLabel = 0x5354FF10;       // "QUIT"
  constexpr uint32_t kTextDescription = 0x5354FF11; // shown under the menu

  // The game's YES / NO message "RETURN TO ONLINE MENU": texts swapped while the quit dialog is up
  constexpr uint32_t kMessage = 0x3AE99363;
  constexpr uint32_t kMessageBody = 0x0DFACC62, kMessageTitle = 0x252270C9;

  // Natives (group 0x53) the added function calls
  constexpr uint32_t kNativeDialogBegin = 14, kNativeDialogEnd = 15, kNativeQuit = 16;
  constexpr uint32_t kNativeEscape = 17; // (menu level) -> 1: Esc was pressed on the category list (level 1); called every frame

  // The patched "mainmenu" module, or nullopt (error says why)
  std::optional<Bytes> PatchScript(const uint8_t *module, size_t size, std::string &error);
}