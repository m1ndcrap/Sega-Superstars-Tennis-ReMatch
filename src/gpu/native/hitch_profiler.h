// sst_rematch - game-thread sampling profiler (--sst_hitch_profiler)

#pragma once

namespace sst::native::hitch
{
    void OnGameFrame();
    void Shutdown();
}