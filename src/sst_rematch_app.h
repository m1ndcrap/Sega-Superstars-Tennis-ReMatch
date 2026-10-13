// sst_rematch - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "gpu/native/native.h"
#include "menu/graphics_menu.h"
#include "menu/restart.h"

#include <rex/filesystem.h>
#include <rex/rex_app.h>
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

  // The GPU is now a runtime-loaded plugin and the gpu_plugin cvar defaults to empty (= no GPU emulation)
  void OnPreSetup(rex::RuntimeConfig &config) override
  {
    if (config.gpu_plugin.empty())
    {
      config.gpu_plugin = "xenos";
    }
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
  }
  void OnShutdown() override { sst::native::Shutdown(); }

  // Other hooks available:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
};