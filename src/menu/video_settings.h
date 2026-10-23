// The VIDEO settings: the rows of the VIDEO page, their values, quality presets, sst_settings.ini, and the cvars they set

#pragma once

#include "window/window_fit.h"

#include <array>
#include <functional>
#include <string>

namespace sst::menu::video
{
  enum Row : int
  {
    kPreset,
    kFullscreen,
    kShadows,
    kBloom,
    kShaderPrecision,
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

  constexpr std::array<RowInfo, kRows> kRowInfo = {{
      {"preset", "QUALITY PRESET", 4, false},
      {"fullscreen", "FULLSCREEN", 2, false},
      {"shadows", "SHADOWS", 6, false},
      {"bloom", "BLOOM", 2, false},
      {"shader_precision", "SHADER QUALITY", 2, false}, // "SHADER PRECISION" is wider than the label column was made for
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

  // The in-match VIDEO page: every row but those needing a restart (ASPECT RATIO: cameras are set up per match; SHADER PRECISION)
  constexpr int kPauseRowCount = kRows - 2;

  constexpr std::array<int, kPauseRowCount> kPauseRows = []
  {
    std::array<int, kPauseRowCount> rows{};
    int n = 0;

    for (int r = 0; r < kRows; ++r)
    {
      if (r != kAspect && r != kShaderPrecision)
        rows[n++] = r;
    }

    return rows;
  }();

  enum Preset : int
  {
    kLow,
    kMedium,
    kHigh,
    kCustom // shown, not picked
  };

  enum AaType : int
  {
    kAaOff,
    kFxaa,
    kMsaa
  };

  // Each row's value is its position in the row's list (see the ini's header comment for what they mean)
  struct Settings
  {
    std::array<int, kRows> v = {kMedium, 0, 4, 1, 0, 1, 1, 10, kAaOff, 1, 1, 0, 1, 10};
    bool operator==(const Settings &) const = default;
  };

  // What this run started with (changes to these wait for a restart)
  struct BootSettings
  {
    int aspect = 1;          // ASPECT RATIO
    int precision = 0;       // SHADER PRECISION: the renderer picks its shader pack once, when it starts
    bool four_three = false; // ASPECT RATIO 4:3: the game runs in its own 4:3 mode
  };

  // From the cvars (command line / config), with the matching preset
  Settings FromFlags();

  // sst_settings.ini over `s`; false if there is no file (or nothing in it)
  bool Load(Settings &s);
  void Save(const Settings &s);

  // Sets the cvars the renderer and presenter read; the window (fullscreen, windowed size and shape) goes through the handler (SetWindowHandler)
  void Apply(const Settings &s);
  void SetWindowHandler(std::function<void(const sst::window::Request &)> handler);

  void ApplyPreset(Settings &s, int preset);

  // Left / right on a row (dir -1 / +1); the preset follows the rows it covers
  void Change(Settings &s, int row, int dir);

  std::string ValueText(const Settings &s, int row);

  // "preset MEDIUM, fullscreen OFF, ..." (log lines)
  std::string Describe(const Settings &s);

  // The pause menu's fonts have fewer glyphs: the row's label / value in the letters they have
  std::string PauseLabel(int row);
  std::string PauseValue(const Settings &s, int row);

  // The settings in use. Callers hold MenuMutex() (menu_internal.h)
  Settings &Current();
  BootSettings &Boot();

  // This RESOLUTION value fits on the monitor at the aspect ratio
  bool ResolutionFits(const Settings &s, int value);

  // ASPECT RATIO / SHADER PRECISION differ from what this run started with
  bool RestartNeeded();
}