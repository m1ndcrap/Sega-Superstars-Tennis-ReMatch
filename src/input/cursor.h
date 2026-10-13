// sst_rematch - hides the mouse cursor over the game window after a moment without mouse movement (--sst_hide_cursor=false keeps it)

#pragma once

namespace sst::input
{
  // Starts watching for the game window (call once at startup)
  void StartCursorHider();
}