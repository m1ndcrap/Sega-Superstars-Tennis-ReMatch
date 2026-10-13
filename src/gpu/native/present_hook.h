// sst_rematch - VSync control (GRAPHICS page "VSYNC", cvar sst_vsync)

#pragma once

namespace rex::ui::d3d12
{
    class D3D12Provider;
}

namespace sst::native
{
    // Patches IDXGISwapChain::Present (found through a temporary swap chain)... Safe to call more than once.
    void InstallPresentHook(const rex::ui::d3d12::D3D12Provider &provider);
}