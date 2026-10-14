// sst_rematch - in-game achievements: Options > ACHIEVEMENTS list and unlock popups

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace rex::system
{
  class AchievementManager;
}

namespace sst::ach
{
  // Native group of the page script (natives 0x41xx)
  constexpr uint32_t kNativeGroup = 0x41;

  // sst_achievements_ui: the game-styled page and popups instead of the SDK's overlay and toasts
  bool UseGameUi();

  // At startup: listen for unlocks
  void Initialize(rex::system::AchievementManager &manager);

  // Scene files: extra buffer room for a scene .DAT / .REL (lower-case name) this module patches
  uint32_t ExtraRoom(const std::string &name);

  // A scene file was read (lower-case name, guest buffer): the popup's font comes from msgbox_en.dat
  void OnSceneFile(uint8_t *base, const std::string &name, uint32_t buffer);

  // Scene setup (sub_82595158): the patched sstmainmenu.dat (list) / msgbox.dat (popup) and their .REL; false: not patched (logged)
  bool BuildScene(const std::string &dat_name, const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, std::vector<uint8_t> &out_dat, std::vector<uint8_t> &out_rel);

  // Whether the caller put the patched scene in place (it may not fit its buffer)
  void SceneReady(const std::string &dat_name, bool ok);

  // Text ids of the list and the popup -> guest string (false: not ours)
  bool ResolveText(uint8_t *base, uint32_t id, uint32_t &out);

  // Script start (sub_821CCA28, r3 front-end, r4 script hash)
  void OnScriptStart(PPCContext &ctx, uint8_t *base, uint32_t front_end, uint32_t script);

  // Natives of the game the module answers instead (true: answered)
  bool OverrideNative(PPCContext &ctx, uint8_t *base, uint32_t id);

  // Script natives 0x41xx
  void HandleNative(PPCContext &ctx, uint8_t *base, uint32_t id);

  // Every frame (the scene manager's update, game thread): unlock popups
  void Frame(PPCContext &ctx, uint8_t *base);
}