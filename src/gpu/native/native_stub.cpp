// sst_rematch - native renderer stubs for platforms without D3D12

#include "gpu/native/native.h"

namespace sst::native
{

    void Initialize(rex::Runtime *) {}
    void Shutdown() {}
    bool Active() { return false; }
    void OnCreateShader(const uint8_t *, uint32_t, uint32_t) {}
    void OnSetStreamSource(const HookArgs &) {}
    void OnDrawIndexed(const uint8_t *, const HookArgs &) {}
    void OnDraw(const uint8_t *, const HookArgs &) {}
    void OnDrawUP(const uint8_t *, const HookArgs &) {}
    void OnClear(const uint8_t *, const HookArgs &) {}
    void OnSwap() {}

}