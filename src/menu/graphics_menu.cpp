// The port's menu changes: startup and the switches for each of them (menu_internal.h lists where each one lives)

#include "menu/graphics_menu.h"
#include "menu/menu_internal.h"
#include "menu/video_settings.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(sst_skip_intro, true, "SST", "The boot intro (SEGA movie, logo cards) can be skipped with Start / A (Enter on the keyboard)");
REXCVAR_DEFINE_BOOL(sst_online_play_name, true, "SST", "Main menu: the Xbox LIVE item is called ONLINE PLAY");
REXCVAR_DEFINE_BOOL(sst_quit_item, true, "SST", "Main menu: a QUIT item below OPTIONS (asks, then closes the game; Esc on the main menu asks too)");
REXCVAR_DEFINE_BOOL(sst_controls_name, true, "SST", "Settings: the CONTROLLER page is called CONTROLS");
REXCVAR_DEFINE_BOOL(sst_controller_diagram, true, "SST", "Settings > CONTROLLER shows the DualShock 3 diagram while a PlayStation pad is in use");
REXCVAR_DEFINE_BOOL(sst_wide_layouts, true, "SST", "Ultrawide aspect ratios: menus, loading screens and backgrounds get layouts that fill the extra width (generated from the 16:9 ones)");
REXCVAR_DEFINE_BOOL(sst_graphics_menu, true, "SST", "Replace Options > Settings > VIDEO with the port's VIDEO page");

namespace sst::menu
{
  namespace
  {
    std::mutex g_mutex;
    std::function<void()> g_request_quit;
    bool g_loaded_from_file = false; // sst_settings.ini existed (else the first boot picks a preset)
  }

  std::mutex &MenuMutex() { return g_mutex; }

  void RequestQuit()
  {
    if (g_request_quit)
      g_request_quit();
  }

  void Initialize(std::function<void(bool)> set_fullscreen, std::function<void()> request_quit)
  {
    std::lock_guard lock(g_mutex);
    video::SetFullscreenHandler(std::move(set_fullscreen));
    g_request_quit = std::move(request_quit);

    video::Settings &s = video::Current();
    s = video::FromFlags();
    g_loaded_from_file = video::Load(s);
    video::Apply(s);
    REXLOG_INFO("[sst-menu] video settings{}: {}", g_loaded_from_file ? " (sst_settings.ini)" : "", video::Describe(s));

    video::BootSettings &boot = video::Boot();
    boot.aspect = std::clamp(s.v[video::kAspect], 0, 4);
    boot.precision = s.v[video::kShaderPrecision];
    boot.four_three = boot.aspect == 0;
    ApplyCameraAspect(nullptr); // scenes loaded at boot already see the aspect

    // ASPECT RATIO 4:3: a 4:3 video mode (1280x1024, the game's HD 4:3 mode) makes the game pick its 4:3 layouts and cameras at boot
    if (boot.four_three)
    {
      rex::cvar::SetFlagByName("video_mode_width", "1280");
      rex::cvar::SetFlagByName("video_mode_height", "1024");
      REXLOG_INFO("[sst-menu] aspect ratio 4:3: the game's own 4:3 mode (video mode 1280x1024)");
    }
  }

  bool NativeFourThree()
  {
    return video::Boot().four_three;
  }

  void OnGpuDetected(uint64_t video_memory_mb, const std::string &name)
  {
    std::lock_guard lock(g_mutex);

    if (g_loaded_from_file)
      return;

    // First boot (no sst_settings.ini): a preset from the GPU's memory
    const int preset = video_memory_mb < 3000 ? video::kLow : video_memory_mb < 7000 ? video::kMedium
                                                                                     : video::kHigh;

    video::Settings &s = video::Current();
    video::ApplyPreset(s, preset);

    // LOW (integrated GPUs, small video memory): FAST shaders too. Before the renderer starts, so this run already uses them
    s.v[video::kShaderPrecision] = preset == video::kLow ? 1 : 0;
    video::Boot().precision = s.v[video::kShaderPrecision];
    video::Apply(s);
    video::Save(s);
    g_loaded_from_file = true;
    static const char *kNames[] = {"LOW", "MEDIUM", "HIGH"};
    REXLOG_INFO("[sst-menu] first boot: {} with {} MB video memory -> {} preset ({})", name, video_memory_mb, kNames[preset], video::Describe(s));
  }
}