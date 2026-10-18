// VIDEO settings

#include "menu/video_settings.h"
#include "menu/port_text.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

REXCVAR_DEFINE_INT32(sst_resolution, 720, "SST", "Internal resolution in lines: 480, 720, 1080, 1440, 2160 (VIDEO page)");
REXCVAR_DEFINE_INT32(sst_render_scale, 100, "SST", "3D render scale in percent of the internal resolution, 50-100 (the UI stays at the internal resolution; VIDEO page)");
REXCVAR_DEFINE_INT32(sst_shadow_size, 1024, "SST", "Shadow map size: 0 (shadows off), 128-2048 (the game's is 1024; 3D textures stop at 2048 in D3D12; VIDEO page)");
REXCVAR_DEFINE_INT32(sst_aa_type, 0, "SST", "Anti-aliasing: 0 off, 1 FXAA, 2 MSAA (VIDEO page)");
REXCVAR_DEFINE_INT32(sst_aa_level, 4, "SST", "MSAA samples: 2, 4, 8 (VIDEO page)");
REXCVAR_DEFINE_INT32(sst_aspect, 1, "SST", "Aspect ratio: 0 4:3, 1 16:9, 2 16:10, 3 21:9, 4 32:9 (VIDEO page)");
REXCVAR_DEFINE_INT32(sst_fps_limit, 60, "SST", "Frame rate limit: 30 or 60 (VIDEO page)");
REXCVAR_DEFINE_BOOL(sst_vsync, true, "SST", "VSync (VIDEO page)");
REXCVAR_DEFINE_INT32(sst_brightness, 50, "SST", "Brightness 0-100, 50 = the game's default (VIDEO page)");
REXCVAR_DEFINE_BOOL(sst_fullscreen, false, "SST", "Borderless fullscreen (VIDEO page)");
REXCVAR_DEFINE_BOOL(sst_bloom, true, "SST", "Bloom / glow (VIDEO page)");
REXCVAR_DEFINE_BOOL(sst_fps_counter, false, "SST", "FPS counter in the top-left corner (VIDEO page)");
REXCVAR_DECLARE(bool, sst_fast_shaders); // SHADER PRECISION (gpu/native/shader_cache.cpp)

namespace sst::menu::video
{
  namespace
  {
    // What a row's list positions stand for
    constexpr int kShadowSizes[6] = {0, 128, 256, 512, 1024, 2048};
    constexpr int kResolutions[5] = {480, 720, 1080, 1440, 2160};
    constexpr const char *kAspects[5] = {"4:3", "16:9", "16:10", "21:9", "32:9"};
    constexpr int kAaLevels[3] = {2, 4, 8};
    constexpr int RenderScalePercent(int v) { return 50 + 5 * v; }
    constexpr int BrightnessPercent(int v) { return 5 * v; }

    // Quality presets: the rows they set
    struct PresetRows
    {
      int shadows, resolution, render_scale, aa_type, aa_level;
    };

    constexpr PresetRows kPresets[3] = {
        {3, 1, 10, kAaOff, 1}, // LOW: 512 shadows, 720p, 100%, AA off
        {4, 2, 10, kFxaa, 1},  // MEDIUM: 1024 shadows, 1080p, FXAA
        {5, 3, 10, kMsaa, 1},  // HIGH: 2048 shadows, 1440p, MSAA 4x
    };

    Settings g_current;
    BootSettings g_boot;
    std::function<void(bool)> g_set_fullscreen;

    // The preset the rows match (CUSTOM if none)
    void UpdatePreset(Settings &s)
    {
      s.v[kPreset] = kCustom;

      for (int p = 0; p < 3; ++p)
      {
        const PresetRows &q = kPresets[p];

        if (s.v[kShadows] == q.shadows && s.v[kResolution] == q.resolution && s.v[kRenderScale] == q.render_scale && s.v[kAaType] == q.aa_type && (q.aa_type != kMsaa || s.v[kAaLevel] == q.aa_level))
          s.v[kPreset] = p;
      }
    }

    // The list position whose value is nearest
    int NearestIndex(const int *values, int n, int value)
    {
      int best = 0;

      for (int i = 0; i < n; ++i)
      {
        if (std::abs(values[i] - value) < std::abs(values[best] - value))
          best = i;
      }

      return best;
    }

    std::filesystem::path SettingsPath()
    {
      return rex::filesystem::GetExecutableFolder() / "sst_settings.ini";
    }

    std::string Trim(const std::string &x)
    {
      const size_t a = x.find_first_not_of(" \t\r");
      const size_t b = x.find_last_not_of(" \t\r");
      return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
    }

    // In the letters the pause menu's fonts have: the row font (labels) A-Y without J/Q, 1-4, " ().?"; the value font also 0-9
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
  }

  Settings &Current() { return g_current; }
  BootSettings &Boot() { return g_boot; }

  void SetFullscreenHandler(std::function<void(bool)> handler) { g_set_fullscreen = std::move(handler); }

  Settings FromFlags()
  {
    Settings s;
    s.v[kResolution] = NearestIndex(kResolutions, 5, REXCVAR_GET(sst_resolution));
    s.v[kRenderScale] = std::clamp((REXCVAR_GET(sst_render_scale) - 50) / 5, 0, 10);
    s.v[kShadows] = NearestIndex(kShadowSizes, 6, REXCVAR_GET(sst_shadow_size));
    s.v[kAaType] = std::clamp(REXCVAR_GET(sst_aa_type), 0, 2);
    s.v[kAaLevel] = NearestIndex(kAaLevels, 3, REXCVAR_GET(sst_aa_level));
    s.v[kAspect] = std::clamp(REXCVAR_GET(sst_aspect), 0, 4);
    s.v[kFpsLimit] = REXCVAR_GET(sst_fps_limit) <= 30 ? 0 : 1;
    s.v[kVSync] = REXCVAR_GET(sst_vsync) ? 1 : 0;
    s.v[kBrightness] = std::clamp(REXCVAR_GET(sst_brightness) / 5, 0, 20);
    s.v[kFullscreen] = REXCVAR_GET(sst_fullscreen) ? 1 : 0;
    s.v[kBloom] = REXCVAR_GET(sst_bloom) ? 1 : 0;
    s.v[kFpsCounter] = REXCVAR_GET(sst_fps_counter) ? 1 : 0;
    s.v[kShaderPrecision] = REXCVAR_GET(sst_fast_shaders) ? 1 : 0;
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

      if (eq == std::string::npos || line[0] == '#' || line[0] == ';')
        continue;

      const std::string key = Trim(line.substr(0, eq));
      const int value = std::atoi(Trim(line.substr(eq + 1)).c_str());

      for (int r = 0; r < kRows; ++r)
      {
        if (key == kRowInfo[r].key)
        {
          s.v[r] = std::clamp(value, 0, kRowInfo[r].count - 1);
          any = true;
        }
      }
    }

    UpdatePreset(s);
    return any;
  }

  void Save(const Settings &s)
  {
    // Lines this page does not own (e.g. "language = french", read by menu/language.cpp) are kept
    std::vector<std::string> others;
    {
      std::ifstream in(SettingsPath());
      std::string line;

      while (in && std::getline(in, line))
      {
        const size_t eq = line.find('=');

        if (eq == std::string::npos || line.empty() || line[0] == '#' || line[0] == ';')
          continue;

        const std::string key = Trim(line.substr(0, eq));
        bool ours = false;

        for (int r = 0; r < kRows; ++r)
          ours = ours || key == kRowInfo[r].key;

        if (!ours)
          others.push_back(Trim(line));
      }
    }

    std::ofstream f(SettingsPath(), std::ios::trunc);

    if (!f)
    {
      REXLOG_ERROR("[sst-menu] could not write {}", SettingsPath().string());
      return;
    }

    f << "# sst_rematch video settings (Options > Settings > VIDEO). Values are list\n"
         "# positions: preset 0 low 1 medium 2 high 3 custom; shadows 0 off, 1-5 = 128-2048;\n"
         "# bloom 0 off 1 on; shader_precision 0 accurate 1 fast; resolution 0-4 = 480p 720p 1080p 1440p 2160p; aspect_ratio 0-4 = 4:3 16:9 16:10\n"
         "# 21:9 32:9; render_scale 0-10 = 50-100%; aa_type 0 off 1 FXAA 2 MSAA; aa_level\n"
         "# 0-2 = 2x 4x 8x; fps_limit 0 = 30, 1 = 60; brightness 0-20 = 0-100.\n"
         "# language: auto (Windows display language), english, french, german, spanish, italian (restart to apply).\n";

    for (int r = 0; r < kRows; ++r)
      f << kRowInfo[r].key << " = " << s.v[r] << "\n";

    bool language = false;

    for (const std::string &line : others)
    {
      f << line << "\n";
      language = language || Trim(line.substr(0, line.find('='))) == "language";
    }

    if (!language)
      f << "language = auto\n";
  }

  void Apply(const Settings &s)
  {
    auto set = [](const char *name, const std::string &value)
    { rex::cvar::SetFlagByName(name, value); };

    auto set_bool = [&](const char *name, int v)
    { set(name, v ? "true" : "false"); };

    set("sst_resolution", std::to_string(kResolutions[s.v[kResolution]]));
    set("sst_render_scale", std::to_string(RenderScalePercent(s.v[kRenderScale])));
    set("sst_shadow_size", std::to_string(kShadowSizes[s.v[kShadows]]));
    set("sst_aa_type", std::to_string(s.v[kAaType]));
    set("sst_aa_level", std::to_string(kAaLevels[s.v[kAaLevel]]));
    set("sst_aspect", std::to_string(s.v[kAspect]));
    set("sst_fps_limit", s.v[kFpsLimit] ? "60" : "30");
    set_bool("sst_vsync", s.v[kVSync]);
    set("sst_brightness", std::to_string(BrightnessPercent(s.v[kBrightness])));
    set_bool("sst_bloom", s.v[kBloom]);
    set_bool("sst_fps_counter", s.v[kFpsCounter]);
    set_bool("sst_fast_shaders", s.v[kShaderPrecision]); // read when the renderer starts
    set_bool("sst_fullscreen", s.v[kFullscreen]);

    if (g_set_fullscreen)
      g_set_fullscreen(s.v[kFullscreen] != 0);
  }

  void ApplyPreset(Settings &s, int preset)
  {
    const PresetRows &q = kPresets[preset];
    s.v[kShadows] = q.shadows;
    s.v[kResolution] = q.resolution;
    s.v[kRenderScale] = q.render_scale;
    s.v[kAaType] = q.aa_type;
    s.v[kAaLevel] = q.aa_level;
    s.v[kPreset] = preset;
  }

  void Change(Settings &s, int row, int dir)
  {
    if (row == kAaLevel && s.v[kAaType] != kMsaa)
      return; // only MSAA has levels

    const RowInfo &info = kRowInfo[row];
    const int count = row == kPreset ? 3 : info.count; // (CUSTOM is shown, not picked)
    int v = s.v[row];

    if (row == kPreset && v == kCustom)
      v = dir > 0 ? -1 : 3; // into LOW (right) or HIGH (left)

    v += dir;
    v = info.slider ? std::clamp(v, 0, count - 1) : (v + count) % count;

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

  std::string ValueText(const Settings &s, int row)
  {
    const int v = s.v[row];

    // Rows whose change waits for a restart show it
    auto restart = [](std::string text, bool changed)
    { return changed ? text + " (RESTART)" : text; };

    switch (row)
    {
    case kPreset:
      return v == kLow ? "LOW" : v == kMedium ? "MEDIUM"
                             : v == kHigh     ? "HIGH"
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

    case kShaderPrecision: // the shaders and pipelines are made once per run
      return restart(v ? "FAST" : "ACCURATE", v != g_boot.precision);

    case kAspect: // layouts are set up as scenes load
      return restart(kAspects[v], v != g_boot.aspect);

    case kRenderScale:
      return std::to_string(RenderScalePercent(v)) + "%";

    case kAaType:
      return v == kAaOff ? "OFF" : v == kFxaa ? "FXAA"
                                              : "MSAA";

    case kAaLevel:
      return s.v[kAaType] == kMsaa ? std::to_string(kAaLevels[v]) + "X" : "N/A";

    case kFpsLimit:
      return v ? "60" : "30";

    case kBrightness:
      return std::to_string(BrightnessPercent(v));
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

      out += kRowInfo[r].key;
      out += ' ';
      out += ValueText(s, r);
    }

    return out;
  }

  // The pause menu's fonts (PauseMenu's language file): rows 0x54291604, values 0x2207B8BE. In the game's language when the fonts are known (port_text.h: missing letters simplified or dropped); else the English font's letters (PauseText).
  namespace
  {
    constexpr uint32_t kPauseLabelFont = 0x54291604, kPauseValueFont = 0x2207B8BE;

    std::string PauseFit(std::string text, uint32_t font, bool digits)
    {
      if (!sst::text::FontKnown(nullptr, "pausemenu", font))
        return PauseText(text, digits);

      for (char &c : text)
      {
        if (c >= 'a' && c <= 'z')
          c = char(c - 'a' + 'A');
      }

      // As PauseText: 16:9 -> 16X9, ANTI-ALIASING -> ANTI ALIASING when the font has no ':' / '-'
      if (!sst::text::HasChar(nullptr, "pausemenu", font, ':'))
        std::replace(text.begin(), text.end(), ':', 'X');

      if (!sst::text::HasChar(nullptr, "pausemenu", font, '-'))
        std::replace(text.begin(), text.end(), '-', ' ');

      return sst::text::Fit(nullptr, text, "pausemenu", font);
    }
  }

  std::string PauseLabel(int row)
  {
    if (row == kPreset)
      return PauseFit(sst::text::Tr("PRESET"), kPauseLabelFont, false); // (English: no Q in the font)

    return PauseFit(sst::text::Tr(kRowInfo[row].label), kPauseLabelFont, false);
  }

  std::string PauseValue(const Settings &s, int row)
  {
    if (row == kAaLevel && s.v[kAaType] != kMsaa)
      return PauseFit(sst::text::Tr("NONE"), kPauseValueFont, true);

    return PauseFit(sst::text::Tr(ValueText(s, row)), kPauseValueFont, true);
  }

  // A new aspect ratio needs a restart: 4:3 is the game's own mode, and the wide layouts are made as scenes load (the main menu's at boot). So does a new shader precision: the renderer picks its shader pack once, at startup
  bool RestartNeeded()
  {
    return g_current.v[kAspect] != g_boot.aspect || g_current.v[kShaderPrecision] != g_boot.precision;
  }
}