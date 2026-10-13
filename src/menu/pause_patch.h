// sst_rematch - adds a VIDEO row to the in-match pause menu

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sst::menu::pause
{
  using Bytes = std::vector<uint8_t>;

  constexpr int kRows = 7;     // rows after the patch
  constexpr int kVideoRow = 6; // index of the new row (the game hard-codes 0-5)
  // Rows top to bottom: VIDEO sits above RETURN TO CHARACTER SELECT / MAIN MENU
  // (its index stays 6; the cursor and the VIDEO page follow this order).
  constexpr std::array<int, kRows> kVisualOrder = {0, 1, 2, 3, 6, 4, 5};

  // Text ids used by the patched scene; the port's text hook resolves them.
  constexpr uint32_t kTextBlank = 0x5354FF00;
  constexpr uint32_t kTextVideo = 0x5354FF01;

  // Natives the trampoline calls (group 0x53).
  constexpr uint32_t kNativeResult = 4;    // (k) -> see graphics_menu.cpp
  constexpr uint32_t kNativeEventHash = 5; // (input event hash)
  constexpr uint32_t kNativeBegin = 6;     // ()
  constexpr uint32_t kNativeLabel = 7;     // (visible row 0-6) -> text id
  constexpr uint32_t kNativeValue = 8;     // (visible row 0-6) -> text id
  constexpr uint32_t kNativeStep = 10;     // (row, -1 / +1, first, count) -> next row

  constexpr uint32_t kScriptHash = 0xEF8CD98C; // lookup2("pausemenu_match")

  // Relocation lists for PatchRel, filled by PatchDat: chunk tag -> offsets.
  struct Relocs
  {
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> chunks;
  };

  std::optional<Bytes> PatchDat(const uint8_t *data, size_t size, Relocs &relocs, std::string &error);
  std::optional<Bytes> PatchRel(const uint8_t *data, size_t size, const Relocs &relocs, std::string &error);
  std::optional<Bytes> PatchScript(const uint8_t *module, size_t size, std::string &error);

  // Size of a script module, from its header (end of the debug strings); 0 if the header does not look like a module.
  size_t ModuleSize(const uint8_t *module, size_t max_size);
}