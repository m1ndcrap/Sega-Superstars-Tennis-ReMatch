// Windowed: shaped to the chosen aspect ratio, resizable (the aspect stays locked while dragging), and the internal resolution follows its size. For both fullscreen and windows, the monitor's size limits RESOLUTION on the VIDEO page

#pragma once

namespace rex::ui
{
  class Window;
}

namespace sst::window
{
  // What the VIDEO settings ask of the window
  struct Request
  {
    bool fullscreen = false;
    int lines = 720;             // RESOLUTION: the windowed picture's height in pixels when this (or the aspect) changes
    float aspect = 16.0f / 9.0f; // the aspect ratio the picture is shown at
  };

  // UI thread. Puts the window in or out of borderless fullscreen and reads its monitor's size. Windowed: locks dragging to 'aspect' and, when the resolution or the aspect changed since it last sized the window (or it never has), sizes the window, shrunk to fit the screen and centred on it.
  void Apply(rex::ui::Window &window, const Request &request);

  // The internal resolution in lines while windowed with sst_window_resolution on like the height of the picture in the window (the window less any letterbox), once the size has stayed the same for a moment (no new targets every frame of a drag) or else 0 (RESOLUTION applies).
  int RenderLines();

  // Any thread: the tallest picture (in lines) at 'aspect' the window's monitor can show like its full height, or its width / aspect when that is less, windowed or fullscreen, so the VIDEO page can gray out RESOLUTION values that cannot fit. 0: no limit (the monitor is not known yet).
  int MaxLines(float aspect);
}