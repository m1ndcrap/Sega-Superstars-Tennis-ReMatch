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

  // Achievement icons: a linear 32-bit ARGB texture page added to a scene (its pixels are written after the scene's .GPU file, at data_offset in the file's buffer), split into square slots side by side
  struct IconPage
  {
    uint32_t data_offset = 0;     // in the .GPU buffer (4 KB aligned)
    uint32_t width = 0, height = 0; // pixels (width a multiple of 32)
    std::vector<uint32_t> slots;  // texture hash per slot (slot k: columns [k * height, (k + 1) * height))
  };

  // Options > ACHIEVEMENTS: a scrolling list on the RECORDS page (scene 487BEEA2), in the records' clipped panel. Row i: group row_group + i (y offset i x pitch) with a blue panel, the icon (colour, and greyed for locked), name, description and gamerscore texts. All texts start blank (text_blank); the page sets them
  struct AchievementList
  {
    uint32_t page_hash = 0;              // new scene page holding the list and the summary
    uint32_t list = 0;                   // the rows' group (in the records' content group's place; scrolls with keys d32c49f0 (top) .. 03ccad8d (bottom))
    uint32_t summary = 0;                // TEXT right of the page title
    uint32_t row_group = 0, row_panel = 0, row_icon = 0, row_icon_locked = 0, row_name = 0, row_desc = 0, row_score = 0; // + row
    uint32_t text_blank = 0;
    IconPage page;
    std::vector<int> icon_slot;          // per row: colour slot (locked: icon_slot + locked_offset)
    int locked_offset = 4;
    std::vector<float> desc_scale;       // per row
    int visible_rows = 5;
    float pitch = 0;                     // out: row spacing (layout heights)
  };

  bool AddAchievementList(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, AchievementList &list, Bytes &out_dat, Bytes &out_rel, std::string &error);

  // Unlock popup: a new page (scene entry `page_hash`) in MsgBox.DAT with one group that slides down from above the screen (key hide_label, frame 0) to the top of it (show_label, frame 10): title bar, header text, icon (icon_base + slot, one object per slot), name and up to three description lines; the panel picture comes in three heights (panel[n - 1] for n description lines; the popup shows one)
  struct AchievementPopup
  {
    static constexpr int kLines = 3;
    uint32_t page_hash = 0, group = 0, bar = 0, header = 0, name = 0, icon_base = 0;
    uint32_t panel[kLines] = {}, desc[kLines] = {};
    uint32_t hide_label = 0, show_label = 0;
    uint32_t text_blank = 0;
    IconPage page;
  };

  // The popup's description: lines of at most this width (font units of the MsgBox body font at scale 1)
  constexpr float kPopupDescWidth = 535.0f;

  bool AddAchievementPopup(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, const AchievementPopup &popup, Bytes &out_dat, Bytes &out_rel, std::string &error);

  // Test: parses and rewrites the scene unchanged (should give the same bytes).
  bool RoundTrip(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, Bytes &out_dat, Bytes &out_rel, std::string &error);

  // Extra buffer room to reserve when reading a scene file that may be widened.
  inline uint32_t ExtraRoom(uint32_t size, bool rel)
  {
    return rel ? size / 2 + 0x2000 : size / 3 + 0x8000;
  }
}