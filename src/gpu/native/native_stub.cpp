// sst_rematch - native renderer stubs for platforms without D3D12 (see native.h).

#include "gpu/native/native.h"

namespace sst::native {

void Initialize(rex::Runtime*) {}
void Shutdown() {}
bool Active() { return false; }
void OnCreateShader(const uint8_t*, uint32_t, uint32_t) {}
void OnClear(uint32_t, uint32_t, float, uint32_t) {}
void OnSwap() {}

}  // namespace sst::native
