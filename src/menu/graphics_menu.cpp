// sst_rematch - in-game VIDEO page

#include "menu/graphics_menu.h"
#include "input/controls.h"
#include "menu/hud_anchor.h"
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
    };

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
                       for (auto [id, text] : {std::pair<uint32_t, const char *>{pause::kTextBlank, " "}, {pause::kTextVideo, "VIDEO"}})
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
      int Count() const { return pause ? kRows - 1 : kRows; }
      int Row(int i) const { return pause && i >= kAspect ? i + 1 : i; } // list index -> Row
      int cursor = 0, top = 0;
      int old_visible = 0, new_visible = 0;
      bool redraw = false, leave = false;
      int sound = kNoSound;
    } g_list;

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
        REXLOG_INFO("[sst-menu] VIDEO page opened{}: {}", (id & 0xFF) == kPauseBegin ? " (pause menu)" : "", Describe(g_current));
        SetResult(base, out, 0, 2);
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
        std::string text = (id & 0xFF) == kLabel ? std::string(kList[row].label) : ValueText(g_current, row);
        SetResult(base, out, TextId(base, text), 3);
        break;
      }

      default:
        REXLOG_WARN("[sst-menu] unknown native {:04X}", id);
        SetResult(base, out, 0, 0);
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
        REXLOG_INFO("[sst-menu] \"video\" script -> the port's VIDEO page");
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
extern "C" void sub_82122480(PPCContext &__restrict, uint8_t *); // allocate (size, align)
extern "C" void sub_821289D8(PPCContext &__restrict, uint8_t *); // read file (fs, path, buffer)

// Front-end: start script r4 (name hash) with argument r5
extern "C" REX_FUNC(sub_821CCA28)
{
  {
    std::lock_guard lock(g_mutex);
    ApplyAspect(base);
  }

  if (ctx.r4.u32 == pause::kScriptHash && REXCVAR_GET(sst_graphics_menu))
  {
    SetupGuest(base);
    RedirectPauseScript(base, ctx.r3.u32);
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
  bool g_controller_ps = false;
}

// Script VM: call native r5 (group << 8 | index)
extern "C" REX_FUNC(sub_821DFBC0)
{
  const uint32_t id = ctx.r5.u32 & 0xFFFF;

  if ((id >> 8) == kNativeGroup)
  {
    HandleNative(ctx, base, id);
    return;
  }

  if ((id >> 8) == 4 && ctx.r10.u32 >= 1 && ctx.r10.u32 <= 8 && ctx.r8.u32)
  {
    const uint32_t values = ctx.r8.u32;
    const uint32_t first = Load32(base, values);

    if ((id & 0xFF) == 5 && first == kPs3PadGroup)
    {
      // The page opens: pick the diagram, show it (it may still be hidden from an earlier visit), hide the other
      g_controller_ps = REXCVAR_GET(sst_controller_diagram) && sst::input::CurrentPromptStyle() == sst::input::PromptStyle::kPlayStation;
      const uint32_t shown = g_controller_ps ? kPs3PadGroup : kXboxPadGroup, hidden = g_controller_ps ? kXboxPadGroup : kPs3PadGroup;
      PPCContext saved = ctx;
      Store32(base, values, shown);
      ctx.r5.u64 = (ctx.r5.u64 & ~0xFFFFull) | 0x404; // 4:4 show
      __imp__sub_821DFBC0(ctx, base);
      ctx = saved;
      Store32(base, values, hidden);
      REXLOG_INFO("[sst-menu] controller page: {} diagram", g_controller_ps ? "DualShock 3" : "Xbox 360");
      __imp__sub_821DFBC0(ctx, base);
      return;
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

    if ((dat || rel) && WideLayoutFor(as_dat) && widen::WantsWideLayout(as_dat))
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
  const uint32_t dat = uint32_t(ctx.r4.u64 >> 32), dat_size = uint32_t(ctx.r4.u64);
  const uint32_t rel = uint32_t(ctx.r5.u64 >> 32), rel_size = uint32_t(ctx.r5.u64);
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
      const bool sides = hud::IsHudScene(hud_name);
      const int moved = hud::Anchor(GuestPtr(base, dat), dat_size, shift, sides ? &log : nullptr, sides);

      if (sides)
      {
        REXLOG_INFO("[sst-menu] {}: {} HUD elements to the screen edges (shift {:.3f})", hud_name, moved, shift);

        for (const std::string &line : log)
          REXLOG_INFO("[sst-menu]   {}", line);
      }
    }
  }

  const bool rooms = dat_room && rel_room && dat_size <= dat_room && rel_size <= rel_room;
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
    pause::Relocs relocs;
    auto new_dat = pause::PatchDat(d.data(), d.size(), relocs, error);
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