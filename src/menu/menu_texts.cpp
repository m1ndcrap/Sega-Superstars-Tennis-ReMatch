// Texts the port adds or renames. The game looks texts up by id (SumoTool's text tables, sub_82594AD0); the hook answers for the port's own ids (strings kept in a guest pool) and swaps a few of the game's while one of the port's dialogs borrows a message box.

#include "menu/main_menu_quit.h"
#include "menu/achievements.h"
#include "menu/menu_internal.h"
#include "menu/pause_patch.h"

#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <atomic>
#include <unordered_map>

namespace sst::menu
{
  namespace
  {
    constexpr uint32_t kPoolSize = 0x10000;
    constexpr uint32_t kTextIdBase = 0x53540000;       // the port's ids: kTextIdBase + n (made on demand) and a few fixed ones
    constexpr uint32_t kTextQuitQuestion = 0x5354FF12; // QUIT dialog body

    // The game's texts the port renames: "CONTROLLER" (the Settings page title, the Settings list item - same id as the page's layer hash -, the pause menu row) and "Xbox LIVE" (the main menu item and the header above the menu)
    constexpr uint32_t kTextController[] = {0x2E29AD95, 0xABB4502D, 0x193DCBAD};
    constexpr uint32_t kTextXboxLive[] = {0xA1B21EAB, 0x2815E3A5};

    // The restart dialog: the game's one-button (OK) message "NO CHANNELS FOUND" with other texts
    constexpr uint32_t kRestartBody = 0x84D43E5B;  // "Unable to find any Superstars Channels"
    constexpr uint32_t kRestartTitle = 0xEDDAB5CE; // "NO CHANNELS FOUND"

    struct Pool
    {
      std::mutex mutex;
      uint32_t base = 0, used = 0; // guest block
      bool tried = false;
      std::unordered_map<std::string, uint32_t> ids; // string -> text id
      std::unordered_map<uint32_t, uint32_t> texts;  // text id -> guest string (the fixed ids too)
    } g_pool;

    std::atomic<bool> g_restart_dialog{false}, g_quit_dialog{false};

    // A string in the pool; 0 when full. Under g_pool.mutex
    uint32_t Put(uint8_t *base, const std::string &text)
    {
      const uint32_t n = uint32_t(text.size()) + 1;

      if (!g_pool.base || g_pool.used + n > kPoolSize)
        return 0;

      const uint32_t at = g_pool.base + g_pool.used;
      std::memcpy(GuestPtr(base, at), text.c_str(), n);
      g_pool.used += n;
      return at;
    }

    // The pool and the fixed ids the patched scenes use (once). Under g_pool.mutex
    bool EnsurePool(uint8_t *base)
    {
      if (g_pool.tried)
        return g_pool.base != 0;

      g_pool.tried = true;
      g_pool.base = GuestAlloc(kPoolSize);

      if (!g_pool.base)
      {
        REXLOG_ERROR("[sst-menu] could not allocate guest memory for the port's texts");
        return false;
      }

      const std::pair<uint32_t, const char *> fixed[] = {
          {pause::kTextBlank, " "},
          {pause::kTextVideo, "VIDEO"},
          {quit::kTextLabel, "QUIT"},
          {quit::kTextDescription, "Close the game and return to the desktop."},
          {kTextQuitQuestion, "Are you sure you want to quit to the desktop?"},
      };

      for (auto [id, text] : fixed)
        g_pool.texts.emplace(id, Put(base, text));

      return true;
    }

    // Under g_pool.mutex
    uint32_t MakeId(uint8_t *base, const std::string &text)
    {
      if (auto it = g_pool.ids.find(text); it != g_pool.ids.end())
        return it->second;

      if (!EnsurePool(base))
        return 0;

      const uint32_t at = Put(base, text);

      if (!at)
        return 0;

      const uint32_t id = kTextIdBase + uint32_t(g_pool.ids.size());
      g_pool.ids.emplace(text, id);
      g_pool.texts.emplace(id, at);
      return id;
    }

    // The guest string for one of the port's ids; 0 if unknown. Under g_pool.mutex
    uint32_t Find(uint8_t *base, uint32_t id)
    {
      EnsurePool(base);
      const auto it = g_pool.texts.find(id);
      return it != g_pool.texts.end() ? it->second : 0;
    }

    // What the port shows instead of the game's text `id` (0: the game's)
    uint32_t Replacement(uint8_t *base, uint32_t id)
    {
      auto in = [id](const auto &list)
      { return std::find(std::begin(list), std::end(list), id) != std::end(list); };

      std::lock_guard lock(g_pool.mutex);

      if (g_restart_dialog && (id == kRestartBody || id == kRestartTitle))
        return Find(base, MakeId(base, id == kRestartBody ? "The game will now restart to apply the new settings." : "RESTART REQUIRED"));

      // QUIT dialog: the game's "RETURN TO ONLINE MENU" YES / NO message
      if (g_quit_dialog && (id == quit::kMessageBody || id == quit::kMessageTitle))
        return Find(base, id == quit::kMessageBody ? kTextQuitQuestion : quit::kTextLabel);

      if (in(kTextController) && REXCVAR_GET(sst_controls_name))
        return Find(base, MakeId(base, "CONTROLS"));

      if (in(kTextXboxLive) && REXCVAR_GET(sst_online_play_name))
        return Find(base, MakeId(base, "ONLINE PLAY"));

      if ((id & 0xFFFF0000u) == kTextIdBase)
        return Find(base, id);

      return 0;
    }
  }

  uint32_t TextId(uint8_t *base, const std::string &text)
  {
    std::lock_guard lock(g_pool.mutex);
    return MakeId(base, text);
  }

  void SetRestartDialogUp(bool up) { g_restart_dialog = up; }
  void SetQuitDialogUp(bool up) { g_quit_dialog = up; }
  bool QuitDialogUp() { return g_quit_dialog; }
}

using namespace sst::menu;

extern "C" void __imp__sub_82594AD0(PPCContext &__restrict, uint8_t *);

// SumoTool: text tables r3, text id r4 -> string (0 if not found)
extern "C" REX_FUNC(sub_82594AD0)
{
  uint32_t text = 0;

  if (sst::ach::ResolveText(base, ctx.r4.u32, text))
  {
    ctx.r3.u32 = text;
    return;
  }

  if (const uint32_t replaced = Replacement(base, ctx.r4.u32))
  {
    ctx.r3.u32 = replaced;
    return;
  }

  __imp__sub_82594AD0(ctx, base);
}