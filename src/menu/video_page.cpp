// The port's list page: Settings > VIDEO, CONTROLS > KEY BINDINGS (the same page script with another list) and the in-match pause menu's VIDEO page. The page scripts (tools/gen_graphics_script.py, pause_patch.cpp) draw a few visible rows and ask natives for everything else: the texts of
// each row, what an input event did (moved, changed a value, left, which sound), the title.

#include "input/controls.h"
#include "menu/menu_internal.h"
#include "menu/pause_patch.h"
#include "menu/restart.h"
#include "menu/video_settings.h"

#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <utility>

namespace sst::menu::page
{
  namespace
  {
    using namespace video;

    constexpr uint32_t kTextVideoGame = 0x5458AA85; // the game's own "VIDEO" (page title)

    // The Settings list's items (cursor + 1 the page leaves behind)
    constexpr int kControlsItem = 3, kVideoItem = 5;

    constexpr int kVisible = 4;                 // Settings > VIDEO: GENERAL's four rows
    constexpr int kPauseVisible = pause::kRows; // in-match pause menu: seven
    static_assert(pause::kScrollCount == kPauseRowCount && pause::kScrollVisible == kPauseVisible, "the pause page's scroll bar is built for its list");

    // The page script's list events (kEvent) and the pause menu's input event hashes (kPauseEvent)
    enum Event : int
    {
      kLeft = 0,
      kRight,
      kUp,
      kDown,
      kSelect,
      kBack
    };

    constexpr std::pair<uint32_t, int> kPauseEvents[] = {{0xF5BF6BF3, kLeft}, {0x5856F909, kRight}, {0xB0A4EB7F, kUp}, {0xA3AD84E1, kDown}, {0x2FAB294A, kSelect}, {0xA979AAA1, kBack}};

    // What the page script plays after an event
    enum Sound : int
    {
      kNoSound = 0,
      kMoveSound = 1,
      kSelectSound = 2,
      kBackSound = 3
    };

    // The page's list and what the last event did (kResult reads it back)
    struct List
    {
      int visible = kVisible; // rows on screen
      bool pause = false;     // the in-match page: no rows that need a restart
      bool keys = false;      // CONTROLS > KEY BINDINGS instead of VIDEO
      int cursor = 0, top = 0;

      // The last event's result
      int old_visible = 0, new_visible = 0; // cursor's line on screen before / after
      bool redraw = false, leave = false;
      int sound = kNoSound;
      bool dialog = false; // show the restart dialog

      int Count() const { return keys ? sst::input::KeyBindingCount() : pause ? kPauseRowCount
                                                                              : kRows; }

      // List position -> settings Row
      int Row(int i) const { return !keys && pause ? kPauseRows[std::clamp(i, 0, kPauseRowCount - 1)] : i; }

      void NothingHappened()
      {
        old_visible = new_visible = cursor - top;
        redraw = leave = dialog = false;
        sound = kNoSound;
      }
    };

    // What the "more rows" markers show (-1: not set on this page yet)
    struct Markers
    {
      int up = -1, down = -1;   // Settings page: arrows above / below the four rows
      int shown = -1, pos = -1; // pause page: scroll bar shown, thumb position
    };

    // Under MenuMutex
    List g_list;
    Markers g_markers;
    Settings g_snapshot; // the settings when the page opened (Back restores them)
    std::vector<Anim> g_anims;
    bool g_keys_next = false;   // the next page start is KEY BINDINGS
    uint32_t g_keys_y_seen = 0; // sst::input::YPresses() handled on the KEY BINDINGS page
    std::atomic<bool> g_scroll_bar{false};

    // Markers
    void UpdateMarkers()
    {
      const List &l = g_list;
      Markers &m = g_markers;

      if (l.pause)
      {
        const int pos = std::clamp(l.top, 0, pause::kScrollCount - pause::kScrollVisible);

        if (m.shown != 1)
        {
          g_anims.push_back({pause::kScrollGroup, kAnimShow});
          m.shown = 1;
          g_scroll_bar = true;
        }

        if (pos != m.pos)
        {
          g_anims.push_back({pause::kScrollThumb, pause::kScrollPos + uint32_t(pos)});
          m.pos = pos;
        }

        return;
      }

      const int up = l.top > 0, down = l.top + l.visible < l.Count();

      if (up != m.up)
      {
        g_anims.push_back({kCaretUp, up ? kCaretShow : kCaretHide});
        m.up = up;
      }

      if (down != m.down)
      {
        g_anims.push_back({kCaretDown, down ? kCaretShow : kCaretHide});
        m.down = down;
      }
    }

    void HideMarkers()
    {
      if (g_list.pause)
      {
        g_anims.push_back({pause::kScrollGroup, kAnimHide});
        g_scroll_bar = false;
      }
      else
      {
        g_anims.push_back({kCaretUp, kCaretHide});
        g_anims.push_back({kCaretDown, kCaretHide});
      }

      g_markers = {};
    }

    // The restart dialog was confirmed: start the new copy, then close this one (forced after a few seconds if the normal shutdown stalls)
    void Restart()
    {
      REXLOG_INFO("[sst-menu] restarting for the aspect ratio / shader precision change");

      if (!RestartGame())
      {
        REXLOG_ERROR("[sst-menu] restart failed: the change applies at the next start");
        return;
      }

      RequestQuit();
      ExitSoon(3000);
    }

    // KEY BINDINGS: select waits for a key (or toggles / resets a row), back leaves; nothing while a key is being taken
    void KeyBindingsEvent(int event)
    {
      List &l = g_list;

      if ((event == kSelect || event == kLeft || event == kRight) && sst::input::KeyBindingToggle(l.cursor))
      {
        l.redraw = true;
        l.sound = kMoveSound;
      }
      else if (event == kSelect)
      {
        sst::input::BeginKeyCapture(l.cursor);
        l.redraw = true;
        l.sound = kSelectSound;
      }
      else if (event == kBack)
      {
        REXLOG_INFO("[sst-menu] KEY BINDINGS closed");
        l.leave = true;
        l.sound = kBackSound;
      }
    }

    void MoveCursor(int event)
    {
      List &l = g_list;
      const int n = l.Count();
      l.cursor = (l.cursor + (event == kDown ? 1 : n - 1)) % n;
      const int top = std::clamp(l.top, std::max(0, l.cursor - l.visible + 1), std::min(l.cursor, n - l.visible));

      if (top != l.top)
      {
        l.top = top;
        l.redraw = true;
      }

      l.sound = kMoveSound;
    }

    void SettingsEvent(int event)
    {
      List &l = g_list;
      Settings &current = Current();

      switch (event)
      {
      case kLeft:

      case kRight:
      {
        const Settings before = current;
        Change(current, l.Row(l.cursor), event == kRight ? 1 : -1);

        if (current != before)
        {
          Apply(current);
          l.redraw = true;
          l.sound = kMoveSound;
          REXLOG_INFO("[sst-menu] {}", Describe(current));
        }
        break;
      }

      case kUp:

      case kDown:
        MoveCursor(event);
        break;

      case kSelect:
        Save(current);
        REXLOG_INFO("[sst-menu] VIDEO saved: {}", Describe(current));
        l.sound = kSelectSound;

        if (!l.pause && RestartNeeded())
        {
          l.dialog = true;
          SetRestartDialogUp(true);
          break;
        }

        l.leave = true;
        break;

      case kBack:
        if (current != g_snapshot)
        {
          current = g_snapshot;
          Apply(current);
        }

        REXLOG_INFO("[sst-menu] VIDEO cancelled: {}", Describe(current));
        l.leave = true;
        l.sound = kBackSound;
        break;
      }
    }

    void HandleEvent(int event)
    {
      List &l = g_list;
      l.NothingHappened();

      if (l.keys && sst::input::KeyCaptureActive())
        return; // nothing while a key is being taken

      if (l.keys && event != kUp && event != kDown)
        KeyBindingsEvent(event);
      else
        SettingsEvent(event);

      l.new_visible = l.cursor - l.top;
    }

    void Begin(bool pause)
    {
      g_snapshot = Current();
      g_list = {};
      g_list.visible = pause ? kPauseVisible : kVisible;
      g_list.pause = pause;
      g_list.keys = !pause && g_keys_next;
      g_keys_next = false;
      g_keys_y_seen = sst::input::YPresses();
      g_markers = {};

      if (g_list.keys)
        REXLOG_INFO("[sst-menu] KEY BINDINGS page opened");
      else
        REXLOG_INFO("[sst-menu] VIDEO page opened{}: {}", pause ? " (pause menu)" : "", Describe(Current()));
    }

    // Every frame: KEY BINDINGS redraws when a key was taken (or the wait cancelled); Y (not while a key is being taken) resets them
    bool Frame()
    {
      if (!g_list.keys)
        return false;

      const int captured = sst::input::PollKeyCapture();
      const uint32_t y = sst::input::YPresses();

      if (captured || sst::input::KeyCaptureActive())
      {
        g_keys_y_seen = y;
        return captured != 0;
      }

      if (y == g_keys_y_seen)
        return false;

      g_keys_y_seen = y;
      sst::input::ResetKeyBindings();
      return true;
    }

    // kResult k: 0 the cursor's old line, 1 its new line, 2 redraw, 3 leave, 4 sound, 5 show the restart dialog
    int Result(int k)
    {
      const List &l = g_list;

      // (Pause page: lines 0-6 are drawn on the rows in their screen order)
      auto line = [&](int i)
      { return l.pause && i >= 0 && i < pause::kRows ? pause::kVisualOrder[i] : i; };

      switch (k)
      {
      case 0:
        return line(l.old_visible);
      case 1:
        return line(l.new_visible);
      case 2:
        return l.redraw;
      case 3:
        return l.leave;
      case 4:
        return l.sound;
      default:
        return l.dialog;
      }
    }

    // A visible row's label or value (pause: asked by the pause menu's page)
    std::string RowText(int visible_row, bool label, bool pause)
    {
      const List &l = g_list;

      if (pause)
      {
        const int row = l.Row(l.top + std::clamp(visible_row, 0, kPauseVisible - 1));
        return label ? PauseLabel(row) : PauseValue(Current(), row);
      }

      const int row = l.top + std::clamp(visible_row, 0, kVisible - 1);

      if (l.keys)
        return label ? std::string(sst::input::KeyBindingLabel(row)) : sst::input::KeyBindingValue(row);

      return label ? std::string(kRowInfo[row].label) : ValueText(Current(), row);
    }

    uint32_t PauseEvent(uint32_t hash)
    {
      for (auto [h, e] : kPauseEvents)
      {
        if (hash == h)
          return uint32_t(e);
      }

      return UINT32_MAX;
    }
  }

  bool HandleNative(const NativeCall &call, uint32_t index)
  {
    uint8_t *base = call.base;
    std::lock_guard lock(MenuMutex());

    switch (index)
    {
    case kBegin:

    case kPauseBegin:
      Begin(index == kPauseBegin);
      call.ReturnInt(0);
      return true;

    case kPageTitle:
      call.ReturnHash(g_list.keys ? TextId(base, "KEY BINDINGS") : kTextVideoGame);
      return true;

    case kPageFrame:
      call.ReturnInt(Frame());
      return true;

    case kPageReturnCursor:
      call.ReturnInt(g_list.keys ? kControlsItem : kVideoItem);
      return true;

    case kPageReturnScript:
      call.ReturnHash(g_list.keys ? kControllerScript : kSettingsScript);
      g_list.keys = false; // (the page is closing)
      return true;

    case kEvent:

    case kPauseEvent:
    {
      const uint32_t event = index == kEvent ? uint32_t(call.Int(0)) : PauseEvent(call.Raw(0));

      if (event <= kBack)
        HandleEvent(int(event));
      else
        g_list.NothingHappened();

      if (g_list.leave)
        HideMarkers();

      call.ReturnInt(0);
      return true;
    }

    case kLabel:

    case kValue:

    case kPauseLabel:

    case kPauseValue:
    {
      const bool label = index == kLabel || index == kPauseLabel;
      call.ReturnHash(TextId(base, RowText(call.Int(0), label, index == kPauseLabel || index == kPauseValue)));

      if (label)
        UpdateMarkers();

      return true;
    }

    case kResult:
      call.ReturnInt(Result(call.Int(0)));
      return true;

    case kRestart:
      SetRestartDialogUp(false);
      Restart();
      call.ReturnInt(0);
      return true;
    }

    return false;
  }

  std::vector<Anim> TakeAnims()
  {
    std::lock_guard lock(MenuMutex());
    return std::exchange(g_anims, {});
  }

  bool ScrollBarShown() { return g_scroll_bar; }

  bool KeyBindingsUp()
  {
    std::lock_guard lock(MenuMutex());
    return g_list.keys;
  }

  void OpenKeyBindingsNext()
  {
    std::lock_guard lock(MenuMutex());
    g_keys_next = true;
  }

  void KeyBindingsClosed()
  {
    std::lock_guard lock(MenuMutex());
    g_list.keys = false;
  }
}