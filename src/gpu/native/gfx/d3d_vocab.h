// The D3D12 / DXGI plain-data types the renderer describes resources and state with:
// - Windows: the real headers.
// - Other platforms: the same names and values for the subset the renderer uses (gfx/d3d_vocab_portable.h), with no COM interfaces.

#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi.h>
#else
#include "gpu/native/gfx/d3d_vocab_portable.h"
#endif