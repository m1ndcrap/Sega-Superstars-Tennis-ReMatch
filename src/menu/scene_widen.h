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

  // Test: parses and rewrites the scene unchanged (should give the same bytes).
  bool RoundTrip(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, Bytes &out_dat, Bytes &out_rel, std::string &error);

  // Extra buffer room to reserve when reading a scene file that may be widened.
  inline uint32_t ExtraRoom(uint32_t size, bool rel)
  {
    return rel ? size / 2 + 0x2000 : size / 3 + 0x8000;
  }
}