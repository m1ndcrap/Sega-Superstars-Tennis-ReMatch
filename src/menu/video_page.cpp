// The port's list page: Settings > VIDEO, CONTROLS > KEY BINDINGS (the same page script with another list) and the in-match pause menu's VIDEO page. The page scripts (tools/gen_graphics_script.py, pause_patch.cpp) draw a few visible rows and ask natives for everything else: the texts of
// each row, what an input event did (moved, changed a value, left, which sound), the title.

#include "input/controls.h"
#include "menu/language.h"
#include "menu/menu_internal.h"
#include "menu/pause_patch.h"
#include "menu/restart.h"
#include "menu/video_settings.h"

#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

#include <fmt/format.h>

// The game's settings natives (group 9; r3 result {value, type}, r5 values) the GENERAL list uses, as the game's options_general script does
extern "C" void sub_822809B0(PPCContext &__restrict, uint8_t *); // 9:209 difficulty -> 0-2
extern "C" void sub_822809F8(PPCContext &__restrict, uint8_t *); // 9:210 (difficulty)
extern "C" void sub_8227EC08(PPCContext &__restrict, uint8_t *); // 9:117 superstar mode on
extern "C" void sub_8227EC58(PPCContext &__restrict, uint8_t *); // 9:118 superstar mode full
extern "C" void sub_8227EA88(PPCContext &__restrict, uint8_t *); // 9:115 (superstar mode 0 off, 1 quick, 2 full)
extern "C" void sub_8227DAF8(PPCContext &__restrict, uint8_t *); // 9:81 fix camera (0 on, 1 off)
extern "C" void sub_8227DB28(PPCContext &__restrict, uint8_t *); // 9:82 (fix camera)
extern "C" void sub_8227D8D0(PPCContext &__restrict, uint8_t *); // 9:74 speed units (0 KPH, 1 MPH)
extern "C" void sub_8227D880(PPCContext &__restrict, uint8_t *); // 9:72 (speed units)

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
      bool general = false;   // Settings > GENERAL: the game's four settings and LANGUAGE
      int cursor = 0, top = 0;

      // The last event's result
      int old_visible = 0, new_visible = 0; // cursor's line on screen before / after
      bool redraw = false, leave = false;
      int sound = kNoSound;
      bool dialog = false; // show the restart dialog

      int Count() const;

      // List position -> settings Row
      int Row(int i) const { return !keys && pause ? kPauseRows[std::clamp(i, 0, kPauseRowCount - 1)] : i; }

      void NothingHappened()
      {
        old_visible = new_visible = cursor - top;
        redraw = leave = dialog = false;
        sound = kNoSound;
      }
    };

    // Settings > GENERAL (the game's options_general script is replaced by this page): DIFFICULTY, SUPERSTAR MODE, FIX CAMERA and SPEED (the game's settings, changed live through its natives as its script does; Back puts them back) and LANGUAGE (the port's, menu/language.h: saved on Select, a
    // restart when the language changes). Labels and the game rows' values are the game's own text ids (so they follow the language); LANGUAGE's are the port's.
    namespace general
    {
      enum Row : int
      {
        kDifficulty = 0,
        kSuperstar,
        kFixCamera,
        kSpeed,
        kLanguage,
        kCount
      };

      constexpr int kStates[kCount] = {3, 3, 2, 2, sst::lang::kChoices};
      constexpr uint32_t kTitle = 0x6A94D0C1;                                                    // "GENERAL"
      constexpr uint32_t kLabels[kSpeed + 1] = {0x17E129F2, 0x6DD403F0, 0x8302CC03, 0x0880BE5D}; // DIFFICULTY, SUPERSTAR MODE, FIX CAMERA, SPEED

      constexpr uint32_t kValues[kSpeed + 1][3] = {
          {0x7F0D720B, 0x2C5C0EEF, 0x62F200D4}, // EASY, NORMAL, HARD
          {0x1D2E9662, 0x6DD0FF6D, 0xF1674504}, // OFF, QUICK, FULL
          {0x30A031AF, 0xD667AC41, 0},          // 0 ON, 1 OFF (the game's fix camera value)
          {0xE253D66D, 0xF491294E, 0},          // 0 KPH, 1 MPH
      };

      constexpr int kSettingsItem = 1; // GENERAL is the Settings list's first item (cursor + 1)
      constexpr int kRows = 5;         // on screen when the scene has the fifth row (GeneralRowOk)

      struct State
      {
        int v[kCount] = {};
        int snapshot[kCount] = {};
      };

      State g_state;
      bool g_next = false;         // the next page start is GENERAL
      PPCContext *g_ctx = nullptr; // the native call being handled (for the game's natives)
      uint8_t *g_base = nullptr;
      uint32_t g_scratch = 0;

      // Calls one of the game's natives with integer arguments -> its result value (0 if it can't be called)
      int GameNative(void (*native)(PPCContext &__restrict, uint8_t *), std::initializer_list<int> args)
      {
        if (!g_ctx || !g_base)
          return 0;

        if (!g_scratch)
          g_scratch = GuestAlloc(64);

        if (!g_scratch)
          return 0;

        std::memset(GuestPtr(g_base, g_scratch), 0, 48);
        uint32_t n = 0;

        for (int a : args)
        {
          Store32(g_base, g_scratch + 8 + 4 * n, uint32_t(a));
          *GuestPtr(g_base, g_scratch + 40 + n) = uint8_t(kInt);
          ++n;
        }

        PPCContext c = *g_ctx;
        c.r3.u64 = g_scratch;
        c.r5.u64 = g_scratch + 8;
        c.r6.u64 = g_scratch + 40;
        c.r7.u64 = n;
        native(c, g_base);
        return int32_t(Load32(g_base, g_scratch));
      }

      void Read(State &s)
      {
        s.v[kDifficulty] = std::clamp(GameNative(sub_822809B0, {}), 0, 2);
        s.v[kSuperstar] = GameNative(sub_8227EC08, {}) ? (GameNative(sub_8227EC58, {}) ? 2 : 1) : 0;
        s.v[kFixCamera] = GameNative(sub_8227DAF8, {}) ? 1 : 0;
        s.v[kSpeed] = GameNative(sub_8227D8D0, {}) ? 1 : 0;
        s.v[kLanguage] = sst::lang::Choice();
      }

      // A game setting to the game (LANGUAGE is only saved on Select)
      void Write(int row, int value)
      {
        switch (row)
        {
        case kDifficulty:
          GameNative(sub_822809F8, {value});
          break;

        case kSuperstar:
          GameNative(sub_8227EA88, {value});
          break;

        case kFixCamera:
          GameNative(sub_8227DB28, {value});
          break;

        case kSpeed:
          GameNative(sub_8227D880, {value});
          break;

        default:
          break;
        }
      }

      std::string Describe(const State &s)
      {
        return fmt::format("difficulty {}, superstar mode {}, fix camera {}, speed {}, language {}", s.v[kDifficulty], s.v[kSuperstar], s.v[kFixCamera] ? "off" : "on", s.v[kSpeed] ? "MPH" : "KPH", sst::lang::ChoiceLabel(s.v[kLanguage]));
      }

      uint32_t Label(uint8_t *base, int row)
      {
        return row < kLanguage ? kLabels[row] : TextId(base, "LANGUAGE");
      }

      uint32_t Value(uint8_t *base, int row)
      {
        const int v = g_state.v[row];
        return row < kLanguage ? kValues[row][std::clamp(v, 0, kStates[row] - 1)] : TextId(base, sst::lang::ChoiceLabel(v));
      }
    }

    // What the "more rows" markers show (-1: not set on this page yet)
    struct Markers
    {
      int up = -1, down = -1;   // Settings page: arrows above / below the four rows
      int shown = -1, pos = -1; // pause page: scroll bar shown, thumb position
    };

    int List::Count() const
    {
      return keys ? sst::input::KeyBindingCount() : general ? general::kCount
                                                : pause     ? kPauseRowCount
                                                            : kRows;
    }

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
      REXLOG_INFO("[sst-menu] restarting to apply the new settings");

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

    void GeneralEvent(int event)
    {
      using namespace general;
      List &l = g_list;
      State &s = g_state;

      switch (event)
      {
      case kLeft:

      case kRight:
      {
        const int row = l.cursor, n = kStates[row];
        s.v[row] = (s.v[row] + (event == kRight ? 1 : n - 1)) % n; // (wraps, as the game's page)
        Write(row, s.v[row]);
        l.redraw = true;
        l.sound = kMoveSound;
        REXLOG_INFO("[sst-menu] GENERAL: {}", Describe(s));
        break;
      }

      case kUp:

      case kDown:
        MoveCursor(event);
        break;

      case kSelect:
      {
        l.sound = kSelectSound;
        const int choice = s.v[kLanguage];

        if (choice != s.snapshot[kLanguage])
        {
          sst::lang::SaveChoice(choice);

          // A restart when the language this boot would pick changes
          if (sst::lang::ChoiceLanguage(choice) != sst::lang::Current())
          {
            REXLOG_INFO("[sst-menu] GENERAL saved: {} (restart for the language)", Describe(s));
            l.dialog = true;
            SetRestartDialogUp(true);
            break;
          }
        }

        REXLOG_INFO("[sst-menu] GENERAL saved: {}", Describe(s));
        l.leave = true;
        break;
      }

      case kBack:
        for (int row = 0; row < kLanguage; ++row)
        {
          if (s.v[row] != s.snapshot[row])
            Write(row, s.snapshot[row]);
        }

        std::copy(std::begin(s.snapshot), std::end(s.snapshot), std::begin(s.v));
        REXLOG_INFO("[sst-menu] GENERAL cancelled: {}", Describe(s));
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
      else if (l.general)
        GeneralEvent(event);
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
      g_list.general = !pause && !g_list.keys && general::g_next;
      g_keys_next = false;
      general::g_next = false;
      g_keys_y_seen = sst::input::YPresses();
      g_markers = {};

      if (g_list.general)
      {
        if (GeneralRowOk())
          g_list.visible = general::kRows;

        general::Read(general::g_state);
        std::copy(std::begin(general::g_state.v), std::end(general::g_state.v), std::begin(general::g_state.snapshot));
        REXLOG_INFO("[sst-menu] GENERAL page opened: {}", general::Describe(general::g_state));
      }
      else if (g_list.keys)
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

    // The restart dialog was answered NO: what needed the restart goes back to this run's (and is saved so), the page stays open
    void CancelRestart()
    {
      List &l = g_list;

      if (l.general)
      {
        general::State &s = general::g_state;

        if (s.v[general::kLanguage] != s.snapshot[general::kLanguage])
        {
          s.v[general::kLanguage] = s.snapshot[general::kLanguage];
          sst::lang::SaveChoice(s.v[general::kLanguage]);
        }

        REXLOG_INFO("[sst-menu] restart cancelled: GENERAL {}", general::Describe(s));
      }
      else
      {
        Settings &current = Current();
        current.v[kAspect] = Boot().aspect;
        current.v[kShaderPrecision] = Boot().precision;
        Apply(current);
        Save(current);
        g_snapshot = current;
        REXLOG_INFO("[sst-menu] restart cancelled: VIDEO {}", Describe(current));
      }

      l.NothingHappened();
      l.redraw = true;
    }

    // The cursor's animation keys (the GENERAL layout's cursor group): rows 0-3 as the game's page; the fifth row has its own stretch from row 3 (kGeneralCursorFrom -> kGeneralCursorTo); jumps to and from it otherwise (wrapping)
    uint32_t CursorKey(int k)
    {
      constexpr uint32_t kKeys[4] = {0xAFF91553, 0x417E03CC, 0xFFB6C545, 0x4318364C};
      const List &l = g_list;

      auto key = [&](int line)
      { return line >= 4 ? kGeneralCursorTo : kKeys[std::clamp(line, 0, 3)]; };

      const int from = l.old_visible, to = l.new_visible;

      if (k >= 2)
        return key(to);

      if (from < 4 && to < 4)
        return key(k == 0 ? from : to);

      if (from == 3 && to == 4)
        return k == 0 ? kGeneralCursorFrom : kGeneralCursorTo;

      if (from == 4 && to == 3)
        return k == 0 ? kGeneralCursorTo : kGeneralCursorFrom;

      return key(to); // (wrapping to / from the fifth row: no animation)
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

  bool HandleNative(PPCContext &ctx, const NativeCall &call, uint32_t index)
  {
    uint8_t *base = call.base;
    std::lock_guard lock(MenuMutex());
    general::g_ctx = &ctx;
    general::g_base = base;

    struct ClearContext
    {
      ~ClearContext() { general::g_ctx = nullptr; }
    } clear_context;

    switch (index)
    {
    case kBegin:

    case kPauseBegin:
      Begin(index == kPauseBegin);
      call.ReturnInt(0);
      return true;

    case kPageTitle:
      call.ReturnHash(g_list.keys ? TextId(base, "KEY BINDINGS") : g_list.general ? general::kTitle
                                                                                  : kTextVideoGame);
      return true;

    case kPageFrame:
      call.ReturnInt(Frame());
      return true;

    case kPageReturnCursor:
      call.ReturnInt(g_list.keys ? kControlsItem : g_list.general ? general::kSettingsItem
                                                                  : kVideoItem);
      return true;

    case kPageReturnScript:
      call.ReturnHash(g_list.keys ? kControllerScript : kSettingsScript);
      g_list.keys = false; // (the page is closing)
      g_list.general = false;
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
      const bool pause_row = index == kPauseLabel || index == kPauseValue;

      if (g_list.general && !pause_row)
      {
        const int line = call.Int(0);

        if (line < 0 || line >= g_list.visible)
        {
          call.ReturnHash(TextId(base, " "));
        }
        else
        {
          const int row = std::clamp(g_list.top + line, 0, general::kCount - 1);
          call.ReturnHash(label ? general::Label(base, row) : general::Value(base, row));
        }
      }
      else if (!pause_row && call.Int(0) >= g_list.visible)
      {
        call.ReturnHash(TextId(base, " ")); // (the fifth row's texts on the four-row lists: hidden anyway)
      }
      else
      {
        call.ReturnHash(TextId(base, RowText(call.Int(0), label, pause_row)));
      }

      if (label)
        UpdateMarkers();

      return true;
    }

    case kResult:
      call.ReturnInt(Result(call.Int(0)));
      return true;

    case kPageRows:
      call.ReturnInt(g_list.visible);
      return true;

    case kPageCursorKey:
      call.ReturnHash(CursorKey(call.Int(0)));
      return true;

    case kRestart:
      SetRestartDialogUp(false);
      Restart();
      call.ReturnInt(0);
      return true;

    case kRestartCancel:
      SetRestartDialogUp(false);
      CancelRestart();
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

  void OpenGeneralNext()
  {
    std::lock_guard lock(MenuMutex());
    general::g_next = true;
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