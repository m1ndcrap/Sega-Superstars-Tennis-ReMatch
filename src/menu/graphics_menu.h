// sst_rematch - in-game VIDEO page (Options > Settings > VIDEO)

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace sst::menu
{
    // Loads sst_settings.ini (if present) and applies it. `set_fullscreen` puts the window in or out of borderless fullscreen; `request_quit` closes the game (after a restart has been launched). Call once at startup.
    void Initialize(std::function<void(bool)> set_fullscreen, std::function<void()> request_quit);

    // The GPU, once known: on the first boot (no sst_settings.ini yet) picks the quality preset from its video memory and saves the settings.
    void OnGpuDetected(uint64_t video_memory_mb, const std::string &name);

    // ASPECT RATIO 4:3 at startup: the game runs in its own 4:3 mode (4:3 menus and HUD, 1280x1024 frame) until the next restart. Valid after Initialize.
    bool NativeFourThree();
}