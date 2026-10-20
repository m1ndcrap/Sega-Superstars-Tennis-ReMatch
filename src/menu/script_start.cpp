// Scripts starting (the front end's sub_821CCA28: start script r4, a name hash, with argument r5). The front end finds a script's module in its directory (guest.h, FindScriptNode). The port points directory entries at its own modules before the game starts them:
//   - "video": the port's VIDEO page (tools/gen_graphics_script.py)
//   - "splash": a skippable boot intro (tools/gen_intro_script.py)
//   - "pausemenu_match": the game's module patched with a VIDEO row (pause_patch.cpp); needs the patched PauseMenu scene
//   - "mainmenu": the game's module patched with a QUIT row (main_menu_quit.cpp); needs the patched main menu scene
//   - "options_general": the same list page with the GENERAL list (the game's four settings and LANGUAGE)
//
// It also tells the input code where the game is (title screen, menus, a match)

#include "input/controls.h"
#include "menu/achievements.h"
#include "menu/main_menu_quit.h"
#include "menu/menu_internal.h"
#include "menu/pause_patch.h"
#include "menu/vs_screen.h"

#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>

extern "C" void __imp__sub_821CCA28(PPCContext &__restrict, uint8_t *);

namespace sst::menu
{
  namespace
  {
#include "menu/graphics_script.inc"
#include "menu/intro_script.inc"

    // Scripts whose start means a match is on (Esc pauses) or a menu is (Esc goes back)
    // lookup2: mainhud, ballgamehuds, tvhud
    constexpr uint32_t kMatchScripts[] = {0x2AF836AB, 0x8A7D60F5, 0x4D80CE71};

    // lookup2: mainmenu, startscreen, charselect, posselect, results_match, results_mission, results_scoreattack, results_scoreattack_retry,
    // challengemap, challengetourntree, tournament_ladder_offline, tournament_ladder_intro, tournament_lose_retry, tournament_special_match,
    // tournresultsrankandscore, trophy_sequence, trophy_sequence_loser, trophy_sequence_special, onlinemenu, options_settings, awards,
    // credits, nameentry, waitbeforespecialmatch, tournament_hiscores, challengeinit
    constexpr uint32_t kMenuScripts[] = {0x9E754704, 0x1187AE2B, 0x4BC618A9, 0x2EC0986E, 0x1CC9C24A, 0x0BE84691, 0xB504251A, 0xF40FB5E4, 0x394ED102, 0x93F12D2E, 0x7BC4731A, 0x63C234A3, 0x5B3870AF, 0x5C03314A, 0x1F4FCE03, 0x96420D5C, 0x6552C391, 0x962D4940, 0xDD7C9C15, 0x10701090, 0xE33D3780, 0xCF7FABD5, 0x2EDF05C6, 0x104664B1, 0xC85542DA, 0xAA7B77F8};

    // The module header's +28: where its debug strings start (the module ends a few hundred bytes after)
    constexpr uint32_t kModuleDebugStrings = 28, kModuleTail = 0x400;

    using PatchFn = std::optional<std::vector<uint8_t>> (*)(const uint8_t *module, size_t size, std::string &error);

    // A game script the port patches: the patched copy of each module seen (made once; 0 = failed), and whether the patched scene it
    // needs loaded
    struct PatchedScript
    {
      uint32_t hash;
      PatchFn patch;
      const char *feature;                           // for the log
      std::unordered_map<uint32_t, uint32_t> copies; // game's module -> patched copy
      bool scene_failed = false;
    };

    std::mutex g_mutex; // the PatchedScripts
    PatchedScript g_pause_script{pause::kScriptHash, pause::PatchScript, "pause menu VIDEO row"};
    PatchedScript g_main_menu_script{quit::kScriptHash, quit::PatchScript, "QUIT"};

    template <size_t N>

    bool In(const uint32_t (&list)[N], uint32_t h)
    {
      return std::find(std::begin(list), std::end(list), h) != std::end(list);
    }

    // Points the directory entry for `hash` at `module`
    bool Redirect(uint8_t *base, uint32_t front_end, uint32_t hash, uint32_t module)
    {
      const uint32_t node = FindScriptNode(base, front_end, hash);

      if (!node)
        return false;

      if (Load32(base, node + kNodeModule) != module)
      {
        Store32(base, node + kNodeModule, module);
        REXLOG_INFO("[sst-menu] script {:08X} -> the port's version", hash);
      }

      return true;
    }

    // The patched copy of a module (made on first sight); 0 if it can't be patched. Under g_mutex
    uint32_t PatchedCopy(uint8_t *base, PatchedScript &p, uint32_t module)
    {
      if (auto it = p.copies.find(module); it != p.copies.end())
        return it->second;

      p.copies[module] = 0; // tried (no retry on failure)
      const uint32_t debug = Load32(base, module + kModuleDebugStrings);

      if (debug < 0x20 || debug > 0x100000)
        return 0;

      std::vector<uint8_t> bytes(debug + kModuleTail);
      std::memcpy(bytes.data(), GuestPtr(base, module), bytes.size());
      std::string error;
      const auto out = p.patch(bytes.data(), bytes.size(), error);

      if (!out)
      {
        REXLOG_WARN("[sst-menu] {} disabled (script: {})", p.feature, error);
        return 0;
      }

      const uint32_t copy = CopyToGuest(base, out->data(), uint32_t(out->size()));

      if (copy)
        REXLOG_INFO("[sst-menu] {:08X} patched: {} ({} bytes at {:08X})", p.hash, p.feature, out->size(), copy);

      p.copies[module] = copy;
      return copy;
    }

    // Points the script at its patched copy, or back at the game's module when the patched one can't be used (`usable` false, or its scene wasn't patched)
    void RedirectPatched(uint8_t *base, uint32_t front_end, PatchedScript &p, bool usable)
    {
      const uint32_t node = FindScriptNode(base, front_end, p.hash);

      if (!node)
        return;

      const uint32_t module = Load32(base, node + kNodeModule);
      std::lock_guard lock(g_mutex);
      usable = usable && !p.scene_failed;

      for (auto &[original, copy] : p.copies)
      {
        if (copy && module == copy)
        {
          if (!usable)
            Store32(base, node + kNodeModule, original);

          return;
        }
      }

      if (!usable)
        return;

      if (const uint32_t copy = PatchedCopy(base, p, module))
        Store32(base, node + kNodeModule, copy);
    }

    // The port's own module for a script (copied to guest memory once)
    uint32_t OwnModule(uint8_t *base, const uint8_t *script, uint32_t size, uint32_t &module, std::once_flag &once)
    {
      std::call_once(once, [&]
                     { module = CopyToGuest(base, script, size); });

      return module;
    }

    void RedirectToOwn(uint8_t *base, uint32_t front_end, uint32_t hash, uint32_t module, const char *name)
    {
      if (module && !Redirect(base, front_end, hash, module))
        REXLOG_WARN("[sst-menu] \"{}\" not found in the script directory", name);
    }

    // Where the game is, for the input code
    void TrackScreens(uint32_t script)
    {
      // Title screen -> main menu: whoever pressed Start is the game's 1P
      static bool title = false;

      if (script == kStartScreenScript)
      {
        title = true;
        sst::input::SetTitleScreen(true);
      }
      else if (script == quit::kScriptHash && title)
      {
        title = false;
        sst::input::SetTitleScreen(false);
        sst::input::MarkGameStarted();
      }

      // Esc on the keyboard: pause once a match's HUD starts, back once a menu starts
      if (In(kMatchScripts, script))
        sst::input::SetInMatch(true);
      else if (In(kMenuScripts, script))
        sst::input::SetInMatch(false);
    }
  }

  bool IsPauseMenuModule(uint32_t module)
  {
    if (!module)
      return false;

    std::lock_guard lock(g_mutex);

    for (auto &[original, copy] : g_pause_script.copies)
    {
      if (module == original || module == copy)
        return true;
    }

    return false;
  }

  void SetPauseSceneOk(bool ok)
  {
    std::lock_guard lock(g_mutex);
    g_pause_script.scene_failed = !ok;
  }

  void SetQuitSceneOk(bool ok)
  {
    std::lock_guard lock(g_mutex);
    g_main_menu_script.scene_failed = !ok;
  }
}

using namespace sst::menu;

extern "C" REX_FUNC(sub_821CCA28)
{
  const uint32_t front_end = ctx.r3.u32;
  ApplyCameraAspect(base);

  // The KEY BINDINGS page is over once the page it leaves for starts
  if (ctx.r4.u32 == kControllerScript || ctx.r4.u32 == kSettingsScript)
    page::KeyBindingsClosed();

  // CONTROLS page left with Y: KEY BINDINGS (the VIDEO page's script with the key list) instead of the Settings list
  if (ctx.r4.u32 == kSettingsScript && controls::TakeOpenKeyBindings())
  {
    page::OpenKeyBindingsNext();
    ctx.r4.u64 = kVideoScript;
  }

  // Settings > GENERAL: the port's list page (the VIDEO page's script with the GENERAL list: the game's settings and LANGUAGE)
  if (ctx.r4.u32 == kGeneralScript && REXCVAR_GET(sst_graphics_menu) && REXCVAR_GET(sst_language_menu))
  {
    page::OpenGeneralNext();
    ctx.r4.u64 = kVideoScript;
  }

  const uint32_t script = ctx.r4.u32;
  TrackScreens(script);
  vs::OnScriptStart(base, front_end, script);

  if (REXCVAR_GET(sst_graphics_menu))
  {
    if (script == pause::kScriptHash)
    {
      RedirectPatched(base, front_end, g_pause_script, true);
      page::PauseMenuStarted();
    }

    if (script == quit::kScriptHash)
      RedirectPatched(base, front_end, g_main_menu_script, REXCVAR_GET(sst_quit_item));

    if (script == kVideoScript)
    {
      static uint32_t module = 0;
      static std::once_flag once;
      RedirectToOwn(base, front_end, kVideoScript, OwnModule(base, kGraphicsScript, sizeof(kGraphicsScript), module, once), "video");
    }
  }

  if (script == kSplashScript && REXCVAR_GET(sst_skip_intro))
  {
    static uint32_t module = 0;
    static std::once_flag once;
    RedirectToOwn(base, front_end, kSplashScript, OwnModule(base, kIntroScript, sizeof(kIntroScript), module, once), "splash");
  }

  // Options > ACHIEVEMENTS / RECORDS (achievements.cpp)
  sst::ach::OnScriptStart(ctx, base, front_end, script);

  __imp__sub_821CCA28(ctx, base);
}