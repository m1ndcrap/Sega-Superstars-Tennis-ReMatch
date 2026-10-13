// sst_rematch - ultrawide layouts for SumoTool scenes

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sst::menu::widen
{
  using Bytes = std::vector<uint8_t>;

  // Should scene `name` (lower-case file name, e.g. "sstmainmenu.dat") get a wide layout? (Not the in-match HUD scenes: those are anchored instead.)
  bool WantsWideLayout(const std::string &name);

  struct Stats
  {
    int bars = 0, strips = 0, cropped = 0, swoosh = 0, stars = 0;
    int Total() const { return bars + strips + cropped + swoosh + stars; }
  };

  // Rewrites the scene for a screen `shift` wider on each side. Returns false (out untouched) on anything unexpected; `error` says why. Nothing to do is success with stats.Total() == 0.
  bool Widen(const std::string &name, const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, float shift, Bytes &out_dat, Bytes &out_rel, Stats &stats, std::vector<std::string> *log, std::string &error);

  // Main menu: a new category row below OPTIONS (a copy of the Xbox LIVE row: no sub-items). Hashes and labels are the caller's (the main menu script's tables refer to them)
  struct MainMenuRow
  {
    uint32_t parent, row, highlight; // position group, row group (the script's category list), highlight group
    uint32_t first_child;            // the copied children get first_child, first_child + 1, ...
    uint32_t text;                   // text id of the row's label
    uint32_t bar_label;              // cursor bar key for the row
    uint32_t options_from, options_to; // row keys: pushed down while OPTIONS is open (y 0 -> options_shift)
    float options_shift;
  };

  bool AddMainMenuRow(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, const MainMenuRow &row, Bytes &out_dat, Bytes &out_rel, std::string &error);

  // Settings > VIDEO (the GENERAL page layout, four rows): small up / down arrows above the first and below the last row, the rows' value arrow turned 90 degrees. Hidden (key `hide`, alpha 0) until the page animates them to key `show`
  struct ListCarets
  {
    uint32_t up, down;   // new objects
    uint32_t hide, show; // their key labels
    uint32_t colour;     // RGB in the top 24 bits (the arrow texture is white)
  };

  bool AddListCarets(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, const ListCarets &carets, Bytes &out_dat, Bytes &out_rel, std::string &error);

  // A new texture entry `to`: the part [u0, u1] x [v0, v1] (fractions) of texture `from`
  bool AddSubTexture(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, uint32_t from, uint32_t to, float u0, float u1, float v0, float v1, Bytes &out_dat, Bytes &out_rel, std::string &error);

  // Test: parses and rewrites the scene unchanged (should give the same bytes).
  bool RoundTrip(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, Bytes &out_dat, Bytes &out_rel, std::string &error);

  // Extra buffer room to reserve when reading a scene file that may be widened.
  inline uint32_t ExtraRoom(uint32_t size, bool rel)
  {
    return rel ? size / 2 + 0x2000 : size / 3 + 0x8000;
  }
}