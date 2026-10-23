// What the menu hooks share. Private to src/menu.
//
// The port's menu changes, by file:
//   graphics_menu.cpp: startup (settings, first boot), the feature switches (cvars)
//   video_settings.cpp: the VIDEO settings: rows, presets, sst_settings.ini
//   video_page.cpp: the VIDEO / KEY BINDINGS page and the pause menu's VIDEO page (the "video" script's natives)
//   natives.cpp: the script VM's native calls: the port's own natives (group 0x53), and changes to the game's
//   controls_page.cpp: Settings > CONTROLS: the PlayStation diagram, KEY BINDINGS on Y, VIBRATION greyed for the keyboard
//   script_start.cpp: scripts starting: the port's scripts in place of the game's (VIDEO, intro, pause menu, main menu)
//   scene_files.cpp: scene files loading: room to grow, then the port's rows, arrows and wide layouts
//   menu_texts.cpp: texts the port adds or renames
//   camera_aspect.cpp: the cameras' aspect ratio

#pragma once

#include "menu/guest.h"

#include <rex/cvar.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

REXCVAR_DECLARE(bool, sst_graphics_menu);
REXCVAR_DECLARE(bool, sst_skip_intro);
REXCVAR_DECLARE(bool, sst_online_play_name);
REXCVAR_DECLARE(bool, sst_quit_item);
REXCVAR_DECLARE(bool, sst_language_menu);
REXCVAR_DECLARE(bool, sst_controls_name);
REXCVAR_DECLARE(bool, sst_controller_diagram);
REXCVAR_DECLARE(bool, sst_wide_layouts);

// Scene natives (thin wrappers around the scene manager 0x82DB4370): r3 result, r5 values, r6 types, r7 count
extern "C" void sub_82282BB8(PPCContext &__restrict, uint8_t *); // 4:4 show (object)
extern "C" void sub_82282C28(PPCContext &__restrict, uint8_t *); // 4:5 hide (object)
extern "C" void sub_82282AA0(PPCContext &__restrict, uint8_t *); // 4:10 animate (object, from label, to label, flags)
extern "C" void sub_82283318(PPCContext &__restrict, uint8_t *); // 4:18 alpha (object, 0-255)
extern "C" void sub_82582110(PPCContext &__restrict, uint8_t *); // scenes 0x82DB4370, object hash -> object (+0xB: alpha)

namespace sst::menu
{
  // Script hashes (lookup2 of the script's name)
  constexpr uint32_t kVideoScript = 0x4B585196;       // "video": the port's VIDEO page replaces it
  constexpr uint32_t kSettingsScript = 0x10701090;    // "options_settings": the Settings list
  constexpr uint32_t kControllerScript = 0x5ABCAE69;  // "controller": Settings > CONTROLLER
  constexpr uint32_t kSplashScript = 0x0ED21739;      // "splash": the boot intro
  constexpr uint32_t kStartScreenScript = 0x1187AE2B; // "startscreen": the title screen
  constexpr uint32_t kGeneralScript = 0x0EF38D57;     // "options_general": Settings > GENERAL (the port's list page replaces it)

  // Settings > GENERAL's fifth row (scene_rows.cpp AddGeneralRow; added to SSTMainMenu by scene_files.cpp, used by the page script and video_page.cpp): its bar and value row (shown on GENERAL, hidden on the four-row lists), label and value texts, cursor keys (row 3 -> 4)
  constexpr uint32_t kGeneralBar = 0x53474E01, kGeneralValues = 0x53474E02, kGeneralLabel = 0x53474E03, kGeneralValue = 0x53474E04;
  constexpr uint32_t kGeneralCursorFrom = 0x53474E10, kGeneralCursorTo = 0x53474E11;

  // The Settings page's list arrows (added to the SSTMainMenu scene by scene_files.cpp, shown by video_page.cpp): objects, their keys
  constexpr uint32_t kCaretUp = 0x53564301, kCaretDown = 0x53564302, kCaretHide = 0x53564310, kCaretShow = 0x53564311;

  // The port's natives (group 0x53) called by the port's scripts
  constexpr uint32_t kNativeGroup = 0x53;

  enum Native : uint32_t
  {
    // VIDEO / KEY BINDINGS page (tools/gen_graphics_script.py)
    kBegin = 0,
    kEvent = 1, // (list event)
    kLabel = 2, // (visible row 0-3) -> text id
    kValue = 3, // (visible row 0-3) -> text id

    kResult = 4,
    // In-match pause menu's VIDEO page (pause_patch.h)
    kPauseEvent = 5,
    kPauseBegin = 6,
    kPauseLabel = 7,
    kPauseValue = 8,
    kRestart = 9, // the restart dialog was confirmed
    kPauseStep = 10,

    // The port's boot intro script (tools/gen_intro_script.py)
    kIntroBegin = 11, // () forget earlier Start / A presses
    kIntroSkip = 12,  // (ms) -> 1 on a new Start / A press, or once ms have passed since kIntroTimer (ms > 0)
    kIntroTimer = 13, // () start the hold timer

    // QUIT (main_menu_quit.h)
    kQuitDialogBegin = 14,
    kQuitDialogEnd = 15,
    kQuit = 16,
    kQuitEscape = 17,

    // The page script's list-agnostic hooks (tools/gen_graphics_script.py): VIDEO or KEY BINDINGS
    kPageTitle = 18,        // -> title text id
    kPageFrame = 19,        // every frame -> 1: redraw
    kPageReturnCursor = 20, // -> Settings list cursor + 1 to leave behind
    kPageReturnScript = 21, // -> script to start on leaving
    kPageRows = 22,         // -> rows on screen (4, or 5 on GENERAL when its fifth row is in the scene)
    kPageCursorKey = 23,    // (k) cursor animation after an event: 0 from key, 1 to key; 2 the key of the cursor's row (page start)
    kRestartCancel = 24,    // the restart dialog was answered NO: the settings that need a restart go back to this run's, the page stays
  };

  // graphics_menu.cpp:
  // Guards the VIDEO settings and the page's state (video_settings.h, video_page.cpp)
  std::mutex &MenuMutex();

  // Closes the game (QUIT, or after starting the restarted copy)
  void RequestQuit();

  // menu_texts.cpp:
  // The text id of a string the port shows (made once, kept in guest memory); 0 when the pool is full
  uint32_t TextId(uint8_t *base, const std::string &text);

  // While up, the game's message boxes these replace show the port's texts
  void SetRestartDialogUp(bool up);
  void SetQuitDialogUp(bool up);
  bool QuitDialogUp();

  // video_page.cpp:
  namespace page
  {
    // A page native (kBegin .. kResult, kPause*, kPage*, kRestart): true if it was one
    bool HandleNative(PPCContext &ctx, const NativeCall &call, uint32_t index);

    // Scene animations the page asked for (played once the native has returned): show / hide an object, or animate it to a key
    struct Anim
    {
      uint32_t object, key; // key kAnimShow / kAnimHide, else 4:10 to that key
    };

    constexpr uint32_t kAnimShow = 0, kAnimHide = 1;
    std::vector<Anim> TakeAnims();

    // After the page's per-frame natives (kPageFrame; kPauseEvent, kEvent): RESOLUTION's value grayed while it is more than the monitor can show (video::ResolutionFits), its alpha back otherwise and when the page leaves. Outside the page's lock (it calls scene natives)
    void UpdateDims(PPCContext &ctx, uint8_t *base);

    // The pause menu's VIDEO page has its scroll bar up (the rest of the pause menu keeps it hidden)
    bool ScrollBarShown();

    // The pause menu is opening: its VIDEO page is not up (in case the last pause ended without leaving it)
    void PauseMenuStarted();

    // The list shows KEY BINDINGS (the VIDEO page script, CONTROLS > Y)
    bool KeyBindingsUp();

    // CONTROLS left with Y: the next page start is KEY BINDINGS. And: the page it returns to has started
    void OpenKeyBindingsNext();

    // Settings > GENERAL is starting: the next page start (the VIDEO page script) is the GENERAL list
    void OpenGeneralNext();
    void KeyBindingsClosed();
  }

  // controls_page.cpp:
  namespace controls
  {
    // A native the game's scripts call (any group but the port's). True: handled, the original was called
    bool OnNative(PPCContext &ctx, uint8_t *base, uint32_t id);

    // CONTROLS left with Y (its exit to the Settings list opens KEY BINDINGS instead); cleared when taken
    bool TakeOpenKeyBindings();

    // Calls a scene native directly, as the dispatcher would (r3 result, r4 kept, r5 values, r6 types, r7 count), not through the dispatcher, which pushes the result on the calling script's stack
    void SceneCall(PPCContext &ctx, uint8_t *base, void (*native)(PPCContext &__restrict, uint8_t *), const uint32_t *values, const uint8_t *types, uint32_t count);
    void SceneShow(PPCContext &ctx, uint8_t *base, uint32_t object, bool show);

    // A text grayed out (half alpha, as the game grays its Xbox LIVE item), called each frame: the game's own alpha changes (fades, highlights) are followed. With `dim` false it gets its alpha back
    struct Dim
    {
      uint32_t hash;
      bool active = false;
      uint8_t written = 0, original = 0xFF;
    };

    void DimText(PPCContext &ctx, uint8_t *base, Dim &d, bool dim);
  }

  // script_start.cpp:
  // `module` is the in-match pause menu's script (the game's or the port's patched copy)
  bool IsPauseMenuModule(uint32_t module);

  // The patched PauseMenu / main menu scene loaded (or not): the patched scripts need it
  void SetPauseSceneOk(bool ok);
  void SetQuitSceneOk(bool ok);

  // scene_files.cpp: GENERAL's fifth row is in the main menu scene (else the GENERAL list scrolls on four rows)
  bool GeneralRowOk();

  // camera_aspect.cpp:
  // The cameras' aspect (16:9 in the game's 4:3 mode: its cameras are already 4:3)
  float CameraAspect();
  // Writes the boot aspect into the game's camera aspect constant (base null: only the hooks' value)
  void ApplyCameraAspect(uint8_t *base);

  // scene_files.cpp; the game started wider than 16:9 (and the wide layouts are on)
  bool WideLayouts();
}