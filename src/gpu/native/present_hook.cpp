// sst_rematch - VSync control

#include "gpu/native/present_hook.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include <dwmapi.h>
#include <dxgi1_5.h>
#include <windows.h>
#include <wrl/client.h>

REXCVAR_DECLARE(bool, sst_vsync);
REXCVAR_DEFINE_BOOL(sst_present_stats, true, "SST", "Log what the display actually showed every 600 presents (DXGI frame statistics): refreshes, new frames shown, frames replaced before being shown");

namespace sst::native
{
  namespace
  {
    using Microsoft::WRL::ComPtr;
    using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);

    // IUnknown (3) + IDXGIObject (4) + IDXGIDeviceSubObject (1) -> Present is #8
    constexpr size_t kPresentSlot = 8;

    PresentFn g_original = nullptr;
    std::once_flag g_once;
    std::atomic<int> g_last_mode{-1}; // logged state: 0 vsync, 1 tearing, 2 tearing unsupported

    // What reached the screen (--sst_present_stats): per logging period, the display refreshes that passed, the presents that were shown and the presents issued. Refreshes without a new frame are frames shown twice; issued but never shown are frames replaced (dropped) before their refresh. Also when in the refresh each Present call came (DWM's vblank clock): calls close to the next vblank are the ones at risk
    struct PresentStats
    {
      bool init = false, have_base = false;
      UINT base_present = 0, base_refresh = 0;
      uint64_t issued = 0, disjoint = 0, failed = 0;
      double phase_sum = 0, phase_max = 0, interval_max = 0;
      uint64_t phase_n = 0, phase_late = 0, intervals_over_20 = 0;
      int64_t last_qpc = 0;
      double qpf = 0;
      DWORD thread = 0;
      bool thread_changed = false;
      using DwmTimingFn = HRESULT(WINAPI *)(HWND, DWM_TIMING_INFO *);
      DwmTimingFn dwm_timing = nullptr;
    };

    PresentStats g_stats;

    void RecordPresent(IDXGISwapChain *swap_chain, UINT sync_interval, int64_t now)
    {
      PresentStats &s = g_stats;

      if (!s.init)
      {
        s.init = true;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s.qpf = double(f.QuadPart);

        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
          s.dwm_timing = reinterpret_cast<PresentStats::DwmTimingFn>(reinterpret_cast<void *>(GetProcAddress(dwm, "DwmGetCompositionTimingInfo")));

        s.thread = GetCurrentThreadId();
      }

      if (GetCurrentThreadId() != s.thread)
      {
        s.thread_changed = true;
        s.thread = GetCurrentThreadId();
      }

      ++s.issued;

      if (s.last_qpc)
      {
        const double ms = double(now - s.last_qpc) * 1000.0 / s.qpf;
        s.interval_max = std::max(s.interval_max, ms);
        s.intervals_over_20 += ms > 20.0;
      }

      s.last_qpc = now;

      if (s.dwm_timing)
      {
        DWM_TIMING_INFO ti = {};
        ti.cbSize = sizeof(ti);

        if (SUCCEEDED(s.dwm_timing(nullptr, &ti)) && ti.qpcRefreshPeriod > 0 && ti.qpcVBlank > 0)
        {
          const int64_t period = int64_t(ti.qpcRefreshPeriod);
          int64_t since = (now - int64_t(ti.qpcVBlank)) % period;

          if (since < 0)
            since += period;

          const double ms = double(since) * 1000.0 / s.qpf, period_ms = double(period) * 1000.0 / s.qpf;
          s.phase_sum += ms;
          s.phase_max = std::max(s.phase_max, ms);
          s.phase_late += ms > period_ms - 3.0;
          ++s.phase_n;
        }
      }

      DXGI_FRAME_STATISTICS fs = {};
      const HRESULT hr = swap_chain->GetFrameStatistics(&fs);

      if (hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT)
      {
        ++s.disjoint;
        s.have_base = false;
      }
      else if (FAILED(hr))
      {
        ++s.failed;
      }
      else if (!s.have_base)
      {
        s.have_base = true;
        s.base_present = fs.PresentCount;
        s.base_refresh = fs.SyncRefreshCount;
        s.issued = 1;
      }

      if (s.issued < 600)
        return;

      UINT last_present = 0;
      swap_chain->GetLastPresentCount(&last_present);

      if (s.have_base && SUCCEEDED(hr))
      {
        const uint64_t refreshes = uint64_t(fs.SyncRefreshCount - s.base_refresh), shown = uint64_t(fs.PresentCount - s.base_present);
        // (The statistics trail the presents by a frame or two; over 600 that evens out)
        REXLOG_INFO("[sst-native] display: {} refreshes, {} new frames shown ({} refreshes repeated the previous frame), {} presents issued ({} never shown); present sync interval {}",
                    refreshes, shown, refreshes > shown ? refreshes - shown : 0, s.issued, s.issued > shown ? s.issued - shown : 0, sync_interval);
      }
      else
      {
        REXLOG_INFO("[sst-native] display: frame statistics unavailable ({} disjoint, {} failed); {} presents issued, present sync interval {}", s.disjoint, s.failed, s.issued, sync_interval);
      }

      REXLOG_INFO("[sst-native] display: Present called {:.2f} ms after the display vblank avg / {:.2f} max ({} within 3 ms of the next vblank); present intervals over 20 ms: {} (longest {:.1f} ms){}",
                  s.phase_n ? s.phase_sum / double(s.phase_n) : 0.0, s.phase_max, s.phase_late, s.intervals_over_20, s.interval_max, s.thread_changed ? "; presented from more than one thread" : "");
      s.have_base = SUCCEEDED(hr);
      s.base_present = fs.PresentCount;
      s.base_refresh = fs.SyncRefreshCount;
      s.issued = 0;
      s.disjoint = s.failed = 0;
      s.phase_sum = s.phase_max = s.interval_max = 0;
      s.phase_n = s.phase_late = s.intervals_over_20 = 0;
      s.thread_changed = false;
    }

    HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain *swap_chain, UINT sync_interval, UINT flags)
    {
      const bool vsync = REXCVAR_GET(sst_vsync);
      int mode = 0;

      if (vsync)
      {
        flags &= ~DXGI_PRESENT_ALLOW_TEARING;
      }
      else if (sync_interval == 0)
      {
        DXGI_SWAP_CHAIN_DESC desc = {};
        const bool can_tear = SUCCEEDED(swap_chain->GetDesc(&desc)) && (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);

        if (can_tear)
        {
          flags |= DXGI_PRESENT_ALLOW_TEARING;
          mode = 1;
        }
        else
        {
          mode = 2;
        }
      }

      if (g_last_mode.exchange(mode) != mode)
      {
        static const char *kModes[] = {"VSync on (no tearing)", "VSync off (tearing allowed)",
                                       "VSync off requested, but the swap chain has no tearing support (run without "
                                       "--d3d12_allow_variable_refresh_rate_and_tearing=false)"};
        REXLOG_INFO("[sst-native] present: {}", kModes[mode]);
      }

      LARGE_INTEGER called;
      QueryPerformanceCounter(&called);
      const HRESULT hr = g_original(swap_chain, sync_interval, flags);

      if (REXCVAR_GET(sst_present_stats) && !(flags & DXGI_PRESENT_TEST))
        RecordPresent(swap_chain, sync_interval, int64_t(called.QuadPart));

      return hr;
    }

    LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
      return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void Install(const rex::ui::d3d12::D3D12Provider &provider)
    {
      IDXGIFactory2 *factory = provider.GetDXGIFactory();
      ID3D12CommandQueue *queue = provider.GetDirectQueue();

      if (!factory || !queue)
      {
        REXLOG_WARN("[sst-native] VSync control unavailable (no DXGI factory / queue)");
        return;
      }
      // A hidden window and a tiny flip-model swap chain: the same DXGI class as the presenter's, so it shares the vtable
      WNDCLASSW wc = {};
      wc.lpfnWndProc = DummyWndProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"sst_present_hook";
      RegisterClassW(&wc);
      HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);

      if (!hwnd)
      {
        REXLOG_WARN("[sst-native] VSync control unavailable (window: {})", GetLastError());
        return;
      }

      DXGI_SWAP_CHAIN_DESC1 desc = {};
      desc.Width = 16;
      desc.Height = 16;
      desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.BufferCount = 2;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      ComPtr<IDXGISwapChain1> swap_chain;
      const HRESULT hr = factory->CreateSwapChainForHwnd(queue, hwnd, &desc, nullptr, nullptr, &swap_chain);

      if (FAILED(hr))
      {
        REXLOG_WARN("[sst-native] VSync control unavailable (swap chain: {:08X})", uint32_t(hr));
        DestroyWindow(hwnd);
        return;
      }

      void **vtable = *reinterpret_cast<void ***>(swap_chain.Get());
      DWORD old_protect = 0;

      if (VirtualProtect(&vtable[kPresentSlot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protect))
      {
        g_original = reinterpret_cast<PresentFn>(vtable[kPresentSlot]);
        vtable[kPresentSlot] = reinterpret_cast<void *>(&HookedPresent);
        VirtualProtect(&vtable[kPresentSlot], sizeof(void *), old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), &vtable[kPresentSlot], sizeof(void *));
        REXLOG_INFO("[sst-native] VSync control installed (currently {})", REXCVAR_GET(sst_vsync) ? "on" : "off");
      }
      else
      {
        REXLOG_WARN("[sst-native] VSync control unavailable (vtable: {})", GetLastError());
      }

      swap_chain.Reset();
      DestroyWindow(hwnd);
    }
  }

  void InstallPresentHook(const rex::ui::d3d12::D3D12Provider &provider)
  {
    std::call_once(g_once, [&]
                   { Install(provider); });
  }
}