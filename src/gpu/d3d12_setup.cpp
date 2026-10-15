// sst_rematch

#include "gpu/d3d12_setup.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(sst_prefer_dedicated_gpu, true, "SST",
                    "On PCs with integrated and dedicated graphics, use the high-performance GPU "
                    "(unless --d3d12_adapter picks one)");

namespace sst::gpu
{
  using Microsoft::WRL::ComPtr;

  namespace
  {
    std::string Narrow(const wchar_t *w)
    {
      const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
      std::string s(n > 0 ? size_t(n - 1) : 0, '\0');

      if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);

      return s;
    }

    void PickHighPerformanceAdapter()
    {
      if (!REXCVAR_GET(sst_prefer_dedicated_gpu) || rex::cvar::Query<int32_t>("d3d12_adapter") != -1)
        return;

      ComPtr<IDXGIFactory6> factory;

      if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return; // older Windows: keep the SDK's choice

      ComPtr<IDXGIAdapter1> preferred;
      DXGI_ADAPTER_DESC1 want{};

      for (UINT i = 0; SUCCEEDED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&preferred))); ++i)
      {
        if (SUCCEEDED(preferred->GetDesc1(&want)) && !(want.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
          break;

        preferred.Reset();
      }

      if (!preferred)
        return;

      // The SDK counts adapters in EnumAdapters1 order: find the same one there (by LUID)
      uint32_t hardware = 0;
      int32_t index = -1;
      ComPtr<IDXGIAdapter1> a;

      for (UINT i = 0; factory->EnumAdapters1(i, &a) == S_OK; ++i, a.Reset())
      {
        DXGI_ADAPTER_DESC1 d{};

        if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
          continue;

        ++hardware;

        if (std::memcmp(&d.AdapterLuid, &want.AdapterLuid, sizeof(LUID)) == 0 && index < 0)
          index = int32_t(i);
      }

      if (hardware < 2 || index < 0)
        return; // one GPU: nothing to choose

      REXLOG_INFO("[sst-gpu] {} GPUs: using the high-performance one, {} (adapter {}; --d3d12_adapter=N picks another)", hardware, Narrow(want.Description), index);

      if (index != 0)
        rex::cvar::SetFlagByName("d3d12_adapter", std::to_string(index));
    }
  }

  void ConfigureDevice()
  {
    PickHighPerformanceAdapter();
  }
}