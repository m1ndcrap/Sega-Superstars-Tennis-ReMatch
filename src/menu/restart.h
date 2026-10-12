// sst_rematch - restarting the game from the VIDEO page (ASPECT RATIO 4:3 is the game's own 4:3 mode, chosen at boot)

#pragma once

namespace sst::menu
{
    // Starts a new copy of the game with the same command line and working folder. The new copy waits for this one to exit (WaitForRestartParent). False if it could not be started (or on platforms without support).
    bool RestartGame();

    // At startup: if this run was started by RestartGame, waits (up to 15 s) for the previous run to exit, so the two never share the caches and log.
    void WaitForRestartParent();

    // Ends this process after `ms` milliseconds unless it has exited by then (after a restart, in case the normal shutdown stalls).
    void ExitSoon(unsigned ms);
}