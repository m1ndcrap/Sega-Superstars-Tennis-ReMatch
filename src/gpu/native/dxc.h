// Runtime shader compiler (DXC), for DXIL (D3D12) or SPIR-V (Vulkan):
// DXIL: dxcompiler.dll next to the exe (the one the SDK's D3D12 provider loads; dxil.dll signs its output).
// SPIR-V: a DXC build with the SPIR-V back end (the Windows SDK's has none): <exe>/dxcompiler-spirv.dll on Windows, libdxcompiler.so next to the exe elsewhere (CMake copies both from a DXC release, see SST_DXC_SPIRV_DIR).

#pragma once

#include "gpu/native/gfx/gfx.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#endif
#include <dxcapi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sst::native::dxc
{
  // DxcCreateInstance of the compiler for `format` (loaded on first use)
  HRESULT CreateInstance(gfx::ShaderFormat format, REFCLSID clsid, REFIID iid, void **out);

  // The library's file name, for messages
  const char *LibraryName(gfx::ShaderFormat format);

  // An owning COM pointer (Microsoft::WRL::ComPtr and ATL's CComPtr aren't both available everywhere)
  template <class T>

  class Ref
  {
  public:
    Ref() = default;
    Ref(const Ref &) = delete;
    Ref &operator=(const Ref &) = delete;
    Ref(Ref &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }

    Ref &operator=(Ref &&o) noexcept
    {
      if (this != &o)
      {
        reset();
        p_ = o.p_;
        o.p_ = nullptr;
      }

      return *this;
    }

    ~Ref() { reset(); }

    T *operator->() const { return p_; }
    T *get() const { return p_; }

    explicit operator bool() const { return p_ != nullptr; }

    // For out-parameters: releases the current object
    void **put()
    {
      reset();
      return reinterpret_cast<void **>(&p_);
    }

    void reset()
    {
      if (p_)
        p_->Release();

      p_ = nullptr;
    }

  private:
    T *p_ = nullptr;
  };

  // Compiles HLSL (UTF-8) with `args` (the profile and options); returns the object, or the errors in `errors`
  bool Compile(IDxcCompiler3 *compiler, const std::string &source, const wchar_t *const *args, uint32_t arg_count, std::vector<uint8_t> &out, std::string *errors);
}