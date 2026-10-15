// sst_rematch - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "debug/crash_log.h"
#ifdef _WIN32
#include "gpu/d3d12_setup.h"
#endif
#include "gpu/native/native.h"
#include "input/controls.h"
#include "menu/achievements.h"
#include "menu/graphics_menu.h"
#include "menu/restart.h"

#include <filesystem>

#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/ui/window.h>

class SstRematchApp : public rex::ReXApp
{
public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext &ctx)
  {
    // Started by the VIDEO page's restart: let the previous run exit first.
    sst::menu::WaitForRestartParent();
    return std::unique_ptr<SstRematchApp>(new SstRematchApp(ctx, "sst_rematch",
                                                            PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig &paths) override
  {
    if (paths.game_data_root.empty())
    {
      paths.game_data_root = rex::filesystem::GetExecutableFolder() / "assets";
    }
  }

  // The GPU is a runtime-loaded plugin and the gpu_plugin cvar defaults to empty (= no GPU emulation). Without --gpu_plugin the port's own native renderer ("sst") is used when its plugin sits next to the exe, so double-clicking the exe starts it; else the SDK's emulated GPU ("xenos")
  void OnPreSetup(rex::RuntimeConfig &config) override
  {
    if (config.gpu_plugin.empty())
    {
      const auto dir = rex::filesystem::GetExecutableFolder();
      bool have_sst = false;

      for (const char *name : {"rexgpu-sst.dll", "rexgpu-sstd.dll", "rexgpu-sstrd.dll", "librexgpu-sst.so", "librexgpu-sst.dylib"})
        have_sst = have_sst || std::filesystem::exists(dir / name);

      config.gpu_plugin = have_sst ? "sst" : "xenos";
    }
#ifdef _WIN32
    // Dual-GPU PCs: the dedicated GPU unless --d3d12_adapter says otherwise
    sst::gpu::ConfigureDevice();
#endif
    // Keyboard controls (on by default) and the input system that tracks which device is in use, for the button prompts
    sst::input::ApplyDefaults();
    config.input_factory = [](bool tool_mode)
    { return sst::input::CreateInputSystem(tool_mode); };
  }

  // Video settings (sst_settings.ini, VIDEO page), then the native
  // renderer: attaches to the "sst" GPU plugin when it is the one loaded
  // (--gpu_plugin=sst), otherwise does nothing.
  void OnPostSetup() override
  {
    sst::menu::Initialize([this](bool fullscreen)
                          { app_context().CallInUIThread([this, fullscreen]
                                                         {
        if (auto* w = window(); w && w->IsFullscreen() != fullscreen) w->SetFullscreen(fullscreen); }); }, [this]
                          { app_context().RequestDeferredQuit(); });
    sst::native::Initialize(runtime());
    // Achievements in the game's style (Options > ACHIEVEMENTS, unlock popups)
    sst::ach::Initialize(achievements());
    // After the runtime's own handlers: a crash is logged with the guest functions on the stack
    sst::debug::InstallCrashLog();
  }
  void OnShutdown() override { sst::native::Shutdown(); }

  // The SDK's achievement overlay (F7) and unlock toasts are replaced by the game-styled page and popups (sst_achievements_ui)
  std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override
  {
    return sst::ach::UseGameUi() ? nullptr : rex::ReXApp::CreateAchievementsOverlay();
  }

  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override
  {
    return sst::ach::UseGameUi() ? nullptr : rex::ReXApp::CreateAchievementNotificationDialog();
  }

  // Other hooks available:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
};