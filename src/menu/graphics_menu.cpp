// sst_rematch - in-game VIDEO page

#include "menu/graphics_menu.h"
#include "input/controls.h"
#include "menu/hud_anchor.h"
#include "menu/main_menu_quit.h"
#include "menu/pause_patch.h"
#include "menu/restart.h"
#include "menu/scene_widen.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_BOOL(sst_skip_intro, true, "SST", "The boot intro (SEGA movie, logo cards) can be skipped with Start / A (Enter on the keyboard)");
REXCVAR_DEFINE_BOOL(sst_online_play_name, true, "SST", "Main menu: the Xbox LIVE item is called ONLINE PLAY");
REXCVAR_DEFINE_BOOL(sst_quit_item, true, "SST", "Main menu: a QUIT item below OPTIONS (asks, then closes the game; Esc on the main menu asks too)");
REXCVAR_DEFINE_BOOL(sst_controls_name, true, "SST", "Settings: the CONTROLLER page is called CONTROLS");
REXCVAR_DEFINE_BOOL(sst_controller_diagram, true, "SST", "Settings > CONTROLLER shows the DualShock 3 diagram while a PlayStation pad is in use");
REXCVAR_DEFINE_BOOL(sst_wide_layouts, true, "SST",
                    "Ultrawide aspect ratios: menus, loading screens and backgrounds get "
                    "layouts that fill the extra width (generated from the 16:9 ones)");

REXCVAR_DEFINE_BOOL(sst_graphics_menu, true, "SST",
                    "Replace Options > Settings > VIDEO with the port's VIDEO page");

REXCVAR_DEFINE_INT32(sst_resolution, 720, "SST",
                     "Internal resolution in lines: 480, 720, 1080, 1440, 2160 (VIDEO page)");

REXCVAR_DEFINE_INT32(sst_render_scale, 100, "SST",
                     "3D render scale in percent of the internal resolution, 50-100 (the "
                     "UI stays at the internal resolution; VIDEO page)");

REXCVAR_DEFINE_INT32(sst_shadow_size, 1024, "SST",
                     "Shadow map size: 0 (shadows off), 128-2048 (the game's is 1024; 3D textures stop at 2048 in D3D12; VIDEO page)");

REXCVAR_DEFINE_INT32(sst_aa_type, 0, "SST", "Anti-aliasing: 0 off, 1 FXAA, 2 MSAA (VIDEO page)");

REXCVAR_DEFINE_INT32(sst_aa_level, 4, "SST", "MSAA samples: 2, 4, 8 (VIDEO page)");

REXCVAR_DEFINE_INT32(sst_aspect, 1, "SST",
                     "Aspect ratio: 0 4:3, 1 16:9, 2 16:10, 3 21:9, 4 32:9 (VIDEO page)");

REXCVAR_DEFINE_INT32(sst_fps_limit, 60, "SST", "Frame rate limit: 30 or 60 (VIDEO page)");

REXCVAR_DEFINE_BOOL(sst_vsync, true, "SST", "VSync (VIDEO page)");

REXCVAR_DEFINE_INT32(sst_brightness, 50, "SST",
                     "Brightness 0-100, 50 = the game's default (VIDEO page)");

REXCVAR_DEFINE_BOOL(sst_fullscreen, false, "SST", "Borderless fullscreen (VIDEO page)");

REXCVAR_DEFINE_BOOL(sst_bloom, true, "SST", "Bloom / glow (VIDEO page)");

REXCVAR_DEFINE_BOOL(sst_fps_counter, false, "SST", "FPS counter in the top-left corner (VIDEO page)");

namespace sst::menu
{
  namespace
  {
#include "menu/graphics_script.inc"
#include "menu/intro_script.inc"

    constexpr uint32_t kVideoScript = 0x4B585196; // lookup2("video")
    constexpr uint32_t kNativeGroup = 0x53;
    enum Native : uint32_t
    {
      kBegin = 0,
      kEvent = 1,
      kLabel = 2,
      kValue = 3,
      kResult = 4,
      // In-match pause menu page (pause_patch.h)
      kPauseEvent = pause::kNativeEventHash,
      kPauseBegin = pause::kNativeBegin,
      kPauseLabel = pause::kNativeLabel,
      kPauseValue = pause::kNativeValue,
      kRestart = 9, // the restart dialog was confirmed
      kPauseStep = pause::kNativeStep,
      // The port's boot intro script (tools/gen_intro_script.py)
      kIntroBegin = 11, // () forget earlier Start / A presses
      kIntroSkip = 12,  // (ms) -> 1 on a new Start / A press, or once ms have passed since kIntroTimer (ms > 0)
      kIntroTimer = 13, // () start the hold timer
      // QUIT (main_menu_quit.h)
      kQuitDialogBegin = quit::kNativeDialogBegin,
      kQuitDialogEnd = quit::kNativeDialogEnd,
      kQuit = quit::kNativeQuit,
      kQuitEscape = quit::kNativeEscape,
      // The page script's list-agnostic hooks (tools/gen_graphics_script.py): VIDEO or KEY BINDINGS
      kPageTitle = 18,        // -> title text id
      kPageFrame = 19,        // every frame -> 1: redraw
      kPageReturnCursor = 20, // -> Settings list cursor + 1 to leave behind
      kPageReturnScript = 21, // -> script to start on leaving
    };
    constexpr uint32_t kSettingsScript = 0x10701090;   // lookup2("options_settings")
    // Settings page arrows (SSTMainMenu scene, widen::AddListCarets): objects and their keys
    constexpr uint32_t kCaretUp = 0x53564301, kCaretDown = 0x53564302, kCaretHide = 0x53564310, kCaretShow = 0x53564311;
    constexpr uint32_t kControllerScript = 0x5ABCAE69; // lookup2("controller")
    constexpr uint32_t kTextVideoGame = 0x5458AA85;    // the game's own "VIDEO"
    constexpr uint32_t kSplashScript = 0x0ED21739; // lookup2("splash")

    static_assert(pause::kNativeResult == kResult);
    enum Event : int
    {
      kLeft = 0,
      kRight,
      kUp,
      kDown,
      kSelect,
      kBack
    };

    enum Sound : int
    {
      kNoSound = 0,
      kMove = 1,
      kSelectSound = 2,
      kBackSound = 3
    };

    // the list
    enum Row : int
    {
      kPreset,
      kFullscreen,
      kShadows,
      kBloom,
      kResolution,
      kAspect,
      kRenderScale,
      kAaType,
      kAaLevel,
      kFpsLimit,
      kFpsCounter,
      kVSync,
      kBrightness,
      kRows
    };

    struct RowInfo
    {
      const char *key;   // sst_settings.ini
      const char *label; // shown (the label font has A-Z, 0-4, '-', ':', '/')
      int count;         // values 0..count-1
      bool slider;       // stops at the ends instead of wrapping
    };

    constexpr std::array<RowInfo, kRows> kList = {{
        {"preset", "QUALITY PRESET", 4, false},
        {"fullscreen", "FULLSCREEN", 2, false},
        {"shadows", "SHADOWS", 6, false},
        {"bloom", "BLOOM", 2, false},
        {"resolution", "RESOLUTION", 5, false},
        {"aspect_ratio", "ASPECT RATIO", 5, false},
        {"render_scale", "RENDER SCALE", 11, true},
        {"aa_type", "ANTI-ALIASING", 3, false},
        {"aa_level", "AA LEVEL", 3, false},
        {"fps_limit", "FPS LIMIT", 2, false},
        {"fps_counter", "FPS COUNTER", 2, false},
        {"vsync", "VSYNC", 2, false},
        {"brightness", "BRIGHTNESS", 21, true},
    }};
    constexpr int kVisible = 4;                 // Settings > VIDEO (GENERAL's four rows)
    constexpr int kPauseVisible = pause::kRows; // in-match pause menu
    constexpr int kPresetCustom = 3;
    constexpr int kShadowSizes[6] = {0, 128, 256, 512, 1024, 2048};
    constexpr int kResolutions[5] = {480, 720, 1080, 1440, 2160};
    constexpr const char *kAspects[5] = {"4:3", "16:9", "16:10", "21:9", "32:9"};
    constexpr int kAaLevels[3] = {2, 4, 8};

    struct Settings
    {
      std::array<int, kRows> v = {1, 0, 4, 1, 1, 1, 10, 0, 1, 1, 0, 1, 10};
    };

    // Quality presets: shadows, resolution, render scale, AA type, AA level
    struct Preset
    {
      int shadows, resolution, render_scale, aa_type, aa_level;
    };

    constexpr Preset kPresets[3] = {
        {3, 1, 10, 0, 1}, // LOW: 512 shadows, 720p, 100%, AA off
        {4, 2, 10, 1, 1}, // MEDIUM: 1024 shadows, 1080p, FXAA
        {5, 3, 10, 2, 1}, // HIGH: 2048 shadows, 1440p, MSAA 4x
    };

    void ApplyPreset(Settings &s, int p)
    {
      const Preset &q = kPresets[p];
      s.v[kShadows] = q.shadows;
      s.v[kResolution] = q.resolution;
      s.v[kRenderScale] = q.render_scale;
      s.v[kAaType] = q.aa_type;
      s.v[kAaLevel] = q.aa_level;
      s.v[kPreset] = p;
    }

    void UpdatePreset(Settings &s)
    {
      s.v[kPreset] = kPresetCustom;

      for (int p = 0; p < 3; ++p)
      {
        const Preset &q = kPresets[p];

        if (s.v[kShadows] == q.shadows && s.v[kResolution] == q.resolution && s.v[kRenderScale] == q.render_scale && s.v[kAaType] == q.aa_type && (q.aa_type != 2 || s.v[kAaLevel] == q.aa_level))
          s.v[kPreset] = p;
      }
    }

    bool g_boot_four_three = false; // ASPECT RATIO 4:3 at startup (see NativeFourThree)
    int g_boot_aspect = 1;          // ASPECT RATIO at startup: the one in use until a restart

    std::string ValueText(const Settings &s, int row)
    {
      const int v = s.v[row];

      switch (row)
      {
      case kPreset:
        return v == 0 ? "LOW" : v == 1 ? "MEDIUM"
                            : v == 2   ? "HIGH"
                                       : "CUSTOM";

      case kFullscreen:
      case kBloom:
      case kFpsCounter:
      case kVSync:
        return v ? "ON" : "OFF";

      case kShadows:
        return v == 0 ? "OFF" : std::to_string(kShadowSizes[v]);

      case kResolution:
        return std::to_string(kResolutions[v]) + "P";

      case kAspect: // layouts are set up as scenes load: a change needs a restart
        return v != g_boot_aspect ? std::string(kAspects[v]) + " (RESTART)" : std::string(kAspects[v]);

      case kRenderScale:
        return std::to_string(50 + 5 * v) + "%";

      case kAaType:
        return v == 0 ? "OFF" : v == 1 ? "FXAA"
                                       : "MSAA";

      case kAaLevel:
        return s.v[kAaType] == 2 ? std::to_string(kAaLevels[v]) + "X" : "N/A";

      case kFpsLimit:
        return v ? "60" : "30";

      case kBrightness:
        return std::to_string(5 * v);
      }

      return "";
    }

    std::string Describe(const Settings &s)
    {
      std::string out;

      for (int r = 0; r < kRows; ++r)
      {
        if (r)
          out += ", ";

        out += kList[r].key;
        out += ' ';
        out += ValueText(s, r);
      }

      return out;
    }

    // The pause menu's fonts only have some glyphs: the row font (labels) A-Y without J/Q, 1-4, " ().?"; the value font the same letters and 0-9
    std::string PauseText(std::string text, bool digits)
    {
      std::string out;

      for (char c : text)
      {
        if (c >= 'a' && c <= 'z')
          c = char(c - 'a' + 'A');

        if (c == '-')
          c = ' ';

        if (c == ':')
          c = 'X'; // 16:9 -> 16X9
        const bool ok = (c >= 'A' && c <= 'Y' && c != 'J' && c != 'Q') || (c >= '1' && c <= '4') || (digits && c >= '0' && c <= '9') || c == ' ' || c == '(' || c == ')' || c == '.' || c == '?';

        if (ok)
          out += c;
      }

      return out;
    }

    std::string PauseLabel(int row)
    {
      if (row == kPreset)
        return "PRESET"; // no Q in the font
      return PauseText(kList[row].label, false);
    }

    std::string PauseValue(const Settings &s, int row)
    {
      if (row == kAaLevel && s.v[kAaType] != 2)
        return "NONE";

      return PauseText(ValueText(s, row), true);
    }

    // guest memory
    inline uint8_t *GuestPtr(uint8_t *base, uint32_t addr)
    {
      return base + addr + rex::memory::detail::PhysicalHostOffset(addr);
    }

    inline uint32_t Load32(uint8_t *base, uint32_t addr)
    {
      uint32_t v;
      std::memcpy(&v, GuestPtr(base, addr), 4);
      return std::byteswap(v);
    }

    inline void Store32(uint8_t *base, uint32_t addr, uint32_t v)
    {
      v = std::byteswap(v);
      std::memcpy(GuestPtr(base, addr), &v, 4);
    }

    constexpr uint32_t kPoolSize = 0x10000;
    constexpr uint32_t kTextIdBase = 0x53540000;
    constexpr uint32_t kTextQuitQuestion = 0x5354FF12; // QUIT dialog body
    // "CONTROLLER" -> "CONTROLS": the Settings page title, the Settings list item (same id as the page's layer hash), the pause menu row
    constexpr uint32_t kTextController[] = {0x2E29AD95, 0xABB4502D, 0x193DCBAD};
    struct Guest
    {
      uint32_t script = 0; // our "video" module
      uint32_t pool = 0, pool_used = 0;
      std::unordered_map<std::string, uint32_t> ids; // string -> text id
      std::unordered_map<uint32_t, uint32_t> texts;  // text id -> guest string (incl. fixed ids)
      bool ok = false;
    };

    Guest g_guest;
    std::once_flag g_guest_once;
    std::mutex g_mutex; // settings, list state, texts

    void SetupGuest(uint8_t *base)
    {
      std::call_once(g_guest_once,
                     [base]
                     {
                       const uint32_t script_size = (uint32_t(sizeof(kGraphicsScript)) + 31) & ~31u;
                       auto *memory = rex::system::kernel_memory();
                       const uint32_t block = memory ? memory->SystemHeapAlloc(script_size + kPoolSize) : 0;

                       if (!block)
                       {
                         REXLOG_ERROR("[sst-menu] could not allocate guest memory for the VIDEO page");
                         return;
                       }

                       std::memcpy(GuestPtr(base, block), kGraphicsScript, sizeof(kGraphicsScript));
                       g_guest.script = block;
                       g_guest.pool = block + script_size;
                       g_guest.ok = true;
                       // Fixed ids the patched pause scene uses
                       for (auto [id, text] : {std::pair<uint32_t, const char *>{pause::kTextBlank, " "}, {pause::kTextVideo, "VIDEO"}, {quit::kTextLabel, "QUIT"}, {quit::kTextDescription, "Close the game and return to the desktop."}, {kTextQuitQuestion, "Are you sure you want to quit to the desktop?"}})
                       {
                         const uint32_t n = uint32_t(std::strlen(text)) + 1;
                         const uint32_t at = g_guest.pool + g_guest.pool_used;
                         std::memcpy(GuestPtr(base, at), text, n);
                         g_guest.pool_used += n;
                         g_guest.texts.emplace(id, at);
                       }

                       REXLOG_INFO("[sst-menu] VIDEO page script ({} bytes) at {:08X}", sizeof(kGraphicsScript), block);
                     });
    }

    // Text id for a string (made once, kept in guest memory). Under g_mutex
    uint32_t TextId(uint8_t *base, const std::string &text)
    {
      if (auto it = g_guest.ids.find(text); it != g_guest.ids.end())
        return it->second;

      const uint32_t n = uint32_t(text.size()) + 1;

      if (!g_guest.ok || g_guest.pool_used + n > kPoolSize)
        return 0;

      const uint32_t at = g_guest.pool + g_guest.pool_used;
      std::memcpy(GuestPtr(base, at), text.c_str(), n);
      g_guest.pool_used += n;
      const uint32_t id = kTextIdBase + uint32_t(g_guest.ids.size());
      g_guest.ids.emplace(text, id);
      g_guest.texts.emplace(id, at);
      return id;
    }

    // QUIT: its dialog is up (texts swapped); the row is pushed down by OPTIONS' sub-items
    bool g_quit_dialog = false;
    bool g_quit_row_pushed = false;
    // CONTROLS > KEY BINDINGS: the next VIDEO page start is the key list (set when Y leaves the CONTROLS page)
    bool g_keys_page_next = false;
    uint32_t g_keys_y_seen = 0; // sst::input::YPresses() handled on the KEY BINDINGS page
    bool g_open_keys = false; // Y was pressed on the CONTROLS page: its exit (to the Settings list) opens KEY BINDINGS instead
    uint32_t g_escape_seen = 0; // sst::input::EscapePresses() at the main menu's last frame
    bool g_quit_scene_failed = false;
    std::unordered_map<uint32_t, uint32_t> g_quit_modules; // original mainmenu module -> patched copy (0: failed)

    // boot intro (kIntro* natives)
    uint32_t g_intro_presses = 0;
    std::chrono::steady_clock::time_point g_intro_timer{};
    uint32_t g_intro_script = 0; // guest copy of kIntroScript

    // settings
    Settings g_current, g_snapshot;
    bool g_loaded_from_file = false;
    std::function<void(bool)> g_set_fullscreen;
    std::function<void()> g_request_quit;
    struct ListState
    {
      int visible = kVisible; // rows on screen
      bool pause = false;     // in-match page: no ASPECT RATIO (cameras are set up per match)
      bool dialog = false;    // this event: show the restart dialog (4:3 switched)
      bool keys = false;      // CONTROLS > KEY BINDINGS instead of VIDEO
      int Count() const { return keys ? sst::input::KeyBindingCount() : pause ? kRows - 1 : kRows; }
      int Row(int i) const { return !keys && pause && i >= kAspect ? i + 1 : i; } // list index -> Row
      int cursor = 0, top = 0;
      int old_visible = 0, new_visible = 0;
      bool redraw = false, leave = false;
      int sound = kNoSound;
    } g_list;

    // Scene animations (4:10 object, key, key) the dispatcher plays after a native returns (outside g_mutex)
    struct Anim
    {
      uint32_t object, key; // key 0: show (4:4), 1: hide (4:5); else 4:10 to that key
    };
    constexpr uint32_t kAnimShow = 0, kAnimHide = 1;
    std::atomic<bool> g_scroll_bar_on{false}; // the pause page's scroll bar is up (else it is kept hidden while the pause menu runs)
    std::vector<Anim> g_anims;

    // What the markers show (-1: not set on this page yet)
    struct Markers
    {
      int up = -1, down = -1;  // Settings page: arrows above / below the four rows
      int shown = -1, pos = -1; // pause page: scroll bar shown, thumb position
    } g_markers;

    static_assert(pause::kScrollCount == kRows - 1 && pause::kScrollVisible == kPauseVisible, "the pause page's scroll bar is built for its list");

    // With g_mutex held, after the list moved
    void UpdateListMarkers()
    {
      const ListState &l = g_list;
      Markers &m = g_markers;

      if (l.pause)
      {
        const int pos = std::clamp(l.top, 0, pause::kScrollCount - pause::kScrollVisible);

        if (m.shown != 1)
          g_anims.push_back({pause::kScrollGroup, kAnimShow}), m.shown = 1, g_scroll_bar_on = true;

        if (pos != m.pos)
          g_anims.push_back({pause::kScrollThumb, pause::kScrollPos + uint32_t(pos)}), m.pos = pos;

        return;
      }

      const int up = l.top > 0, down = l.top + l.visible < l.Count();

      if (up != m.up)
        g_anims.push_back({kCaretUp, up ? kCaretShow : kCaretHide}), m.up = up;

      if (down != m.down)
        g_anims.push_back({kCaretDown, down ? kCaretShow : kCaretHide}), m.down = down;
    }

    void HideListMarkers()
    {
      if (g_list.pause)
        g_anims.push_back({pause::kScrollGroup, kAnimHide}), g_scroll_bar_on = false;
      else
        g_anims.push_back({kCaretUp, kCaretHide}), g_anims.push_back({kCaretDown, kCaretHide});

      g_markers = {};
    }

    std::filesystem::path SettingsPath()
    {
      return rex::filesystem::GetExecutableFolder() / "sst_settings.ini";
    }

    void Apply(const Settings &s)
    {
      auto set = [](const char *name, const std::string &value)
      {
        rex::cvar::SetFlagByName(name, value);
      };

      set("sst_resolution", std::to_string(kResolutions[s.v[kResolution]]));
      set("sst_render_scale", std::to_string(50 + 5 * s.v[kRenderScale]));
      set("sst_shadow_size", std::to_string(kShadowSizes[s.v[kShadows]]));
      set("sst_aa_type", std::to_string(s.v[kAaType]));
      set("sst_aa_level", std::to_string(kAaLevels[s.v[kAaLevel]]));
      set("sst_aspect", std::to_string(s.v[kAspect]));
      set("sst_fps_limit", s.v[kFpsLimit] ? "60" : "30");
      set("sst_vsync", s.v[kVSync] ? "true" : "false");
      set("sst_brightness", std::to_string(5 * s.v[kBrightness]));
      set("sst_bloom", s.v[kBloom] ? "true" : "false");
      set("sst_fps_counter", s.v[kFpsCounter] ? "true" : "false");
      const bool fullscreen = s.v[kFullscreen] != 0;
      set("sst_fullscreen", fullscreen ? "true" : "false");

      if (g_set_fullscreen)
        g_set_fullscreen(fullscreen);
    }

    Settings FromFlags()
    {
      Settings s;
      auto index_of = [](const int *values, int n, int value)
      {
        int best = 0;

        for (int i = 0; i < n; ++i)
          if (std::abs(values[i] - value) < std::abs(values[best] - value))
            best = i;

        return best;
      };

      s.v[kResolution] = index_of(kResolutions, 5, REXCVAR_GET(sst_resolution));
      s.v[kRenderScale] = std::clamp((REXCVAR_GET(sst_render_scale) - 50) / 5, 0, 10);
      s.v[kShadows] = index_of(kShadowSizes, 6, REXCVAR_GET(sst_shadow_size));
      s.v[kAaType] = std::clamp(REXCVAR_GET(sst_aa_type), 0, 2);
      s.v[kAaLevel] = index_of(kAaLevels, 3, REXCVAR_GET(sst_aa_level));
      s.v[kAspect] = std::clamp(REXCVAR_GET(sst_aspect), 0, 4);
      s.v[kFpsLimit] = REXCVAR_GET(sst_fps_limit) <= 30 ? 0 : 1;
      s.v[kVSync] = REXCVAR_GET(sst_vsync) ? 1 : 0;
      s.v[kBrightness] = std::clamp(REXCVAR_GET(sst_brightness) / 5, 0, 20);
      s.v[kFullscreen] = REXCVAR_GET(sst_fullscreen) ? 1 : 0;
      s.v[kBloom] = REXCVAR_GET(sst_bloom) ? 1 : 0;
      s.v[kFpsCounter] = REXCVAR_GET(sst_fps_counter) ? 1 : 0;
      UpdatePreset(s);
      return s;
    }

    bool Load(Settings &s)
    {
      std::ifstream f(SettingsPath());

      if (!f)
        return false;

      std::string line;
      bool any = false;

      while (std::getline(f, line))
      {
        const size_t eq = line.find('=');

        if (eq == std::string::npos || line.empty() || line[0] == '#' || line[0] == ';')
          continue;

        auto trim = [](std::string x)
        {
          const size_t a = x.find_first_not_of(" \t\r");
          const size_t b = x.find_last_not_of(" \t\r");
          return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
        };

        const std::string key = trim(line.substr(0, eq));
        const int value = std::atoi(trim(line.substr(eq + 1)).c_str());

        for (int r = 0; r < kRows; ++r)
        {
          if (key == kList[r].key)
          {
            s.v[r] = std::clamp(value, 0, kList[r].count - 1);
            any = true;
          }
        }
      }

      UpdatePreset(s);
      return any;
    }

    void Save(const Settings &s)
    {
      std::ofstream f(SettingsPath(), std::ios::trunc);

      if (!f)
      {
        REXLOG_ERROR("[sst-menu] could not write {}", SettingsPath().string());
        return;
      }

      f << "# sst_rematch video settings (Options > Settings > VIDEO). Values are list\n"
           "# positions: preset 0 low 1 medium 2 high 3 custom; shadows 0 off, 1-5 = 128-2048;\n"
           "# bloom 0 off 1 on; resolution 0-4 = 480p 720p 1080p 1440p 2160p; aspect_ratio 0-4 = 4:3 16:9 16:10\n"
           "# 21:9 32:9; render_scale 0-10 = 50-100%; aa_type 0 off 1 FXAA 2 MSAA; aa_level\n"
           "# 0-2 = 2x 4x 8x; fps_limit 0 = 30, 1 = 60; brightness 0-20 = 0-100.\n";

      for (int r = 0; r < kRows; ++r)
        f << kList[r].key << " = " << s.v[r] << "\n";
    }

    // list navigation
    void Change(Settings &s, int row, int dir)
    {
      const RowInfo &info = kList[row];

      if (row == kAaLevel && s.v[kAaType] != 2)
        return; // only MSAA has levels
      int count = info.count;

      if (row == kPreset)
        count = 3; // CUSTOM is shown, not picked
      int v = s.v[row];

      if (row == kPreset && v == kPresetCustom)
        v = dir > 0 ? -1 : 3;

      v += dir;

      if (info.slider)
      {
        v = std::clamp(v, 0, count - 1);
      }
      else
      {
        v = (v + count) % count;
      }

      if (row == kPreset)
      {
        ApplyPreset(s, v);
      }
      else
      {
        s.v[row] = v;
        UpdatePreset(s);
      }
    }

    // A new aspect ratio needs a restart: 4:3 is the game's own mode, and the wide layouts are made as scenes load (the main menu's at boot)
    bool RestartNeeded()
    {
      return g_current.v[kAspect] != g_boot_aspect;
    }

    // Restart dialog: the game's message box with a one-button (OK) message whose texts are swapped while it is up (see sub_82594AD0 below)
    std::atomic<bool> g_restart_dialog{false};
    constexpr uint32_t kDialogBody = 0x84D43E5B;  // "Unable to find any Superstars Channels"
    constexpr uint32_t kDialogTitle = 0xEDDAB5CE; // "NO CHANNELS FOUND"
    constexpr const char *kDialogBodyText = "The game will now restart to apply the new aspect ratio.";
    constexpr const char *kDialogTitleText = "RESTART REQUIRED";

    // The dialog was confirmed: start the new copy, then close this one (forced after a few seconds if the normal shutdown stalls)
    void Restart()
    {
      REXLOG_INFO("[sst-menu] restarting for the aspect ratio change");

      if (RestartGame())
      {
        if (g_request_quit)
          g_request_quit();

        ExitSoon(3000);
      }
      else
      {
        REXLOG_ERROR("[sst-menu] restart failed: the aspect ratio applies at the next start");
      }
    }

    void HandleEvent(int event)
    {
      ListState &l = g_list;
      l.old_visible = l.cursor - l.top;
      l.redraw = false;
      l.leave = false;
      l.sound = kNoSound;
      l.dialog = false;

      // KEY BINDINGS: select waits for a key (or resets them all), back leaves; nothing while a key is being taken
      if (l.keys)
      {
        if (sst::input::KeyCaptureActive())
        {
          l.new_visible = l.old_visible;
          return;
        }

        if ((event == kSelect || event == kLeft || event == kRight) && sst::input::KeyBindingToggle(l.cursor))
        {
          l.redraw = true;
          l.sound = kMove;
          l.new_visible = l.old_visible;
          return;
        }

        if (event == kSelect)
        {
          sst::input::BeginKeyCapture(l.cursor);

          l.redraw = true;
          l.sound = kSelectSound;
          l.new_visible = l.old_visible;
          return;
        }

        if (event == kBack)
        {
          REXLOG_INFO("[sst-menu] KEY BINDINGS closed");
          l.leave = true;
          l.sound = kBackSound;
          l.new_visible = l.old_visible;
          return;
        }

        if (event == kLeft || event == kRight)
        {
          l.new_visible = l.old_visible;
          return;
        }
      }

      switch (event)
      {
      case kLeft:
      case kRight:
      {
        const Settings before = g_current;
        Change(g_current, l.Row(l.cursor), event == kRight ? 1 : -1);

        if (g_current.v != before.v)
        {
          Apply(g_current);
          l.redraw = true;
          l.sound = kMove;
          REXLOG_INFO("[sst-menu] {}", Describe(g_current));
        }
        break;
      }

      case kUp:
      case kDown:
      {
        const int n = l.Count();
        l.cursor = (l.cursor + (event == kDown ? 1 : n - 1)) % n;
        const int top = std::clamp(l.top, std::max(0, l.cursor - l.visible + 1), std::min(l.cursor, n - l.visible));

        if (top != l.top)
        {
          l.top = top;
          l.redraw = true;
        }

        l.sound = kMove;
        break;
      }

      case kSelect:
        Save(g_current);
        REXLOG_INFO("[sst-menu] VIDEO saved: {}", Describe(g_current));
        l.sound = kSelectSound;

        if (!l.pause && RestartNeeded())
        {
          l.dialog = true;
          g_restart_dialog = true;
          break;
        }

        l.leave = true;
        break;

      case kBack:
        if (g_current.v != g_snapshot.v)
        {
          g_current = g_snapshot;
          Apply(g_current);
        }

        REXLOG_INFO("[sst-menu] VIDEO cancelled: {}", Describe(g_current));
        l.leave = true;
        l.sound = kBackSound;
        break;
      }

      l.new_visible = l.cursor - l.top;
    }

    // natives (group 0x53)
    // sub_821DFBC0's arguments: r3 = result value {u32 value, u32 type}, r4 = VM, r5 = native id, r8 = argument values (u32 each), r9 = argument types (u8 each), r10 = count. Types: 0 nil, 1 float, 2 int, 3 hash
    void SetResult(uint8_t *base, uint32_t out, uint32_t value, uint32_t type)
    {
      Store32(base, out, value);
      Store32(base, out + 4, type);
    }

    int ArgInt(uint8_t *base, uint32_t values, uint32_t types, uint32_t count, uint32_t i)
    {
      if (i >= count)
        return 0;

      const uint32_t raw = Load32(base, values + 4 * i);

      if (*GuestPtr(base, types + i) == 1)
      { // float
        float f;
        std::memcpy(&f, &raw, 4);
        return int(f);
      }

      return int32_t(raw);
    }

    void ApplyAspect(uint8_t *base);

    void HandleNative(PPCContext &ctx, uint8_t *base, uint32_t id)
    {
      const uint32_t out = ctx.r3.u32;
      const uint32_t values = ctx.r8.u32, types = ctx.r9.u32, count = ctx.r10.u32;
      std::lock_guard lock(g_mutex);
      SetupGuest(base);
      struct AspectOnExit
      {
        uint8_t *base;
        ~AspectOnExit() { ApplyAspect(base); }
      } aspect_on_exit{base};

      switch (id & 0xFF)
      {
      case kBegin:
      case kPauseBegin:
        g_snapshot = g_current;
        g_list = {};
        g_list.visible = (id & 0xFF) == kPauseBegin ? kPauseVisible : kVisible;
        g_list.pause = (id & 0xFF) == kPauseBegin;
        g_list.keys = (id & 0xFF) == kBegin && g_keys_page_next;
        g_keys_page_next = false;
        g_keys_y_seen = sst::input::YPresses();

        if (g_list.keys)
          REXLOG_INFO("[sst-menu] KEY BINDINGS page opened");
        else
          REXLOG_INFO("[sst-menu] VIDEO page opened{}: {}", (id & 0xFF) == kPauseBegin ? " (pause menu)" : "", Describe(g_current));

        SetResult(base, out, 0, 2);
        break;

      case kPageTitle:
        SetResult(base, out, g_list.keys ? TextId(base, "KEY BINDINGS") : kTextVideoGame, 3);
        break;

      case kPageFrame:
      {
        bool redraw = false;

        if (g_list.keys)
        {
          // A key was taken (or the wait cancelled); Y (not while a key is being taken) resets the bindings
          const int captured = sst::input::PollKeyCapture();
          const uint32_t y = sst::input::YPresses();
          redraw = captured != 0;

          if (captured || sst::input::KeyCaptureActive())
          {
            g_keys_y_seen = y;
          }
          else if (y != g_keys_y_seen)
          {
            g_keys_y_seen = y;
            sst::input::ResetKeyBindings();
            redraw = true;
          }
        }

        SetResult(base, out, redraw ? 1 : 0, 2);
        break;
      }

      case kPageReturnCursor:
        SetResult(base, out, g_list.keys ? 3 : 5, 2); // CONTROLS / VIDEO item of the Settings list, + 1
        break;

      case kPageReturnScript:
        SetResult(base, out, g_list.keys ? kControllerScript : kSettingsScript, 3);
        g_list.keys = false; // (the page is closing)
        break;

      case kPauseEvent:
      {
        // The pause menu's input events (3:18): up, down, left, right, select, back
        const uint32_t hash = Load32(base, values);
        static constexpr std::pair<uint32_t, int> kEvents[] = {{0xF5BF6BF3, kLeft}, {0x5856F909, kRight}, {0xB0A4EB7F, kUp}, {0xA3AD84E1, kDown}, {0x2FAB294A, kSelect}, {0xA979AAA1, kBack}};
        int event = -1;

        for (auto [h, e] : kEvents)
          if (count >= 1 && hash == h)
            event = e;

        if (event >= 0)
        {
          HandleEvent(event);
        }
        else
        {
          g_list.old_visible = g_list.new_visible = g_list.cursor - g_list.top;
          g_list.redraw = g_list.leave = false;
          g_list.sound = kNoSound;
        }

        SetResult(base, out, 0, 2);
        break;
      }

      case kPauseLabel:
      case kPauseValue:
      {
        const int row = g_list.Row(g_list.top + std::clamp(ArgInt(base, values, types, count, 0), 0, kPauseVisible - 1));
        const std::string text = (id & 0xFF) == kPauseLabel ? PauseLabel(row) : PauseValue(g_current, row);
        SetResult(base, out, TextId(base, text), 3);
        break;
      }

      case kEvent:
        HandleEvent(ArgInt(base, values, types, count, 0));
        SetResult(base, out, 0, 2);
        break;

      case kResult:
      {
        const int k = ArgInt(base, values, types, count, 0);
        const ListState &l = g_list;
        // (Pause page: lines 0-6 are drawn on the rows in their screen order.)
        auto at = [&](int line)
        {
          return l.pause && line >= 0 && line < pause::kRows ? pause::kVisualOrder[line] : line;
        };

        const int v = k == 0 ? at(l.old_visible) : k == 1 ? at(l.new_visible)
                                               : k == 2   ? int(l.redraw)
                                               : k == 3   ? int(l.leave)
                                               : k == 4   ? l.sound
                                                          : int(l.dialog);
        SetResult(base, out, uint32_t(v), 2);
        break;
      }

      case kQuitDialogBegin:
      case kQuitDialogEnd:
        g_quit_dialog = (id & 0xFF) == kQuitDialogBegin;
        sst::input::SetDialogUp(g_quit_dialog);
        SetResult(base, out, 0, 2);
        break;

      case kQuit:
        REXLOG_INFO("[sst-menu] QUIT confirmed: closing the game");
        SetResult(base, out, 0, 2);

        if (g_request_quit)
          g_request_quit();

        // (The normal shutdown, asked for from the script thread, does not finish on its own: end the process once the log is out)
        ExitSoon(1000);
        break;

      case kQuitEscape:
      {
        // Every main menu frame (argument: menu level, 1 = category list): Esc on the category list asks to QUIT
        const bool category_list = ArgInt(base, values, types, count, 0) == 1;
        sst::input::MainMenuFrame(category_list && !g_quit_dialog);
        const uint32_t presses = sst::input::EscapePresses();
        const bool ask = category_list && !g_quit_dialog && presses != g_escape_seen;
        g_escape_seen = presses;

        if (ask)
          REXLOG_INFO("[sst-menu] Esc on the main menu: QUIT?");

        SetResult(base, out, ask ? 1 : 0, 2);
        break;
      }

      case kIntroBegin:
        g_intro_presses = sst::input::ConfirmPresses();
        g_intro_timer = std::chrono::steady_clock::now();
        SetResult(base, out, 0, 2);
        break;

      case kIntroTimer:
        g_intro_timer = std::chrono::steady_clock::now();
        SetResult(base, out, 0, 2);
        break;

      case kIntroSkip:
      {
        const int ms = ArgInt(base, values, types, count, 0);
        const uint32_t presses = sst::input::ConfirmPresses();
        bool skip = false;

        if (presses != g_intro_presses)
        {
          g_intro_presses = presses;
          skip = true;
          REXLOG_INFO("[sst-menu] intro: skipped");
        }
        else if (ms > 0)
        {
          skip = std::chrono::steady_clock::now() - g_intro_timer >= std::chrono::milliseconds(ms);
        }

        SetResult(base, out, skip ? 1 : 0, 2);
        break;
      }

      case kPauseStep:
      {
        // Pause menu up / down: the next row in screen order (pause::kVisualOrder) among rows first .. first + count - 1, wrapping
        const int row = ArgInt(base, values, types, count, 0);
        const int dir = ArgInt(base, values, types, count, 1) < 0 ? -1 : 1;
        const int first = ArgInt(base, values, types, count, 2);
        const int n = ArgInt(base, values, types, count, 3);
        std::vector<int> order;

        for (int r : pause::kVisualOrder)
          if (r >= first && r < first + n)
            order.push_back(r);

        for (int r = first; r < first + n; ++r) // (any rows the order doesn't list)
          if (std::find(order.begin(), order.end(), r) == order.end())
            order.push_back(r);

        int next = row;

        if (!order.empty())
        {
          const auto it = std::find(order.begin(), order.end(), row);
          const int i = it == order.end() ? (dir > 0 ? -1 : 0) : int(it - order.begin());
          const int sz = int(order.size());
          next = order[((i + dir) % sz + sz) % sz];
        }

        SetResult(base, out, uint32_t(next), 2);
        break;
      }

      case kRestart:
        g_restart_dialog = false;
        Restart();
        SetResult(base, out, 0, 2);
        break;

      case kLabel:
      case kValue:
      {
        const int row = g_list.top + std::clamp(ArgInt(base, values, types, count, 0), 0, 3);
        std::string text = g_list.keys ? ((id & 0xFF) == kLabel ? std::string(sst::input::KeyBindingLabel(row)) : sst::input::KeyBindingValue(row))
                                       : (id & 0xFF) == kLabel ? std::string(kList[row].label)
                                                               : ValueText(g_current, row);
        SetResult(base, out, TextId(base, text), 3);
        break;
      }

      default:
        REXLOG_WARN("[sst-menu] unknown native {:04X}", id);
        SetResult(base, out, 0, 0);
        break;
      }

      // "More rows" markers: the Settings page's up / down arrows, the pause page's scroll bar
      switch (id & 0xFF)
      {
      case kBegin:
      case kPauseBegin:
        g_markers = {};
        break;

      case kLabel:
      case kPauseLabel:
        UpdateListMarkers();
        break;

      case kEvent:
      case kPauseEvent:
        if (g_list.leave)
          HideListMarkers();

        break;
      }

      ctx.r3.u32 = out;
    }

    // Points the front-end's script directory entry for `hash` at `module`. The directory (front-end object + 0x488) is a binary tree of nodes {+4 left, +8 right, +0x10 name hash, +0x18 module}, as walked by sub_821CCA28
    uint32_t FindNode(uint8_t *base, uint32_t fe, uint32_t hash)
    {
      uint32_t node = Load32(base, fe + 0x488);

      for (int guard = 0; node && guard < 64; ++guard)
      {
        const uint32_t key = Load32(base, node + 0x10);

        if (hash == key)
          return node;

        node = Load32(base, node + (hash > key ? 8 : 4));
      }

      return 0;
    }

    bool Redirect(uint8_t *base, uint32_t fe, uint32_t hash, uint32_t module)
    {
      const uint32_t node = FindNode(base, fe, hash);

      if (!node)
        return false;

      if (Load32(base, node + 0x18) != module)
      {
        Store32(base, node + 0x18, module);
        REXLOG_INFO("[sst-menu] script {:08X} -> the port's version", hash);
      }

      return true;
    }

    // aspect ratio
    // The game's cameras build their projections with a vertical field of view and an aspect of 16/9 (each camera keeps its own copy, set up from the constant at 0x82108440). The port hooks the perspective builders and replaces a 16:9 aspect with the chosen one: a wider value widens the view (Hor+). The renderer squeezes the UI back into a 16:9 box and the presenter shows the frame at the chosen aspect
    constexpr float kAspectValues[5] = {4.0f / 3.0f, 16.0f / 9.0f, 1.6f, 21.0f / 9.0f, 32.0f / 9.0f};
    std::atomic<float> g_camera_aspect{16.0f / 9.0f};

    // The cameras also keep the aspect they were set up with (from the 16/9 constant at 0x82108440) and use it for more than the projection - e.g. the area the shadow maps cover. So the constant is changed too (in the xex's read-only data: its page is opened briefly)
    constexpr uint32_t kCameraAspectConstant = 0x82108440;

    void ApplyAspect(uint8_t *base)
    {
      // The aspect ratio the game started with (a new one waits for the restart). 4:3 runs as the game's own 4:3 mode (cameras already 4:3)
      const int v = g_boot_aspect;
      const float want = v == 0 ? 16.0f / 9.0f : kAspectValues[v];

      if (g_camera_aspect.exchange(want) != want)
        REXLOG_INFO("[sst-menu] camera aspect {:.4f} ({}{})", want, kAspects[v], v == 0 ? ", game's 4:3 mode" : "");

      if (!base)
        return;

      uint32_t bits;
      std::memcpy(&bits, &want, 4);

      if (Load32(base, kCameraAspectConstant) == bits)
        return;

      auto *memory = rex::system::kernel_memory();
      auto *heap = memory ? memory->LookupHeap(kCameraAspectConstant) : nullptr;
      const uint32_t page = kCameraAspectConstant & ~0xFFFu;
      uint32_t old_protect = 0;

      if (!heap || !heap->Protect(page, 0x1000, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, &old_protect))
      {
        static bool warned = false;

        if (!warned)
          REXLOG_WARN("[sst-menu] camera aspect constant: could not unprotect {:08X}", page);

        warned = true;
        return;
      }

      Store32(base, kCameraAspectConstant, bits);
      heap->Protect(page, 0x1000, old_protect);
    }

    // f2 of a perspective builder: 16:9 becomes the chosen aspect (other values, e.g. split views, are left alone). Each (caller, aspect) a perspective builder sees is logged once (cameras that do not follow the wide aspect are found this way)
    void LogBuilder(uint32_t builder, uint32_t caller, double aspect, double replaced)
    {
      static std::mutex mutex;
      static std::set<uint64_t> seen;
      const uint64_t key = (uint64_t(caller) << 32) ^ uint64_t(builder) ^ uint64_t(std::llround(aspect * 10000.0));
      std::lock_guard lock(mutex);

      if (seen.size() > 512 || !seen.insert(key).second)
        return;

      REXLOG_INFO("[sst-menu] camera: builder {:08X} from {:08X} aspect {:.4f}{}", builder, caller, aspect, replaced != aspect ? fmt::format(" -> {:.4f}", replaced) : std::string());
    }

    void FixCameraAspect(PPCContext &ctx, uint32_t builder)
    {
      const double a = ctx.f2.f64;
      struct Log
      {
        PPCContext &c;
        uint32_t b;
        double a;
        ~Log() { LogBuilder(b, uint32_t(c.lr), a, c.f2.f64); }
      } log{ctx, builder, a};

      if (std::fabs(a - 16.0 / 9.0) < 0.002)
        ctx.f2.f64 = double(g_camera_aspect.load());
    }

    // in-match pause menu (pause_patch.h)
    std::mutex g_pause_mutex;
    std::unordered_map<uint32_t, uint32_t> g_pause_modules; // game's module -> patched copy
    // PauseMenu file buffer -> {its size, where the loader keeps the file's size}
    struct PauseBuffer
    {
      uint32_t room, size_at;
      std::string name; // lower-case file name
    };

    std::unordered_map<uint32_t, PauseBuffer> g_pause_rooms;
    // A PauseMenu.DAT / .REL patch failed: the scene has no VIDEO row, so the script must not use it either
    bool g_pause_scene_failed = false;

    // Points "pausemenu_match" at a patched copy (made once per original module)
    void RedirectPauseScript(uint8_t *base, uint32_t fe)
    {
      const uint32_t node = FindNode(base, fe, pause::kScriptHash);

      if (!node)
        return;

      const uint32_t module = Load32(base, node + 0x18);
      std::lock_guard lock(g_pause_mutex);

      for (auto &[orig, patched] : g_pause_modules)
      {
        if (module == patched)
        {
          // The script needs the patched scene (row 7 and the spare texts)
          if (g_pause_scene_failed)
            Store32(base, node + 0x18, orig);

          return;
        }
      }

      if (g_pause_scene_failed)
        return;

      uint32_t patched = 0;

      if (auto it = g_pause_modules.find(module); it != g_pause_modules.end())
      {
        patched = it->second;
      }
      else
      {
        g_pause_modules[module] = 0; // tried (no retry on failure)
        // Header first; the module ends with its debug strings (a few hundred bytes)
        const uint32_t debug = Load32(base, module + 28);

        if (debug < 0x20 || debug > 0x100000)
          return;

        std::vector<uint8_t> bytes(debug + 0x400);
        std::memcpy(bytes.data(), GuestPtr(base, module), bytes.size());
        std::string error;
        auto out = pause::PatchScript(bytes.data(), bytes.size(), error);

        if (!out)
        {
          REXLOG_WARN("[sst-menu] pause menu VIDEO row disabled (script: {})", error);
          return;
        }

        auto *memory = rex::system::kernel_memory();
        patched = memory ? memory->SystemHeapAlloc(uint32_t(out->size())) : 0;

        if (!patched)
          return;

        std::memcpy(GuestPtr(base, patched), out->data(), out->size());
        g_pause_modules[module] = patched;
        REXLOG_INFO("[sst-menu] pausemenu_match patched: VIDEO row ({} -> {} bytes at {:08X})", pause::ModuleSize(bytes.data(), bytes.size()), out->size(), patched);
      }

      if (patched)
        Store32(base, node + 0x18, patched);
    }

    // Main menu: the QUIT row (main_menu_quit.h). The patched script needs the patched scene
    void RedirectMainMenuScript(uint8_t *base, uint32_t fe)
    {
      const uint32_t node = FindNode(base, fe, quit::kScriptHash);

      if (!node)
        return;

      const uint32_t module = Load32(base, node + 0x18);
      std::lock_guard lock(g_pause_mutex);

      for (auto &[orig, patched] : g_quit_modules)
        if (module == patched && patched)
        {
          if (g_quit_scene_failed || !REXCVAR_GET(sst_quit_item))
            Store32(base, node + 0x18, orig);

          return;
        }

      if (g_quit_scene_failed || !REXCVAR_GET(sst_quit_item))
        return;

      uint32_t patched = 0;

      if (auto it = g_quit_modules.find(module); it != g_quit_modules.end())
      {
        patched = it->second;
      }
      else
      {
        g_quit_modules[module] = 0; // tried (no retry on failure)
        const uint32_t debug = Load32(base, module + 28);

        if (debug < 0x20 || debug > 0x100000)
          return;

        std::vector<uint8_t> bytes(debug + 0x400);
        std::memcpy(bytes.data(), GuestPtr(base, module), bytes.size());
        std::string error;
        auto out = quit::PatchScript(bytes.data(), bytes.size(), error);

        if (!out)
        {
          REXLOG_WARN("[sst-menu] QUIT disabled (script: {})", error);
          return;
        }

        auto *memory = rex::system::kernel_memory();
        patched = memory ? memory->SystemHeapAlloc(uint32_t(out->size())) : 0;

        if (!patched)
          return;

        std::memcpy(GuestPtr(base, patched), out->data(), out->size());
        g_quit_modules[module] = patched;
        REXLOG_INFO("[sst-menu] mainmenu patched: QUIT ({} bytes at {:08X})", out->size(), patched);
      }

      if (patched)
        Store32(base, node + 0x18, patched);
    }

    // In-match HUD scene buffers (DAT) -> file name, for the ultrawide anchoring
    std::unordered_map<uint32_t, std::string> g_hud_buffers;

    std::string GuestFileName(uint8_t *base, uint32_t path)
    {
      std::string s;

      for (uint32_t i = 0; i < 260; ++i)
      {
        const char c = char(*GuestPtr(base, path + i));

        if (!c)
          break;

        s += char(std::tolower(uint8_t(c)));
      }

      const size_t slash = s.find_last_of("\\/:");
      return slash == std::string::npos ? s : s.substr(slash + 1);
    }

    // Which pause scene file a path names: 1 PauseMenu.DAT, 2 PauseMenu.REL
    int PauseSceneFile(uint8_t *base, uint32_t path)
    {
      std::string s;

      for (uint32_t i = 0; i < 260; ++i)
      {
        const char c = char(*GuestPtr(base, path + i));

        if (!c)
          break;

        s += char(std::tolower(uint8_t(c)));
      }

      const size_t slash = s.find_last_of("\\/:");
      const std::string name = slash == std::string::npos ? s : s.substr(slash + 1);
      const int kind = name == "pausemenu.dat" ? 1 : name == "pausemenu.rel" ? 2
                                                                             : 0;
      static std::atomic<int> logged{0};

      if (s.ends_with(".dat") && logged.fetch_add(1) < 40)
        REXLOG_INFO("[sst-menu] scene file {}{}", s, kind ? " (pause menu)" : "");

      return kind;
    }
  }

  void Initialize(std::function<void(bool)> set_fullscreen, std::function<void()> request_quit)
  {
    std::lock_guard lock(g_mutex);
    g_set_fullscreen = std::move(set_fullscreen);
    g_request_quit = std::move(request_quit);
    Settings s = FromFlags();
    g_loaded_from_file = Load(s);
    g_current = s;
    Apply(g_current);
    REXLOG_INFO("[sst-menu] video settings{}: {}", g_loaded_from_file ? " (sst_settings.ini)" : "", Describe(g_current));
    // ASPECT RATIO 4:3: a 4:3 video mode (1280x1024, the game's HD 4:3 mode) makes the game pick its 4:3 layouts and cameras at boot
    g_boot_aspect = std::clamp(g_current.v[kAspect], 0, 4);
    g_boot_four_three = g_boot_aspect == 0;
    ApplyAspect(nullptr); // scenes loaded at boot already see the aspect
    if (g_boot_four_three)
    {
      rex::cvar::SetFlagByName("video_mode_width", "1280");
      rex::cvar::SetFlagByName("video_mode_height", "1024");
      REXLOG_INFO("[sst-menu] aspect ratio 4:3: the game's own 4:3 mode (video mode 1280x1024)");
    }
  }

  bool NativeFourThree()
  {
    return g_boot_four_three;
  }

  void OnGpuDetected(uint64_t video_memory_mb, const std::string &name)
  {
    std::lock_guard lock(g_mutex);

    if (g_loaded_from_file)
      return;
    // First boot (no sst_settings.ini): pick a preset from the GPU's memory
    const int preset = video_memory_mb < 3000 ? 0 : video_memory_mb < 7000 ? 1
                                                                           : 2;
    ApplyPreset(g_current, preset);
    Apply(g_current);
    Save(g_current);
    g_loaded_from_file = true;
    static const char *kNames[] = {"LOW", "MEDIUM", "HIGH"};
    REXLOG_INFO("[sst-menu] first boot: {} with {} MB video memory -> {} preset ({})", name, video_memory_mb, kNames[preset], Describe(g_current));
  }
}

using namespace sst::menu;

extern "C" void __imp__sub_821CCA28(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_821DFBC0(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_82594AD0(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_82594618(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_825946B0(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_82595158(PPCContext &__restrict, uint8_t *);
extern "C" void sub_82128A90(PPCContext &__restrict, uint8_t *); // start reading (fs, path, buffer, 0)
extern "C" void sub_82594820(PPCContext &__restrict, uint8_t *); // file size (path)
extern "C" void sub_82282BB8(PPCContext &__restrict, uint8_t *); // scene native 4:4 show
extern "C" void sub_82282C28(PPCContext &__restrict, uint8_t *); // scene native 4:5 hide
extern "C" void sub_82282AA0(PPCContext &__restrict, uint8_t *); // scene native 4:10 animate (object, from label, to label, flags)
extern "C" void sub_82283318(PPCContext &__restrict, uint8_t *); // scene native 4:18 alpha (object, 0-255)
extern "C" void sub_82582110(PPCContext &__restrict, uint8_t *); // scenes 0x82DB4370, object hash -> object (+0xB: alpha)
extern "C" void sub_82122480(PPCContext &__restrict, uint8_t *); // allocate (size, align)
extern "C" void sub_821289D8(PPCContext &__restrict, uint8_t *); // read file (fs, path, buffer)

// Front-end: start script r4 (name hash) with argument r5
extern "C" REX_FUNC(sub_821CCA28)
{
  {
    std::lock_guard lock(g_mutex);
    ApplyAspect(base);
  }

  // The KEY BINDINGS page is over once the page it leaves for starts
  if (ctx.r4.u32 == kControllerScript || ctx.r4.u32 == kSettingsScript)
  {
    std::lock_guard lock(g_mutex);
    g_list.keys = false;
  }

  // CONTROLS page left with Y: KEY BINDINGS (the VIDEO page's script with the key list) instead of the Settings list
  if (ctx.r4.u32 == kSettingsScript && g_open_keys)
  {
    g_open_keys = false;
    g_keys_page_next = true;
    ctx.r4.u64 = kVideoScript;
  }

  // Title screen -> main menu: whoever pressed Start is the game's 1P
  {
    static bool title = false;

    if (ctx.r4.u32 == 0x1187AE2B) // startscreen
    {
      title = true;
      sst::input::SetTitleScreen(true);
    }
    else if (ctx.r4.u32 == quit::kScriptHash && title) // mainmenu
    {
      title = false;
      sst::input::SetTitleScreen(false);
      sst::input::MarkGameStarted();
    }
  }

  // Esc on the keyboard: pause once a match's HUD starts, back once a menu starts
  {
    // lookup2: mainhud, ballgamehuds, tvhud
    constexpr uint32_t kMatchScripts[] = {0x2AF836AB, 0x8A7D60F5, 0x4D80CE71};
    // lookup2: mainmenu, startscreen, charselect, posselect, results_match, results_mission, results_scoreattack, results_scoreattack_retry, challengemap, challengetourntree, tournament_ladder_offline, tournament_ladder_intro, tournament_lose_retry, tournament_special_match, tournresultsrankandscore, trophy_sequence, trophy_sequence_loser, trophy_sequence_special, onlinemenu, options_settings, awards, credits, nameentry, waitbeforespecialmatch, tournament_hiscores, challengeinit
    constexpr uint32_t kMenuScripts[] = {0x9E754704, 0x1187AE2B, 0x4BC618A9, 0x2EC0986E, 0x1CC9C24A, 0x0BE84691, 0xB504251A, 0xF40FB5E4, 0x394ED102, 0x93F12D2E, 0x7BC4731A, 0x63C234A3, 0x5B3870AF, 0x5C03314A, 0x1F4FCE03, 0x96420D5C, 0x6552C391, 0x962D4940, 0xDD7C9C15, 0x10701090, 0xE33D3780, 0xCF7FABD5, 0x2EDF05C6, 0x104664B1, 0xC85542DA, 0xAA7B77F8};
    const uint32_t h = ctx.r4.u32;

    if (std::find(std::begin(kMatchScripts), std::end(kMatchScripts), h) != std::end(kMatchScripts))
      sst::input::SetInMatch(true);
    else if (std::find(std::begin(kMenuScripts), std::end(kMenuScripts), h) != std::end(kMenuScripts))
      sst::input::SetInMatch(false);
  }

  if (ctx.r4.u32 == pause::kScriptHash && REXCVAR_GET(sst_graphics_menu))
  {
    SetupGuest(base);
    RedirectPauseScript(base, ctx.r3.u32);
  }

  if (ctx.r4.u32 == quit::kScriptHash && REXCVAR_GET(sst_graphics_menu))
    RedirectMainMenuScript(base, ctx.r3.u32);

  // Boot intro: the port's skippable version of "splash"
  if (ctx.r4.u32 == kSplashScript && REXCVAR_GET(sst_skip_intro))
  {
    SetupGuest(base);
    static std::once_flag once;
    std::call_once(once, [base]
                   {
      auto* memory = rex::system::kernel_memory();
      const uint32_t block = memory ? memory->SystemHeapAlloc((uint32_t(sizeof(kIntroScript)) + 31) & ~31u) : 0;

      if (block)
      {
        std::memcpy(GuestPtr(base, block), kIntroScript, sizeof(kIntroScript));
        g_intro_script = block;
      } });

    if (g_intro_script && !Redirect(base, ctx.r3.u32, kSplashScript, g_intro_script))
      REXLOG_WARN("[sst-menu] \"splash\" not found in the script directory");
  }

  if (ctx.r4.u32 == kVideoScript && REXCVAR_GET(sst_graphics_menu))
  {
    SetupGuest(base);

    if (g_guest.ok && !Redirect(base, ctx.r3.u32, kVideoScript, g_guest.script))
      REXLOG_WARN("[sst-menu] \"video\" not found in the script directory");
  }

  __imp__sub_821CCA28(ctx, base);
}

// Settings > CONTROLLER (script "controller", scene abb4502d) holds two controller diagrams: the Xbox 360 pad (group 50034B87) and the DualShock 3 (3A12D8E8). The Xbox script shows the first and hides the second when the page opens (4:5 on 3A12D8E8), then animates the shown one (4:10, layout types A-D). With a PlayStation pad in use the two are swapped in every scene native the page calls: decided when the page opens, kept until it opens again
namespace
{
  constexpr uint32_t kXboxPadGroup = 0x50034B87, kPs3PadGroup = 0x3A12D8E8;
  constexpr uint32_t kControllerLayer = 0xABB4502D; // lookup2("SETTINGS_CONTROLLER"): the page's layer, closed (4:3) by the script's Exit
  bool g_controller_ps = false;   // the DualShock 3 diagram is shown (the two groups are swapped in the page's natives)
  bool g_controller_open = false; // the page is up: the diagram follows the device in use
  bool g_keys_hint = false;       // the page offers KEY BINDINGS (Y): the keyboard was in use when it opened
  uint32_t g_y_seen = 0;          // sst::input::YPresses() handled
  uint32_t g_navbar = 0;          // guest array for the page's navbar: Select, Confirm, Back + Key Bindings
  constexpr uint32_t kNavbarButtons = 0x69F54F57, kInputEvents = 0x7074624C;
  constexpr uint32_t kEventSelect = 0x2FAB294A;
  uint32_t g_scratch = 0;         // guest buffer for the direct native calls: +0 result (8), +8 values (8 x 4), +40 types (8)
  // The page's last animation (4:10: object, from, to, flags) on the shown diagram, played on the other one when it takes over
  struct PadAnim
  {
    uint32_t count = 0;
    uint32_t values[8]{};
    uint8_t types[8]{};
  } g_pad_anim;

  bool WantPs3Diagram()
  {
    return REXCVAR_GET(sst_controller_diagram) && sst::input::CurrentPromptStyle() == sst::input::PromptStyle::kPlayStation;
  }

  // Calls a scene native directly, as the dispatcher would (r3 result, r4 kept, r5 values, r6 types, r7 count) - not through the dispatcher, which pushes the result on the calling script's stack
  void SceneCall(PPCContext &ctx, uint8_t *base, void (*native)(PPCContext &__restrict, uint8_t *), const uint32_t *values, const uint8_t *types, uint32_t count)
  {
    if (!g_scratch)
    {
      auto *memory = rex::system::kernel_memory();
      g_scratch = memory ? memory->SystemHeapAlloc(64) : 0;

      if (!g_scratch)
        return;
    }

    count = std::min(count, 8u);
    std::memset(GuestPtr(base, g_scratch), 0, 48);

    for (uint32_t i = 0; i < count; ++i)
    {
      Store32(base, g_scratch + 8 + 4 * i, values[i]);
      *GuestPtr(base, g_scratch + 40 + i) = types[i];
    }

    PPCContext c = ctx;
    c.r3.u64 = g_scratch;
    c.r5.u64 = g_scratch + 8;
    c.r6.u64 = g_scratch + 40;
    c.r7.u64 = count;
    c.r8.u64 = ctx.r6.u64;
    c.r9.u64 = ctx.r7.u64;
    native(c, base);
  }

  void SceneShow(PPCContext &ctx, uint8_t *base, uint32_t object, bool show)
  {
    const uint8_t hash = 3;
    SceneCall(ctx, base, show ? sub_82282BB8 : sub_82282C28, &object, &hash, 1);
  }

  // Shows the diagram for the device in use (keyboard: the Xbox pad's, drawn as a keyboard by the prompt textures)
  void UpdateControllerDiagram(PPCContext &ctx, uint8_t *base, bool force)
  {
    const bool ps = WantPs3Diagram();

    if (!force && ps == g_controller_ps)
      return;

    g_controller_ps = ps;
    const uint32_t shown = ps ? kPs3PadGroup : kXboxPadGroup;
    SceneShow(ctx, base, shown, true);
    SceneShow(ctx, base, ps ? kXboxPadGroup : kPs3PadGroup, false);

    // (Live switch: the newly shown diagram plays the page's last animation, so it has the same layout)
    if (!force && g_pad_anim.count)
    {
      PadAnim a = g_pad_anim;
      a.values[0] = shown;
      SceneCall(ctx, base, sub_82282AA0, a.values, a.types, a.count);
    }
    REXLOG_INFO("[sst-menu] controls page: {} diagram", ps ? "DualShock 3" : "Xbox 360 / keyboard");
  }

  // VIBRATION greyed out (half alpha, as the game greys the main menu's Xbox LIVE item) while the keyboard is in use: the texts of the controls page's row and of the pause menu's row
  struct Dim
  {
    uint32_t hash;
    bool active = false;
    uint8_t written = 0, original = 0xFF;
  };

  Dim g_dim_controls[] = {{0x7D139BC8}, {0x9FFE01DC}, {0xC9C4FF84}}; // VIBRATION, OFF, ON (SETTINGS_CONTROLLER\OPTION_2)
  Dim g_dim_pause[] = {{0x2408EA3F}, {0xAF864C0C}};                  // VIBRATION, its value (pause menu row F8A9F905)

  uint32_t FindObject(PPCContext &ctx, uint8_t *base, uint32_t hash)
  {
    PPCContext c = ctx;
    c.r3.u64 = 0x82DB4370;
    c.r4.u64 = hash;
    sub_82582110(c, base);
    return c.r3.u32;
  }

  void SetAlpha(PPCContext &ctx, uint8_t *base, uint32_t hash, uint32_t alpha)
  {
    const uint32_t values[2] = {hash, alpha};
    const uint8_t types[2] = {3, 2};
    SceneCall(ctx, base, sub_82283318, values, types, 2);
  }

  // Each frame: halves the texts' alpha (the game's own alpha changes - fades, highlights - are followed), or puts it back
  template <size_t N>
  void DimTexts(PPCContext &ctx, uint8_t *base, Dim (&dims)[N], bool dim)
  {
    for (Dim &d : dims)
    {
      if (!dim && !d.active)
        continue;

      const uint32_t obj = FindObject(ctx, base, d.hash);

      if (!obj)
      {
        d.active = false;
        continue;
      }

      const uint8_t a = *GuestPtr(base, obj + 0xB);

      if (dim)
      {
        if (!d.active || a != d.written)
        {
          d.original = a;
          d.written = uint8_t(a / 2);
          d.active = true;
          SetAlpha(ctx, base, d.hash, d.written);
        }
      }
      else
      {
        if (a == d.written)
          SetAlpha(ctx, base, d.hash, d.original);

        d.active = false;
      }
    }
  }
}

// Script VM: call native r5 (group << 8 | index)
extern "C" REX_FUNC(sub_821DFBC0)
{
  const uint32_t id = ctx.r5.u32 & 0xFFFF;

  if ((id >> 8) == kNativeGroup)
  {
    HandleNative(ctx, base, id);
    // Scene animations the native asked for
    std::vector<Anim> anims;
    {
      std::lock_guard lock(g_mutex);
      anims.swap(g_anims);
    }

    for (const Anim &a : anims)
    {
      if (a.key == kAnimShow || a.key == kAnimHide)
      {
        SceneShow(ctx, base, a.object, a.key == kAnimShow);
        continue;
      }

      const uint32_t v[4] = {a.object, a.key, a.key, 0};
      const uint8_t t[4] = {3, 3, 3, 2};
      SceneCall(ctx, base, sub_82282AA0, v, t, 4);
    }
    return;
  }

  // Controls page up: the diagram follows the device in use (checked at each native the page's frame calls)
  if (g_controller_open)
    UpdateControllerDiagram(ctx, base, false);

  // A fourth navbar button on Y: Key Bindings on the CONTROLS page (keyboard in use), Reset to Defaults on KEY BINDINGS
  const bool keys_page = g_list.keys && !g_controller_open;

  if (((g_controller_open && g_keys_hint) || keys_page) && id == 0x205 && ctx.r10.u32 == 3 && ctx.r8.u32 && Load32(base, ctx.r8.u32) == kNavbarButtons && *GuestPtr(base, ctx.r9.u32 + 2) == 6)
  {
    if (!g_navbar)
    {
      auto *memory = rex::system::kernel_memory();
      g_navbar = memory ? memory->SystemHeapAlloc(0x80) : 0;
    }

    uint32_t label = 0;
    {
      std::lock_guard lock(g_mutex);
      SetupGuest(base);
      label = TextId(base, keys_page ? "Reset to Defaults" : "Key Bindings");
    }

    if (g_navbar && label)
    {
      // A static array object (as a module's): {0, 0xFFFF8800 (no reference count), count, elements (relative), 0x80000000 | capacity}, elements {value, type}
      const std::pair<uint32_t, uint32_t> elems[] = {{0, 2}, {0xF1141A0A, 3}, {6, 2}, {0xFBD92248, 3}, {2, 2}, {0x3A648438, 3}, {8, 2}, {label, 3}};
      // (+0: the class pointer of the VM's arrays, as the page's own array has it)
      Store32(base, g_navbar, Load32(base, Load32(base, ctx.r8.u32 + 8)));
      Store32(base, g_navbar + 4, 0xFFFF8800);
      Store32(base, g_navbar + 8, uint32_t(std::size(elems)));
      Store32(base, g_navbar + 12, 0x14);
      Store32(base, g_navbar + 16, 0x80000000u | uint32_t(std::size(elems)));

      for (size_t i = 0; i < std::size(elems); ++i)
      {
        Store32(base, g_navbar + 0x14 + 8 * uint32_t(i), elems[i].first);
        Store32(base, g_navbar + 0x18 + 8 * uint32_t(i), elems[i].second);
      }

      Store32(base, ctx.r8.u32 + 8, g_navbar);
    }
  }

  // VIBRATION greyed while the keyboard is in use: the controls page (its frame's input read) and the pause menu (3:18 input reads in a match)
  {
    const bool keyboard = sst::input::CurrentPromptStyle() == sst::input::PromptStyle::kKeyboard;

    if (g_controller_open && id == 0x306 && ctx.r10.u32 == 1 && ctx.r8.u32 && Load32(base, ctx.r8.u32) == kInputEvents)
      DimTexts(ctx, base, g_dim_controls, keyboard);

    if (id == 0x312 && sst::input::InMatch())
    {
      DimTexts(ctx, base, g_dim_pause, keyboard);

      // The pause menu's script reads input (3:18) only while the menu is up: Esc is "back" there, not Start (r4 = the calling VM, +8 its module)
      if (ctx.r4.u32)
      {
        const uint32_t module = Load32(base, ctx.r4.u32 + 8);
        std::lock_guard lock(g_pause_mutex);

        for (auto &[orig, patched] : g_pause_modules)
          if (module && (module == orig || module == patched))
          {
            sst::input::PauseMenuFrame();

            // The VIDEO page's scroll bar stays hidden on the rest of the pause menu
            if (!g_scroll_bar_on.load())
              SceneShow(ctx, base, pause::kScrollGroup, false);

            break;
          }
      }
    }
  }

  // Controls page: Y leaves it (as Confirm, keeping the layout type) for KEY BINDINGS
  if (g_controller_open && g_keys_hint && id == 0x306 && ctx.r10.u32 == 1 && ctx.r8.u32 && Load32(base, ctx.r8.u32) == kInputEvents && sst::input::YPresses() != g_y_seen)
  {
    g_y_seen = sst::input::YPresses();
    const uint32_t out = ctx.r3.u32;
    __imp__sub_821DFBC0(ctx, base);
    const uint32_t type = Load32(base, out + 4);

    if (type <= 3)
    {
      Store32(base, out, kEventSelect);
      Store32(base, out + 4, 3);
      g_open_keys = true;
      REXLOG_INFO("[sst-menu] controls page: Y -> KEY BINDINGS");
    }

    return;
  }

  if ((id >> 8) == 4 && ctx.r10.u32 >= 1 && ctx.r10.u32 <= 8 && ctx.r8.u32)
  {
    const uint32_t values = ctx.r8.u32;
    const uint32_t first = Load32(base, values);

    if ((id & 0xFF) == 5 && first == kPs3PadGroup)
    {
      // The page opens (its script hides the DualShock 3 diagram): show the one for the device in use (it may still be hidden from an earlier visit), hide the other. The script's own call stays (on the hidden one) for its result
      g_controller_open = true;
      g_pad_anim = {};
      g_keys_hint = sst::input::CurrentPromptStyle() == sst::input::PromptStyle::kKeyboard;
      g_y_seen = sst::input::YPresses();
      UpdateControllerDiagram(ctx, base, true);
      Store32(base, values, g_controller_ps ? kXboxPadGroup : kPs3PadGroup);
      __imp__sub_821DFBC0(ctx, base);
      return;
    }

    // The page closes
    if ((id & 0xFF) == 3 && first == kControllerLayer)
    {
      g_controller_open = false;
      g_controller_ps = false;
    }

    // An animation on the diagram (layout type): remembered for a live switch (hashes and numbers only)
    if (g_controller_open && (id & 0xFF) == 10 && (first == kXboxPadGroup || first == kPs3PadGroup))
    {
      PadAnim a;
      a.count = ctx.r10.u32;

      for (uint32_t i = 0; i < a.count; ++i)
      {
        a.values[i] = Load32(base, values + 4 * i);
        a.types[i] = *GuestPtr(base, ctx.r9.u32 + i);

        if (a.types[i] > 3)
          a.count = 0;
      }

      g_pad_anim = a;
    }

    // QUIT row: closing OPTIONS plays (dd6c3c34 -> f5fda6b0) on the rows below it, which for this row is the wrong distance; it goes back from where OPTIONS put it instead
    if ((id & 0xFF) == 19 && first == quit::kRow && ctx.r10.u32 >= 3)
    {
      const uint32_t from = Load32(base, values + 4), to = Load32(base, values + 8);

      if (to == quit::kOptionsTo)
      {
        g_quit_row_pushed = true;
      }
      else if (g_quit_row_pushed && from == 0xDD6C3C34u && to == 0xF5FDA6B0u)
      {
        Store32(base, values + 4, quit::kOptionsTo);
        Store32(base, values + 8, quit::kOptionsFrom);
        g_quit_row_pushed = false;
      }
      else
      {
        g_quit_row_pushed = false;
      }
    }

    if (g_controller_ps)
      for (uint32_t i = 0; i < ctx.r10.u32; ++i)
      {
        const uint32_t v = Load32(base, values + 4 * i);

        if (v == kXboxPadGroup || v == kPs3PadGroup)
          Store32(base, values + 4 * i, v == kXboxPadGroup ? kPs3PadGroup : kXboxPadGroup);
      }
  }

  __imp__sub_821DFBC0(ctx, base);
}

// SumoTool: text tables r3, text id r4 -> string (0 if not found)
extern "C" REX_FUNC(sub_82594AD0)
{
  if (g_restart_dialog && (ctx.r4.u32 == kDialogBody || ctx.r4.u32 == kDialogTitle))
  {
    SetupGuest(base);
    std::lock_guard lock(g_mutex);

    if (const uint32_t id = TextId(base, ctx.r4.u32 == kDialogBody ? kDialogBodyText : kDialogTitleText))
    {
      ctx.r3.u32 = g_guest.texts[id];
      return;
    }
  }

  // QUIT dialog: the game's "RETURN TO ONLINE MENU" YES / NO message with other texts
  if (g_quit_dialog && (ctx.r4.u32 == quit::kMessageBody || ctx.r4.u32 == quit::kMessageTitle))
  {
    SetupGuest(base);
    std::lock_guard lock(g_mutex);

    if (auto it = g_guest.texts.find(ctx.r4.u32 == quit::kMessageBody ? kTextQuitQuestion : quit::kTextLabel); it != g_guest.texts.end())
    {
      ctx.r3.u32 = it->second;
      return;
    }
  }

  // Settings: CONTROLLER -> CONTROLS
  if (std::find(std::begin(kTextController), std::end(kTextController), ctx.r4.u32) != std::end(kTextController) && REXCVAR_GET(sst_controls_name))
  {
    SetupGuest(base);
    std::lock_guard lock(g_mutex);

    if (const uint32_t id = TextId(base, "CONTROLS"))
    {
      ctx.r3.u32 = g_guest.texts[id];
      return;
    }
  }

  // Main menu: Xbox LIVE -> ONLINE PLAY (the item and the header above the menu)
  if ((ctx.r4.u32 == 0xA1B21EABu || ctx.r4.u32 == 0x2815E3A5u) && REXCVAR_GET(sst_online_play_name))
  {
    SetupGuest(base);
    std::lock_guard lock(g_mutex);

    if (const uint32_t id = TextId(base, "ONLINE PLAY"))
    {
      ctx.r3.u32 = g_guest.texts[id];
      return;
    }
  }

  if ((ctx.r4.u32 & 0xFFFF0000u) == kTextIdBase)
  {
    SetupGuest(base);
    std::lock_guard lock(g_mutex);
    const auto it = g_guest.texts.find(ctx.r4.u32);

    if (it != g_guest.texts.end())
    {
      ctx.r3.u32 = it->second;
      return;
    }
  }

  __imp__sub_82594AD0(ctx, base);
}

// Ultrawide layouts (scene_widen.h): when the game started with an aspect ratio wider than 16:9
bool WideLayouts()
{
  return REXCVAR_GET(sst_wide_layouts) && !g_boot_four_three && hud::ShiftFor(g_camera_aspect.load()) > 0.001f;
}
// The star field's spare columns (scene_widen.cpp) apply at any aspect but the game's own 4:3 mode
bool WideLayoutFor(const std::string &dat_name)
{
  return WideLayouts() || ((dat_name == "starbg.dat" || dat_name.starts_with("whtfade")) && REXCVAR_GET(sst_wide_layouts) && !g_boot_four_three);
}

// SumoTool reads scene files through sub_82594618 (synchronous) or sub_825946B0 (asynchronous: the in-match loader). Both: r3 path, r4 -> buffer pointer, r5 -> size, r6 alignment. Scenes the port rewrites get a buffer with room to grow (PauseMenu: the VIDEO row; others: wide layouts); the rewrite happens in sub_82595158, once both files (DAT, REL) are in. kind: 1 PauseMenu.DAT, 2 PauseMenu.REL, 3 another .DAT, 4 another .REL
void ReadPauseFile(PPCContext &ctx, uint8_t *base, int kind, bool async, const std::string &name)
{
  const uint32_t path = ctx.r3.u32, out_buf = ctx.r4.u32, out_size = ctx.r5.u32, align = ctx.r6.u32;
  ctx.r3.u32 = path;
  sub_82594820(ctx, base);
  const int32_t size = int32_t(ctx.r3.u32);
  Store32(base, out_size, uint32_t(size));

  if (size <= 0 || size > 0x800000)
  {
    ctx.r3.u32 = async ? 0 : uint32_t(size);
    return;
  }

  const uint32_t extra = kind == 1 ? 0x4000u : kind == 2 ? 0x1000u
                                                         : widen::ExtraRoom(uint32_t(size), kind == 4);
  const uint32_t room = (uint32_t(size) + extra + 0x7FF) & ~0x7FFu;
  ctx.r3.u32 = room;
  ctx.r4.u32 = align;
  sub_82122480(ctx, base);
  const uint32_t buffer = ctx.r3.u32;
  Store32(base, out_buf, buffer);

  if (buffer)
  {
    std::lock_guard lock(g_pause_mutex);
    g_pause_rooms[buffer] = {room, out_size, name};

    if (kind == 3)
      g_hud_buffers[buffer] = name;
  }

  ctx.r3.u32 = 0x82DB97A0; // the game's file system
  ctx.r4.u32 = path;
  ctx.r5.u32 = buffer;

  if (async)
  {
    ctx.r6.u32 = 0;
    sub_82128A90(ctx, base); // -> request handle
  }
  else
  {
    sub_821289D8(ctx, base);
    ctx.r3.u32 = uint32_t(size);
  }
}

// Other scene files: HUD scenes are remembered by buffer (anchored later)
template <void (*Original)(PPCContext &__restrict, uint8_t *)>
void ReadOtherFile(PPCContext &ctx, uint8_t *base)
{
  const uint32_t path = ctx.r3.u32, out_buf = ctx.r4.u32;
  const std::string name = GuestFileName(base, path);
  Original(ctx, base);

  if (name.ends_with(".dat"))
  { // every scene: off-screen elements (all) / HUD sides
    std::lock_guard lock(g_pause_mutex);
    g_hud_buffers[Load32(base, out_buf)] = name;
  }
}

// Which kind of scene file (see ReadPauseFile) a path is, 0 = read as usual
int SceneFileKind(uint8_t *base, uint32_t path, std::string &name)
{
  int kind = REXCVAR_GET(sst_graphics_menu) ? PauseSceneFile(base, path) : 0;
  name = GuestFileName(base, path);

  if (!kind)
  {
    const bool dat = name.ends_with(".dat"), rel = name.ends_with(".rel");
    const std::string as_dat = rel ? name.substr(0, name.size() - 4) + ".dat" : name;

    if ((dat || rel) && ((WideLayoutFor(as_dat) && widen::WantsWideLayout(as_dat)) || (as_dat == "sstmainmenu.dat" && (REXCVAR_GET(sst_quit_item) || REXCVAR_GET(sst_graphics_menu)))))
      kind = dat ? 3 : 4;
  }

  return kind;
}

extern "C" REX_FUNC(sub_82594618)
{
  std::string name;
  const int kind = SceneFileKind(base, ctx.r3.u32, name);

  if (!kind)
    return ReadOtherFile<__imp__sub_82594618>(ctx, base);

  ReadPauseFile(ctx, base, kind, false, name);
}

extern "C" REX_FUNC(sub_825946B0)
{
  std::string name;
  const int kind = SceneFileKind(base, ctx.r3.u32, name);

  if (!kind)
    return ReadOtherFile<__imp__sub_825946B0>(ctx, base);

  ReadPauseFile(ctx, base, kind, true, name);
}

// SumoTool: set up a scene from its DAT and REL, r4 = {DAT pointer, size}, r5 = {REL pointer, size} (pointer in the high word). The PauseMenu scene is patched here, both files together, or not at all
extern "C" REX_FUNC(sub_82595158)
{
  const uint32_t dat = uint32_t(ctx.r4.u64 >> 32), rel = uint32_t(ctx.r5.u64 >> 32);
  uint32_t dat_size = uint32_t(ctx.r4.u64), rel_size = uint32_t(ctx.r5.u64);
  uint32_t dat_room = 0, rel_room = 0, dat_size_at = 0, rel_size_at = 0;
  std::string hud_name, dat_name;
  {
    std::lock_guard lock(g_pause_mutex);

    if (auto it = g_hud_buffers.find(dat); it != g_hud_buffers.end())
    {
      hud_name = it->second;
      g_hud_buffers.erase(it);
    }

    if (auto it = g_pause_rooms.find(dat); it != g_pause_rooms.end())
      dat_room = it->second.room, dat_size_at = it->second.size_at, dat_name = it->second.name;

    if (auto it = g_pause_rooms.find(rel); it != g_pause_rooms.end())
      rel_room = it->second.room, rel_size_at = it->second.size_at;

    if (dat_room)
      g_pause_rooms.erase(dat);

    if (rel_room)
      g_pause_rooms.erase(rel);
  }
  // Ultrawide: in-match HUD elements to the screen edges
  if (!hud_name.empty() && dat && dat_size && !NativeFourThree())
  {
    const float shift = hud::ShiftFor(g_camera_aspect.load());

    if (shift > 0.0f)
    {
      std::vector<std::string> log;
      const bool edge_only = hud::IsEdgeHudScene(hud_name);
      const bool sides = edge_only || hud::IsHudScene(hud_name);
      const int moved = hud::Anchor(GuestPtr(base, dat), dat_size, shift, sides ? &log : nullptr, sides, edge_only);

      if (sides)
      {
        REXLOG_INFO("[sst-menu] {}: {} HUD elements to the screen edges (shift {:.3f})", hud_name, moved, shift);

        for (const std::string &line : log)
          REXLOG_INFO("[sst-menu]   {}", line);
      }
    }
  }

  const bool rooms = dat_room && rel_room && dat_size <= dat_room && rel_size <= rel_room;
  // Main menu: the QUIT row (before the wide layout, which then covers it too)
  if (rooms && dat_name == "sstmainmenu.dat" && REXCVAR_GET(sst_quit_item))
  {
    widen::MainMenuRow row{quit::kParent, quit::kRow, quit::kHighlight, quit::kFirstChild, quit::kTextLabel, quit::kBarLabel, quit::kOptionsFrom, quit::kOptionsTo, 0.0f};
    widen::Bytes new_dat, new_rel;
    std::string error;

    if (widen::AddMainMenuRow(GuestPtr(base, dat), dat_size, GuestPtr(base, rel), rel_size, row, new_dat, new_rel, error) && new_dat.size() <= dat_room && new_rel.size() <= rel_room)
    {
      std::memcpy(GuestPtr(base, dat), new_dat.data(), new_dat.size());
      std::memcpy(GuestPtr(base, rel), new_rel.data(), new_rel.size());

      if (Load32(base, dat_size_at) == dat_size)
        Store32(base, dat_size_at, uint32_t(new_dat.size()));

      if (Load32(base, rel_size_at) == rel_size)
        Store32(base, rel_size_at, uint32_t(new_rel.size()));

      dat_size = uint32_t(new_dat.size());
      rel_size = uint32_t(new_rel.size());
      ctx.r4.u64 = (uint64_t(dat) << 32) | dat_size;
      ctx.r5.u64 = (uint64_t(rel) << 32) | rel_size;
      std::lock_guard lock(g_pause_mutex);
      g_quit_scene_failed = false;
      REXLOG_INFO("[sst-menu] sstmainmenu.dat: QUIT row added");
    }
    else
    {
      std::lock_guard lock(g_pause_mutex);
      g_quit_scene_failed = true;
      REXLOG_ERROR("[sst-menu] sstmainmenu.dat: QUIT row not added ({})", error.empty() ? std::string("too large") : error);
    }
  }

  // Settings page: up / down arrows for the VIDEO / KEY BINDINGS lists (before the wide layout)
  if (rooms && dat_name == "sstmainmenu.dat" && REXCVAR_GET(sst_graphics_menu))
  {
    widen::Bytes new_dat, new_rel;
    std::string error;

    if (widen::AddListCarets(GuestPtr(base, dat), dat_size, GuestPtr(base, rel), rel_size, {kCaretUp, kCaretDown, kCaretHide, kCaretShow, 0x183E8C00u}, new_dat, new_rel, error) && new_dat.size() <= dat_room && new_rel.size() <= rel_room)
    {
      std::memcpy(GuestPtr(base, dat), new_dat.data(), new_dat.size());
      std::memcpy(GuestPtr(base, rel), new_rel.data(), new_rel.size());

      if (Load32(base, dat_size_at) == dat_size)
        Store32(base, dat_size_at, uint32_t(new_dat.size()));

      if (Load32(base, rel_size_at) == rel_size)
        Store32(base, rel_size_at, uint32_t(new_rel.size()));

      dat_size = uint32_t(new_dat.size());
      rel_size = uint32_t(new_rel.size());
      ctx.r4.u64 = (uint64_t(dat) << 32) | dat_size;
      ctx.r5.u64 = (uint64_t(rel) << 32) | rel_size;
      REXLOG_INFO("[sst-menu] sstmainmenu.dat: list arrows added");
    }
    else
    {
      REXLOG_WARN("[sst-menu] sstmainmenu.dat: list arrows not added ({})", error.empty() ? std::string("too large") : error);
    }
  }

  // Ultrawide: the scene's wide layout
  if (rooms && dat_name != "pausemenu.dat" && WideLayoutFor(dat_name) && widen::WantsWideLayout(dat_name))
  {
    std::vector<uint8_t> d(GuestPtr(base, dat), GuestPtr(base, dat) + dat_size);
    std::vector<uint8_t> r(GuestPtr(base, rel), GuestPtr(base, rel) + rel_size);
    widen::Bytes new_dat, new_rel;
    widen::Stats stats;
    std::vector<std::string> log;
    std::string error;
    const float shift = hud::ShiftFor(g_camera_aspect.load());

    if (!widen::Widen(dat_name, d.data(), d.size(), r.data(), r.size(), shift, new_dat, new_rel, stats, &log, error))
    {
      REXLOG_WARN("[sst-menu] {}: no wide layout ({})", dat_name, error);
    }
    else if (stats.Total())
    {
      if (new_dat.size() <= dat_room && new_rel.size() <= rel_room)
      {
        std::memcpy(GuestPtr(base, dat), new_dat.data(), new_dat.size());
        std::memcpy(GuestPtr(base, rel), new_rel.data(), new_rel.size());
        ctx.r4.u64 = (uint64_t(dat) << 32) | uint32_t(new_dat.size());
        ctx.r5.u64 = (uint64_t(rel) << 32) | uint32_t(new_rel.size());

        if (Load32(base, dat_size_at) == dat_size)
          Store32(base, dat_size_at, uint32_t(new_dat.size()));

        if (Load32(base, rel_size_at) == rel_size)
          Store32(base, rel_size_at, uint32_t(new_rel.size()));

        REXLOG_INFO("[sst-menu] {}: wide layout (shift {:.3f}): {} bars, {} mirrored strips, "
                    "{} cropped backgrounds, {} swoosh, {} stars (DAT {} -> {}, REL {} -> {} bytes)",
                    dat_name, shift, stats.bars, stats.strips, stats.cropped, stats.swoosh, stats.stars, dat_size, new_dat.size(), rel_size, new_rel.size());

        for (const std::string &line : log)
          REXLOG_INFO("[sst-menu]   {}", line);
      }
      else
      {
        REXLOG_WARN("[sst-menu] {}: wide layout too large for its buffer ({} / {})", dat_name, new_dat.size(), dat_room);
      }
    }
  }

  if (rooms && dat_name == "pausemenu.dat")
  {
    std::vector<uint8_t> d(GuestPtr(base, dat), GuestPtr(base, dat) + dat_size);
    std::vector<uint8_t> r(GuestPtr(base, rel), GuestPtr(base, rel) + rel_size);
    std::string error;
    // The scroll thumb's texture: the middle of the yellow 3P tag
    uint32_t thumb_tex = pause::kYellowTag;
    {
      widen::Bytes sd, sr;

      if (widen::AddSubTexture(d.data(), d.size(), r.data(), r.size(), pause::kYellowTag, pause::kThumbTex, 0.4f, 0.6f, 0.5f, 0.85f, sd, sr, error))
      {
        d = std::move(sd);
        r = std::move(sr);
        thumb_tex = pause::kThumbTex;
      }
      else
      {
        REXLOG_WARN("[sst-menu] pause scroll thumb texture: {}", error);
        error.clear();
      }
    }

    pause::Relocs relocs;
    auto new_dat = pause::PatchDat(d.data(), d.size(), relocs, error, thumb_tex);
    std::optional<pause::Bytes> new_rel;

    if (new_dat)
      new_rel = pause::PatchRel(r.data(), r.size(), relocs, error);

    std::lock_guard lock(g_pause_mutex);

    if (new_dat && new_rel && new_dat->size() <= dat_room && new_rel->size() <= rel_room)
    {
      std::memcpy(GuestPtr(base, dat), new_dat->data(), new_dat->size());
      std::memcpy(GuestPtr(base, rel), new_rel->data(), new_rel->size());
      ctx.r4.u64 = (uint64_t(dat) << 32) | uint32_t(new_dat->size());
      ctx.r5.u64 = (uint64_t(rel) << 32) | uint32_t(new_rel->size());
      // The loader's own record of the sizes (used again when the scene is released) must match the patched files
      if (Load32(base, dat_size_at) == dat_size)
        Store32(base, dat_size_at, uint32_t(new_dat->size()));

      if (Load32(base, rel_size_at) == rel_size)
        Store32(base, rel_size_at, uint32_t(new_rel->size()));

      g_pause_scene_failed = false;
      REXLOG_INFO("[sst-menu] PauseMenu scene patched: VIDEO row (DAT {} -> {}, REL {} -> {} bytes)", dat_size, new_dat->size(), rel_size, new_rel->size());
    }
    else
    {
      g_pause_scene_failed = true;
      REXLOG_ERROR("[sst-menu] PauseMenu scene not patched: {}", error.empty() ? std::string("too large") : error);
    }
  }

  __imp__sub_82595158(ctx, base);
}

// Perspective matrix builders (f1 vertical FOV, f2 aspect, f3 near, f4 far)
extern "C" void __imp__sub_8231F950(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_822C2480(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_821227C0(PPCContext &__restrict, uint8_t *);
extern "C" REX_FUNC(sub_8231F950)
{
  FixCameraAspect(ctx, 0x8231F950);
  __imp__sub_8231F950(ctx, base);
}

extern "C" REX_FUNC(sub_822C2480)
{
  FixCameraAspect(ctx, 0x822C2480);
  __imp__sub_822C2480(ctx, base);
}

extern "C" REX_FUNC(sub_821227C0)
{
  FixCameraAspect(ctx, 0x821227C0);
  __imp__sub_821227C0(ctx, base);
}