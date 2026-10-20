// Runtime shader compiler

#include "gpu/native/dxc.h"

#include <rex/filesystem.h>
#include <rex/logging.h>

#include <mutex>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace sst::native::dxc
{
  namespace
  {
    struct Library
    {
      std::once_flag once;
      DxcCreateInstanceProc create = nullptr;
    };

    Library g_libraries[2]; // by ShaderFormat

    DxcCreateInstanceProc Open(gfx::ShaderFormat format)
    {
      const std::filesystem::path path = rex::filesystem::GetExecutableFolder() / LibraryName(format);

#if defined(_WIN32)
      // DXIL: the copy the D3D12 provider loaded, if it did (same module); else the one next to the exe
      HMODULE module = format == gfx::ShaderFormat::kDxil ? GetModuleHandleW(L"dxcompiler.dll") : nullptr;

      if (!module)
        module = LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

      if (!module)
        return nullptr;

      return reinterpret_cast<DxcCreateInstanceProc>(reinterpret_cast<void *>(GetProcAddress(module, "DxcCreateInstance")));
#else
      void *module = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);

      if (!module)
        module = dlopen("libdxcompiler.so", RTLD_NOW | RTLD_LOCAL); // a system install (the Vulkan SDK's)

      if (!module)
        return nullptr;

      return reinterpret_cast<DxcCreateInstanceProc>(dlsym(module, "DxcCreateInstance"));
#endif
    }
  }

  const char *LibraryName(gfx::ShaderFormat format)
  {
#if defined(_WIN32)
    return format == gfx::ShaderFormat::kDxil ? "dxcompiler.dll" : "dxcompiler-spirv.dll";
#else
    (void)format;
    return "libdxcompiler.so";
#endif
  }

  HRESULT CreateInstance(gfx::ShaderFormat format, REFCLSID clsid, REFIID iid, void **out)
  {
    Library &lib = g_libraries[format == gfx::ShaderFormat::kDxil ? 0 : 1];
    std::call_once(lib.once, [&]
                   {
                     lib.create = Open(format);

                     if (!lib.create)
                       REXLOG_ERROR("[sst-native] shader compiler {} not found next to the exe", LibraryName(format)); });

    if (!lib.create)
      return E_FAIL;

    return lib.create(clsid, iid, out);
  }

  bool Compile(IDxcCompiler3 *compiler, const std::string &source, const wchar_t *const *args, uint32_t arg_count, std::vector<uint8_t> &out, std::string *errors)
  {
    DxcBuffer buffer = {source.data(), source.size(), DXC_CP_UTF8};
    Ref<IDxcResult> result;
    HRESULT status = E_FAIL;

    if (FAILED(compiler->Compile(&buffer, const_cast<LPCWSTR *>(args), arg_count, nullptr, __uuidof(IDxcResult), result.put())) || FAILED(result->GetStatus(&status)) || FAILED(status))
    {
      if (errors)
      {
        *errors = "DXC failed";
        Ref<IDxcBlobUtf8> err;

        if (result && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), err.put(), nullptr)) && err && err->GetStringLength())
          *errors += ":\n" + std::string(err->GetStringPointer(), err->GetStringLength());
      }

      return false;
    }

    Ref<IDxcBlob> object;

    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), object.put(), nullptr)) || !object || !object->GetBufferSize())
    {
      if (errors)
        *errors = "DXC returned no object";

      return false;
    }

    const auto *bytes = static_cast<const uint8_t *>(object->GetBufferPointer());
    out.assign(bytes, bytes + object->GetBufferSize());
    return true;
  }
}