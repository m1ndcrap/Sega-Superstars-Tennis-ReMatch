// Adds a VIDEO row to the in-match pause menu

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

  // Rows top to bottom: VIDEO sits above RETURN TO CHARACTER SELECT / MAIN MENU (its index stays 6; the cursor and the VIDEO page follow this order).
  constexpr std::array<int, kRows> kVisualOrder = {0, 1, 2, 3, 6, 4, 5};

  // Text ids used by the patched scene; the port's text hook resolves them.
  constexpr uint32_t kTextBlank = 0x5354FF00;
  constexpr uint32_t kTextVideo = 0x5354FF01;

  // Spare label / value texts the VIDEO page writes + the row's place on screen (0-6, top to bottom)
  constexpr uint32_t kSpareLabel = 0x53560020, kSpareValue = 0x53560030;

  // The VIDEO page's scroll bar (added to the scene; the port hides kScrollGroup (4:5) except on the VIDEO page; the thumb's key kScrollPos + i is scroll position i)
  constexpr uint32_t kScrollGroup = 0x53560040, kScrollTrack = 0x53560041, kScrollThumb = 0x53560042;
  constexpr uint32_t kScrollHide = 0x53560048, kScrollShow = 0x53560049, kScrollPos = 0x53560050;
  constexpr uint32_t kScrollShowFrame = 1000; // (the shown key: past any frame the panel's animations play)
  constexpr float kScrollHiddenX = 4.0f;      // (hidden: this far right of the frame)
  constexpr uint32_t kYellowTag = 0x2A3BDF69; // the yellow (3P) tag texture
  constexpr uint32_t kThumbTex = 0x53560060;  // its middle (a bright yellow strip: the whole tag, squeezed thin, samples a blurred mip level)
  constexpr int kScrollCount = 12;            // settings on the pause menu's VIDEO page (no ASPECT RATIO)
  constexpr int kScrollVisible = 7;           // rows shown

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

  // thumb_tex: the scroll thumb's texture (kThumbTex once scenes::AddSubTexture made it; else the whole yellow tag)
  std::optional<Bytes> PatchDat(const uint8_t *data, size_t size, Relocs &relocs, std::string &error, uint32_t thumb_tex = kYellowTag);
  std::optional<Bytes> PatchRel(const uint8_t *data, size_t size, const Relocs &relocs, std::string &error);
  std::optional<Bytes> PatchScript(const uint8_t *module, size_t size, std::string &error);
}