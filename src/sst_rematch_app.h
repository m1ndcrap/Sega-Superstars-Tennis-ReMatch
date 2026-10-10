// sst_rematch - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "gpu/native/native.h"

#include <rex/filesystem.h>
#include <rex/rex_app.h>

class SstRematchApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<SstRematchApp>(new SstRematchApp(ctx, "sst_rematch",
        PPCImageConfig));
  }

  // Since v0.5-ish the SDK no longer assumes a game folder. Keep the old
  // behaviour: an "assets" folder next to the exe, unless --game_data_root
  // (or game_data_root in sst_rematch.toml) says otherwise.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (paths.game_data_root.empty()) {
      paths.game_data_root = rex::filesystem::GetExecutableFolder() / "assets";
    }
  }

  // The GPU is now a runtime-loaded plugin and the gpu_plugin cvar defaults to
  // empty (= no GPU emulation). Stage 1 uses the Xenia-derived Xenos backend.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }

  // Native renderer: attaches to the "sst" GPU plugin when it is the one
  // loaded (--gpu_plugin=sst), otherwise does nothing.
  void OnPostSetup() override { sst::native::Initialize(runtime()); }
  void OnShutdown() override { sst::native::Shutdown(); }

  // Other hooks available:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
};
