// Windowed, shaped to the aspect ratio, resizable, and the internal resolution follows its size; in any mode, the monitor's size (the VIDEO page's RESOLUTION limit)

#include "window/window_fit.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#else
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>
#endif

REXCVAR_DEFINE_BOOL(sst_window_resolution, true, "SST", "Windowed: the internal resolution follows the window's size (RESOLUTION on the VIDEO page sets the window's size). false: RESOLUTION is the internal resolution, as in fullscreen");
REXCVAR_DEFINE_BOOL(sst_window_lock_aspect, true, "SST", "Windowed: resizing the window keeps it at the chosen aspect ratio (else any shape, the picture letterboxed)");

namespace sst::window
{
  namespace
  {
    constexpr int kMinLines = 240;
    constexpr int kMaxLines = 4320;
    constexpr int64_t kSettleMs = 250; // the window size has to hold this long before the renderer re-makes its targets

    std::atomic<bool> g_fullscreen{false};
    std::atomic<float> g_aspect{16.0f / 9.0f};

    // Picture height in the window: the latest, when it last changed, and the one the renderer uses
    std::atomic<int> g_pending{0};
    std::atomic<int64_t> g_pending_ms{0};
    std::atomic<int> g_committed{0};

    // The window's monitor in pixels (MaxLines)
    std::atomic<int> g_monitor_w{0}, g_monitor_h{0};

    // The last size this module gave the window (UI thread only)
    bool g_sized = false;
    int g_sized_lines = 0;
    float g_sized_aspect = 0.0f;

    int64_t NowMs()
    {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void SetMonitor(int w, int h)
    {
      if (w <= 0 || h <= 0)
        return;

      if (w != g_monitor_w.load() || h != g_monitor_h.load())
        REXLOG_INFO("[sst-window] monitor {}x{}", w, h);

      g_monitor_w = w;
      g_monitor_h = h;
    }

    // The window's client area is now w x h pixels (UI thread): the picture is the largest 'aspect' rectangle in it
    void OnClientSize(int w, int h)
    {
      if (w <= 0 || h <= 0)
        return; // minimized

      const int picture = std::min(h, int(std::lround(float(w) / g_aspect.load())));

      if (picture != g_pending.load())
      {
        g_pending_ms = NowMs();
        g_pending = picture;
      }
    }

    // Client size for a picture at 'aspect', shrunk (same shape) to fit max_w x max_h
    void FitSize(int lines, float aspect, int max_w, int max_h, int &w, int &h)
    {
      h = std::clamp(lines, kMinLines, kMaxLines);
      w = int(std::lround(float(h) * aspect));

      if (max_w > 0 && w > max_w)
      {
        w = max_w;
        h = int(std::lround(float(w) / aspect));
      }

      if (max_h > 0 && h > max_h)
      {
        h = max_h;
        w = int(std::lround(float(h) * aspect));
      }

      h = std::max(h, kMinLines);
      w = std::max(w, int(std::lround(float(h) * aspect)));
    }

#if defined(_WIN32)
    HWND Hwnd(rex::ui::Window &window) { return static_cast<HWND>(window.GetNativeWindowHandle()); }

    // The frame around the client area
    RECT Frame(HWND hwnd)
    {
      RECT r{0, 0, 0, 0};
      const DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
      const DWORD ex_style = DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
      AdjustWindowRectExForDpi(&r, style, FALSE, ex_style, GetDpiForWindow(hwnd));
      return r;
    }

    // WM_SIZING: keep the client area at the aspect ratio. The side or corner being dragged moves; the opposite one stays put
    void LockAspect(HWND hwnd, WPARAM edge, RECT &r)
    {
      const RECT f = Frame(hwnd);
      const int fw = f.right - f.left, fh = f.bottom - f.top;
      const float a = g_aspect;
      int cw = (r.right - r.left) - fw, ch = (r.bottom - r.top) - fh;

      // Which dimension leads: the dragged side's; at a corner, whichever moved further (relative to the shape)
      bool height_leads = edge == WMSZ_TOP || edge == WMSZ_BOTTOM;

      if (edge != WMSZ_TOP && edge != WMSZ_BOTTOM && edge != WMSZ_LEFT && edge != WMSZ_RIGHT)
      {
        RECT now{};
        GetClientRect(hwnd, &now);
        height_leads = std::abs(ch - now.bottom) * a > std::abs(cw - now.right);
      }

      if (height_leads)
        cw = int(std::lround(float(ch) * a));
      else
        ch = int(std::lround(float(cw) / a));

      if (ch < kMinLines)
      {
        ch = kMinLines;
        cw = int(std::lround(float(ch) * a));
      }

      if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
        r.left = r.right - (cw + fw);
      else
        r.right = r.left + cw + fw;

      if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
        r.top = r.bottom - (ch + fh);
      else
        r.bottom = r.top + ch + fh;
    }

    // The monitor the window is on (most of it)
    bool GetMonitor(HWND hwnd, MONITORINFO &mi)
    {
      mi = {};
      mi.cbSize = sizeof(mi);
      return GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) != FALSE;
    }

    // Its size in pixels
    void ReadMonitor(HWND hwnd)
    {
      MONITORINFO mi;

      if (GetMonitor(hwnd, mi))
        SetMonitor(mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    }

    LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR)
    {
      switch (msg)
      {
      case WM_SIZING:
      {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);

        if (!REXCVAR_GET(sst_window_lock_aspect) || g_fullscreen)
          return result;

        LockAspect(hwnd, wp, *reinterpret_cast<RECT *>(lp));
        return TRUE;
      }

      case WM_GETMINMAXINFO:
      {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);

        if (!g_fullscreen)
        {
          const RECT f = Frame(hwnd);
          auto *info = reinterpret_cast<MINMAXINFO *>(lp);
          info->ptMinTrackSize.x = std::max<LONG>(info->ptMinTrackSize.x, (f.right - f.left) + LONG(std::lround(kMinLines * g_aspect.load())));
          info->ptMinTrackSize.y = std::max<LONG>(info->ptMinTrackSize.y, (f.bottom - f.top) + kMinLines);
        }

        return result;
      }

      case WM_SIZE:
        if (wp != SIZE_MINIMIZED)
        {
          OnClientSize(LOWORD(lp), HIWORD(lp));
          ReadMonitor(hwnd);
        }

        break;

      case WM_EXITSIZEMOVE:
      case WM_DISPLAYCHANGE:
        ReadMonitor(hwnd);
        break;

      case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, &Proc, id);
        break;
      }

      return DefSubclassProc(hwnd, msg, wp, lp);
    }

    // Once per window: custom message handler (aspect lock, minimum size, window and monitor size tracking)
    void Attach(HWND hwnd)
    {
      static HWND attached = nullptr;

      if (attached == hwnd)
        return;

      if (SetWindowSubclass(hwnd, &Proc, 0x53535457, 0))
        attached = hwnd;
      else
        REXLOG_WARN("[sst-window] could not attach to the game window ({}): no aspect lock while resizing", GetLastError());
    }

    void ReadClientSize(HWND hwnd)
    {
      RECT r{};

      if (!IsIconic(hwnd) && GetClientRect(hwnd, &r))
        OnClientSize(r.right, r.bottom);
    }

    // A window with a title bar gets a resizable frame and the maximize button
    void MakeResizable(HWND hwnd)
    {
      const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);

      if ((style & WS_CAPTION) != WS_CAPTION || (style & (WS_THICKFRAME | WS_MAXIMIZEBOX)) == (WS_THICKFRAME | WS_MAXIMIZEBOX))
        return;

      SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME | WS_MAXIMIZEBOX);
      SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Client area w x h for the picture, fitted to the monitor's work area (less the frame), centred on it
    bool SizeWindow(HWND hwnd, int lines, float aspect, int &w, int &h)
    {
      if (IsZoomed(hwnd))
        ShowWindow(hwnd, SW_RESTORE);

      MONITORINFO mi;

      if (!GetMonitor(hwnd, mi))
        return false;

      const RECT f = Frame(hwnd);
      const int fw = f.right - f.left, fh = f.bottom - f.top;
      const RECT &work = mi.rcWork;
      const int work_w = work.right - work.left, work_h = work.bottom - work.top;
      FitSize(lines, aspect, work_w - fw, work_h - fh, w, h);

      const int x = work.left + (work_w - (w + fw)) / 2;
      const int y = std::max<int>(work.top, work.top + (work_h - (h + fh)) / 2);
      return SetWindowPos(hwnd, nullptr, x, y, w + fw, h + fh, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
    }
#else
    SDL_Window *GameWindow()
    {
      int count = 0;
      SDL_Window **windows = SDL_GetWindows(&count);
      SDL_Window *w = windows && count > 0 ? windows[0] : nullptr;
      SDL_free(windows);
      return w;
    }

    // The size of the monitor the window is on, in pixels
    void ReadMonitor(SDL_Window *w)
    {
      const SDL_DisplayID display = w ? SDL_GetDisplayForWindow(w) : 0;
      const SDL_DisplayMode *mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;

      if (mode)
      {
        const float density = mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
        SetMonitor(int(std::lround(float(mode->w) * density)), int(std::lround(float(mode->h) * density)));
      }
    }

    bool OnEvent(void *, SDL_Event *event)
    {
      // (on the thread that pumps events: the UI thread)
      switch (event->type)
      {
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        OnClientSize(event->window.data1, event->window.data2);
        ReadMonitor(SDL_GetWindowFromID(event->window.windowID));
        break;

      case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        ReadMonitor(SDL_GetWindowFromID(event->window.windowID));
        break;

      case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        ReadMonitor(GameWindow());
        break;
      }

      return true;
    }

    void Attach()
    {
      static bool attached = false;

      if (!attached)
        attached = SDL_AddEventWatch(OnEvent, nullptr);
    }

    void ReadClientSize(SDL_Window *w)
    {
      int pw = 0, ph = 0;

      if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED) && SDL_GetWindowSizeInPixels(w, &pw, &ph))
        OnClientSize(pw, ph);
    }

    // Window coordinates may be logical points (macOS, Wayland): sizes go through the pixel density
    bool SizeWindow(SDL_Window *win, int lines, float aspect, int &w, int &h)
    {
      if (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)
        SDL_RestoreWindow(win);

      const float density = std::max(SDL_GetWindowPixelDensity(win), 0.25f);
      const SDL_DisplayID display = SDL_GetDisplayForWindow(win);
      SDL_Rect usable{};
      int top = 0, left = 0, bottom = 0, right = 0;
      SDL_GetWindowBordersSize(win, &top, &left, &bottom, &right);
      int max_w = 0, max_h = 0;

      if (display && SDL_GetDisplayUsableBounds(display, &usable))
      {
        max_w = int(float(usable.w - left - right) * density);
        max_h = int(float(usable.h - top - bottom) * density);
      }

      FitSize(lines, aspect, max_w, max_h, w, h);

      if (!SDL_SetWindowSize(win, int(std::lround(float(w) / density)), int(std::lround(float(h) / density))))
        return false;

      if (display)
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));

      return true;
    }
#endif
  }

  void Apply(rex::ui::Window &window, const Request &request)
  {
    const float aspect = std::clamp(request.aspect, 0.5f, 8.0f);
    g_aspect = aspect;
    g_fullscreen = request.fullscreen;

    if (window.IsFullscreen() != request.fullscreen)
      window.SetFullscreen(request.fullscreen);

#if defined(_WIN32)
    HWND hwnd = Hwnd(window);

    if (!hwnd)
      return;

    Attach(hwnd);
    ReadMonitor(hwnd);
#else
    SDL_Window *win = GameWindow();

    if (!win)
      return;

    Attach();
    ReadMonitor(win);
#endif

    if (request.fullscreen)
      return; // RESOLUTION applies; the windowed size is kept for later

    const bool resize = !g_sized || request.lines != g_sized_lines || std::abs(aspect - g_sized_aspect) > 0.001f;
    bool sized = false;
    int w = 0, h = 0;

#if defined(_WIN32)
    MakeResizable(hwnd);

    if (resize)
      sized = SizeWindow(hwnd, request.lines, aspect, w, h);

    ReadClientSize(hwnd);
#else
    SDL_SetWindowResizable(win, true);
    const float lock = REXCVAR_GET(sst_window_lock_aspect) ? aspect : 0.0f;
    SDL_SetWindowAspectRatio(win, lock, lock);

    if (resize)
      sized = SizeWindow(win, request.lines, aspect, w, h);

    ReadClientSize(win);
#endif

    if (resize)
    {
      g_sized = true;
      g_sized_lines = request.lines;
      g_sized_aspect = aspect;

      if (sized)
        REXLOG_INFO("[sst-window] windowed {}x{} ({:.3f}:1, RESOLUTION {}p{}); resizable{}, internal resolution {}", w, h, aspect, request.lines, h < request.lines ? ", shrunk to fit the screen" : "", REXCVAR_GET(sst_window_lock_aspect) ? " at this aspect ratio" : "", REXCVAR_GET(sst_window_resolution) ? "follows the window" : "from RESOLUTION");
    }
  }

  int RenderLines()
  {
    if (!REXCVAR_GET(sst_window_resolution) || g_fullscreen)
      return 0;

    const int pending = g_pending;

    if (pending <= 0)
      return 0; // no window size yet

    int committed = g_committed;

    // The first size at once; later ones once the window has stopped changing
    if (pending != committed && (committed == 0 || NowMs() - g_pending_ms.load() >= kSettleMs))
    {
      committed = pending;
      g_committed = committed;
    }

    return std::clamp(committed, kMinLines, kMaxLines);
  }

  int MaxLines(float aspect)
  {
    const int w = g_monitor_w, h = g_monitor_h;

    if (w <= 0 || h <= 0)
      return 0;

    return std::min(h, int(float(w) / std::clamp(aspect, 0.5f, 8.0f) + 0.5f));
  }
}