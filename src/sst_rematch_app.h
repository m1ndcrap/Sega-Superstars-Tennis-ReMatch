// sst_rematch - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "gpu/native/native.h"

#include <rex/filesystem.h>
#include <rex/rex_app.h>

class SstRematchApp : public rex::ReXApp
{
public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext &ctx)
  {
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

  // Native renderer
  void OnPostSetup() override { sst::native::Initialize(runtime()); }
  void OnShutdown() override { sst::native::Shutdown(); }
};