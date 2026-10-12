// sst_rematch - VSync control

#include "gpu/native/present_hook.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>

#include <atomic>
#include <mutex>

#include <dxgi1_5.h>
#include <windows.h>
#include <wrl/client.h>

REXCVAR_DECLARE(bool, sst_vsync);

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

      return g_original(swap_chain, sync_interval, flags);
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