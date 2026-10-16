// sst_rematch

#include "gpu/d3d12_setup.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstring>
#include <atomic>
#include <cwchar>
#include <string>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(sst_prefer_dedicated_gpu, true, "SST",
                    "On PCs with integrated and dedicated graphics, use the high-performance GPU "
                    "(unless --d3d12_adapter picks one)");

REXCVAR_DEFINE_BOOL(sst_d3d12_dred, true, "SST",
                    "Record where the GPU is in each command list (DRED), so a 'graphics device lost' crash logs what it "
                    "was drawing and which memory it faulted on");

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

  namespace
  {
    ID3D12Device *g_device = nullptr;
    DrawLabelFn g_label = nullptr;
    std::atomic<bool> g_dumped{false};

    void EnableDred()
    {
      if (!REXCVAR_GET(sst_d3d12_dred))
        return;

      ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
      HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
      auto get_debug = d3d12 ? reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(reinterpret_cast<void *>(GetProcAddress(d3d12, "D3D12GetDebugInterface"))) : nullptr;

      if (!get_debug || FAILED(get_debug(IID_PPV_ARGS(&settings))))
      {
        REXLOG_INFO("[sst-gpu] device-removed diagnostics (DRED) not available on this system");
        return;
      }

      settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      REXLOG_INFO("[sst-gpu] device-removed diagnostics (DRED) on");
    }

    const char *OpName(D3D12_AUTO_BREADCRUMB_OP op)
    {
      static const char *const kNames[] = {"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission"};
      return unsigned(op) < std::size(kNames) ? kNames[op] : "op";
    }

    bool IsDraw(D3D12_AUTO_BREADCRUMB_OP op) { return op == D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED || op == D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED; }

    std::string Narrow(const wchar_t *w, const char *a)
    {
      if (w)
        return Narrow(w);

      return a ? a : "(unnamed)";
    }
  }

  void ConfigureDevice()
  {
    PickHighPerformanceAdapter();
    EnableDred();
  }

  void SetDiagnosticsDevice(ID3D12Device *device, DrawLabelFn label)
  {
    g_device = device;
    g_label = label;

    if (!device)
      return;

    // Driver version of the adapter in use (bugs differ between Intel driver releases; laptops often run an OEM driver)
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;

    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
    {
      DXGI_ADAPTER_DESC1 desc{};
      LARGE_INTEGER umd{};
      adapter->GetDesc1(&desc);
      const bool have_version = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
      char name[128] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
      REXLOG_INFO("[sst-gpu] adapter {}: driver {}, {} MB dedicated video memory, {} MB shared system memory", name,
                  have_version ? fmt::format("{}.{}.{}.{}", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart)) : std::string("version unknown"),
                  desc.DedicatedVideoMemory >> 20, desc.SharedSystemMemory >> 20);

      // Intel drivers before the 31.0.101 series (e.g. OEM laptop drivers 30.0.100.x) drop UI draws at random: menus and the HUD blink out
      if (have_version && desc.VendorId == 0x8086 && HIWORD(umd.HighPart) < 31)
        REXLOG_WARN("[sst-gpu] this Intel graphics driver is old and known to make the UI flicker in this game: install Intel's current "
                    "driver (\"Intel 7th-10th Gen Processor Graphics - Windows\" or newer, from intel.com)");
    }
  }

  bool DumpDeviceRemoved()
  {
    if (!g_device)
      return false;

    const HRESULT reason = g_device->GetDeviceRemovedReason();

    if (reason == S_OK || g_dumped.exchange(true))
      return reason != S_OK;

    REXLOG_ERROR("[sst-gpu] GRAPHICS DEVICE REMOVED: reason {:08X}{}", uint32_t(reason), reason == DXGI_ERROR_DEVICE_HUNG ? " (GPU hung: work took too long, TDR)" : reason == DXGI_ERROR_DEVICE_RESET        ? " (device reset)"
                                                                                                                                                                 : reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? " (driver internal error)"
                                                                                                                                                                 : reason == DXGI_ERROR_INVALID_CALL          ? " (invalid call)"
                                                                                                                                                                 : reason == DXGI_ERROR_DEVICE_REMOVED        ? " (device removed)"
                                                                                                                                                                                                              : "");
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;

    if (FAILED(g_device->QueryInterface(IID_PPV_ARGS(&dred))))
    {
      REXLOG_ERROR("[sst-gpu]   no DRED data (sst_d3d12_dred off, or not supported)");
      return true;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};

    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
    {
      for (const D3D12_AUTO_BREADCRUMB_NODE *n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext)
      {
        const uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
        const std::string list = Narrow(n->pCommandListDebugNameW, n->pCommandListDebugNameA);

        if (done >= n->BreadcrumbCount)
          continue; // finished

        REXLOG_ERROR("[sst-gpu]   unfinished command list '{}' (queue '{}'): {} of {} operations done", list, Narrow(n->pCommandQueueDebugNameW, n->pCommandQueueDebugNameA), done, n->BreadcrumbCount);
        uint64_t frame = 0;
        std::swscanf(n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"", L"sst frame %llu", &frame);
        uint32_t draws_before = 0;

        for (uint32_t i = 0; i < n->BreadcrumbCount && i < done; ++i)
          draws_before += IsDraw(n->pCommandHistory[i]);

        const uint32_t from = done > 6 ? done - 6 : 0, to = std::min(n->BreadcrumbCount, done + 4);
        uint32_t draw = draws_before;

        for (uint32_t i = from; i < done; ++i)
          draw -= IsDraw(n->pCommandHistory[i]);

        for (uint32_t i = from; i < to; ++i)
        {
          const D3D12_AUTO_BREADCRUMB_OP op = n->pCommandHistory[i];
          std::string what;

          if (IsDraw(op))
          {
            what = g_label && frame ? g_label(frame, draw) : fmt::format("draw {}", draw);
            ++draw;
          }

          REXLOG_ERROR("[sst-gpu]     {} #{} {}{}{}", i < done ? "done   " : i == done ? "FAILED?"
                                                                                       : "pending",
                       i, OpName(op), what.empty() ? "" : ": ", what);
        }
      }
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};

    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA)
    {
      REXLOG_ERROR("[sst-gpu]   GPU page fault at VA {:016X}", uint64_t(fault.PageFaultVA));

      for (const D3D12_DRED_ALLOCATION_NODE *a = fault.pHeadExistingAllocationNode; a; a = a->pNext)
        REXLOG_ERROR("[sst-gpu]     live allocation there: {} (type {})", Narrow(a->ObjectNameW, a->ObjectNameA), int(a->AllocationType));

      for (const D3D12_DRED_ALLOCATION_NODE *a = fault.pHeadRecentFreedAllocationNode; a; a = a->pNext)
        REXLOG_ERROR("[sst-gpu]     recently FREED allocation there: {} (type {})", Narrow(a->ObjectNameW, a->ObjectNameA), int(a->AllocationType));
    }
    else
    {
      REXLOG_ERROR("[sst-gpu]   no GPU page fault recorded");
    }

    return true;
  }
}