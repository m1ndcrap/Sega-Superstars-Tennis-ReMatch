// mouse cursor hiding

#include "input/cursor.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#endif

REXCVAR_DEFINE_BOOL(sst_hide_cursor, true, "SST", "Hide the mouse cursor over the game window when the mouse is not moving");

namespace sst::input
{
#if defined(_WIN32)
  namespace
  {
    constexpr UINT_PTR kTimerId = 0x5357;
    constexpr int64_t kIdleMs = 2000;

    WNDPROC g_original = nullptr;
    HWND g_window = nullptr;
    POINT g_last_pos{-1, -1};
    int64_t g_last_move = 0;
    bool g_hidden = false;
    bool g_timer = false;
    bool g_any_window = false;

    int64_t NowMs()
    {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // The mouse moved since the last check
    bool Moved()
    {
      POINT p;

      if (!GetCursorPos(&p))
        return false;

      if (p.x == g_last_pos.x && p.y == g_last_pos.y)
        return false;

      g_last_pos = p;
      g_last_move = NowMs();
      return true;
    }

    bool OverClient(HWND hwnd)
    {
      POINT p;
      RECT r;

      if (!GetCursorPos(&p) || !ScreenToClient(hwnd, &p) || !GetClientRect(hwnd, &r))
        return false;

      return PtInRect(&r, p) && GetForegroundWindow() == hwnd;
    }

    LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
      if (!g_timer)
      {
        g_timer = true;
        SetTimer(hwnd, kTimerId, 250, nullptr);
      }

      switch (msg)
      {
      case WM_TIMER:
        if (wp == kTimerId)
        {
          Moved();

          if (!g_hidden && REXCVAR_GET(sst_hide_cursor) && NowMs() - g_last_move >= kIdleMs && OverClient(hwnd))
          {
            g_hidden = true;
            SetCursor(nullptr);
          }

          return 0;
        }

        break;

      case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
        {
          if (Moved())
            g_hidden = false;

          if (g_hidden)
          {
            SetCursor(nullptr);
            return TRUE;
          }
        }

        break;

      case WM_MOUSEMOVE:
        if (Moved())
          g_hidden = false;

        break;
      }

      return CallWindowProcW(g_original, hwnd, msg, wp, lp);
    }

    BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM lp)
    {
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      wchar_t cls[64] = {};
      GetClassNameW(hwnd, cls, 64);

      // SDL's window class (any visible top-level window of this process after 10 s)
      if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER) && (wcscmp(cls, L"SDL_app") == 0 || g_any_window))
      {
        *reinterpret_cast<HWND *>(lp) = hwnd;
        return FALSE;
      }

      return TRUE;
    }

  }

  void StartCursorHider()
  {
    static std::atomic<bool> started{false};

    if (started.exchange(true))
      return;

    std::thread(
        []
        {
          // Wait for the game window (up to a minute), then subclass it
          for (int i = 0; i < 240 && !g_window; ++i)
          {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            g_any_window = i >= 40;
            HWND found = nullptr;
            EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&found));

            if (found)
            {
              g_last_move = NowMs();
              g_original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(found, GWLP_WNDPROC));

              if (g_original && SetWindowLongPtrW(found, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Proc)))
              {
                g_window = found;

                // (the timer starts with the window's next message)
                PostMessageW(found, WM_NULL, 0, 0);
                REXLOG_INFO("[sst-input] mouse cursor: hidden after {} s without movement{}", kIdleMs / 1000, REXCVAR_GET(sst_hide_cursor) ? "" : " (off: --sst_hide_cursor=false)");
              }

              break;
            }
          }
        })
        .detach();
  }
#else
  // SDL: the cursor shows on mouse movement and hides after 2 s without it while it is over the game window
  namespace
  {
    constexpr uint64_t kIdleMs = 2000;
    std::atomic<uint64_t> g_last_move{0};
    std::atomic<bool> g_hidden{false};

    bool OnEvent(void *, SDL_Event *event)
    {
      // (on the thread that pumps events: the main thread)
      if (event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
      {
        g_last_move = SDL_GetTicks();

        if (g_hidden.exchange(false))
          SDL_ShowCursor();
      }

      return true;
    }

    uint32_t OnTimer(void *, SDL_TimerID, uint32_t interval)
    {
      if (!g_hidden && REXCVAR_GET(sst_hide_cursor) && SDL_GetTicks() - g_last_move >= kIdleMs)
      {
        SDL_RunOnMainThread(
            [](void *)
            {
              if (!g_hidden && SDL_GetMouseFocus() && SDL_GetTicks() - g_last_move >= kIdleMs)
              {
                g_hidden = true;
                SDL_HideCursor();
              }
            },
            nullptr, false);
      }

      return interval;
    }
  }

  void StartCursorHider()
  {
    static std::atomic<bool> started{false};

    if (started.exchange(true))
      return;

    g_last_move = SDL_GetTicks();
    SDL_AddEventWatch(OnEvent, nullptr);
    SDL_AddTimer(250, OnTimer, nullptr);
    REXLOG_INFO("[sst-input] mouse cursor: hidden after {} s without movement{}", kIdleMs / 1000, REXCVAR_GET(sst_hide_cursor) ? "" : " (off: --sst_hide_cursor=false)");
  }
#endif
}