// sst_rematch - native renderer: D3D12 frame recording and draws

#include "gpu/native/renderer.h"

#include "gpu/native/shader_cache.h"
#include "gpu/native/vertex_format.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include <xxhash.h>
#include <dxcapi.h>
#include <fmt/format.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>

REXCVAR_DEFINE_BOOL(sst_native_quads, false, "SST",
                    "Native renderer: draw QUADLIST / TRIFAN primitives (converted to triangle "
                    "lists). Off until textures work: full-screen passes would cover the scene");

REXCVAR_DEFINE_BOOL(sst_native_debug_shading, false, "SST",
                    "Native renderer: replace the game's pixel shaders with distance bands "
                    "(checks geometry independently of lighting / textures)");

REXCVAR_DEFINE_BOOL(sst_native_half_pixel, true, "SST",
                    "Native renderer: shift geometry by half a pixel (D3D9 pixel centres)");

REXCVAR_DEFINE_BOOL(sst_native_memexport, true, "SST",
                    "Native renderer: run memory-export (morph) vertex shaders on the CPU so "
                    "characters' faces and morphing meshes appear");

REXCVAR_DEFINE_BOOL(sst_native_memexport_cache, true, "SST",
                    "Native renderer: skip a memory-export draw when its inputs and previous "
                    "output are unchanged");

REXCVAR_DEFINE_INT32(sst_native_memexport_threads, -1, "SST",
                     "Native renderer: extra threads for memory-export (morph) shaders; -1 = "
                     "automatic (half the CPU threads minus one, at most 6)");

REXCVAR_DEFINE_BOOL(sst_native_check_vs, false, "SST",
                    "Native renderer debug: run the vertex shaders of face (morph) draws on the "
                    "CPU too and log vertices that jump on screen while the mesh stays put");

REXCVAR_DEFINE_BOOL(sst_native_check_vs_all, false, "SST",
                    "Native renderer debug: like sst_native_check_vs for every draw (slow)");

REXCVAR_DEFINE_BOOL(sst_native_skip_blended, false, "SST",
                    "Native renderer debug: skip alpha-blended draws (effects, glows, light "
                    "beams), which look like solid white shapes until textures are emulated");

REXCVAR_DEFINE_BOOL(sst_native_check_late_writes, false, "SST",
                    "Native renderer debug: report draws whose index / vertex data the game "
                    "changes after issuing the draw (within the frame)");

REXCVAR_DEFINE_INT32(sst_native_capture_frame, 0, "SST",
                     "Native renderer debug: capture this frame number (as F9 does): every "
                     "draw's vertex positions from the GPU are compared with the original "
                     "shader run on the CPU. -1 = every frame, logging only disagreements "
                     "(slow)");

REXCVAR_DEFINE_STRING(sst_native_rt, "auto", "SST",
                      "Native renderer: which render targets draw into the output until render "
                      "targets are emulated: auto (viewport covers 1280x720), all, or a size as "
                      "logged in 'render target' lines (e.g. 640x736)");

namespace sst::native
{
  using Microsoft::WRL::ComPtr;
  using rex::ui::d3d12::D3D12Presenter;
  using rex::ui::d3d12::D3D12Provider;

  namespace
  {
    constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    constexpr size_t kUploadPageSize = 32u << 20;
    constexpr uint32_t kMaxDrawVertices = 1u << 20;
    constexpr uint32_t kMaxDrawIndices = 1u << 21;

    // Root parameters (XenosRecomp D3D12 layout + SST raw-constant views)
    enum RootParam : UINT
    {
      kRootVSConstants = 0, // b0 space4
      kRootPSConstants,     // b1 space4
      kRootShared,          // b2 space4
      kRootVSConstantsRaw,  // b3 space4 (same buffer as b0)
      kRootPSConstantsRaw,  // b4 space4 (same buffer as b1)
      kRootTextures2D,      // t0 space0 (unbounded)
      kRootTextures3D,      // t0 space1
      kRootTexturesCube,    // t0 space2
      kRootSamplers,        // s0 space3
      kRootCount
    };

    // Placeholder descriptor indices (heap slots)

    constexpr uint32_t kPlaceholder2D = 0, kPlaceholder3D = 1, kPlaceholderCube = 2;
    constexpr uint32_t kSrvHeapSize = 16, kSamplerHeapSize = 16;

    // F9 capture: stream-output buffer (data, then one 16-byte counter per draw)
    constexpr size_t kSoDataBytes = 48u << 20; // 3M vertices
    constexpr uint32_t kSoMaxDraws = 8192;
    constexpr size_t kSoCounterBytes = size_t(kSoMaxDraws) * 16;

    // Slot for VS inputs the declaration doesn't provide (reads zeros)
    constexpr UINT kZeroInputSlot = 31;

    // guest memory
    const uint8_t *GuestPtr(const uint8_t *base, uint32_t addr)
    {
      return base + addr + rex::memory::detail::PhysicalHostOffset(addr);
    }

    uint32_t be32(const uint8_t *base, uint32_t addr)
    {
      uint32_t v;
      std::memcpy(&v, GuestPtr(base, addr), 4);
      return __builtin_bswap32(v);
    }

    uint32_t be32raw(const uint8_t *p)
    {
      uint32_t v;
      std::memcpy(&v, p, 4);
      return __builtin_bswap32(v);
    }

    // D3D object addresses are virtual; tolerate physical ones (map through the 0xA0000000 physical view)
    uint32_t ToVirtual(uint32_t addr) { return addr < 0x20000000u ? (addr | 0xA0000000u) : addr; }
    bool PlausibleAddress(uint32_t addr) { return addr >= 0x40000000u; }

    // Device register mirror (see native-renderer-d3d-layer.md): packed blocks
    uint32_t Reg(const uint8_t *base, uint32_t dev, uint32_t reg)
    {
      if (reg >= 0x2100 && reg <= 0x2114)
        return be32(base, dev + 0x28CC + 4 * (reg - 0x2100));

      if (reg >= 0x2200 && reg <= 0x220B)
        return be32(base, dev + 0x2934 + 4 * (reg - 0x2200));

      return 0;
    }

    float RegF(const uint8_t *base, uint32_t dev, uint32_t reg)
    {
      const uint32_t v = Reg(base, dev, reg);
      float f;
      std::memcpy(&f, &v, 4);
      return f;
    }

    enum : uint32_t
    {
      RB_COLOR_MASK = 0x2104,
      RB_BLEND_RED = 0x2105,
      RB_ALPHA_REF = 0x210E,
      PA_CL_VPORT_XSCALE = 0x210F,
      PA_CL_VPORT_XOFFSET = 0x2110,
      PA_CL_VPORT_YSCALE = 0x2111,
      PA_CL_VPORT_YOFFSET = 0x2112,
      PA_CL_VPORT_ZSCALE = 0x2113,
      PA_CL_VPORT_ZOFFSET = 0x2114,
      RB_DEPTHCONTROL = 0x2200,
      RB_BLENDCONTROL0 = 0x2201,
      RB_COLORCONTROL = 0x2202,
      PA_SU_SC_MODE_CNTL = 0x2205,
      PA_CL_VTE_CNTL = 0x2206,
    };

    // Device fields
    constexpr uint32_t kDevVSConstants = 0x780, kDevPSConstants = 0x1780, kDevBools = 0x2780;
    constexpr uint32_t kDevIndices = 0x3094, kDevRT0 = 0x3098, kDevDepth = 0x30A8;
    constexpr uint32_t kDevPS = 0x3194, kDevVS = 0x3198;

    // Fetch constants: slot j (2 dwords) at +0x480 + 8j. Textures use 3 slots each (sampler i = slots 3i..3i+2); vertex stream s is slot 95 - s
    constexpr uint32_t kDevFetchConstants = 0x480;

    D3D12_RESOURCE_BARRIER Transition(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = r;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = before;
      b.Transition.StateAfter = after;
      return b;
    }

    D3D12_BLEND BlendFactor(uint32_t f, bool alpha)
    {
      switch (f)
      {
      case 0:
        return D3D12_BLEND_ZERO;

      case 1:
        return D3D12_BLEND_ONE;

      case 4:
        return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;

      case 5:
        return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;

      case 6:
        return D3D12_BLEND_SRC_ALPHA;

      case 7:
        return D3D12_BLEND_INV_SRC_ALPHA;

      case 8:
        return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;

      case 9:
        return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;

      case 10:
        return D3D12_BLEND_DEST_ALPHA;

      case 11:
        return D3D12_BLEND_INV_DEST_ALPHA;

      case 12:
      case 14:
        return D3D12_BLEND_BLEND_FACTOR;

      case 13:

      case 15:
        return D3D12_BLEND_INV_BLEND_FACTOR;

      case 16:
        return D3D12_BLEND_SRC_ALPHA_SAT;

      default:
        return D3D12_BLEND_ONE;
      }
    }

    D3D12_BLEND_OP BlendOp(uint32_t op)
    {
      switch (op)
      {
      case 1:
        return D3D12_BLEND_OP_SUBTRACT;

      case 2:
        return D3D12_BLEND_OP_MIN;

      case 3:
        return D3D12_BLEND_OP_MAX;

      case 4:
        return D3D12_BLEND_OP_REV_SUBTRACT;

      default:
        return D3D12_BLEND_OP_ADD;
      }
    }

    // --sst_native_rt: which render targets draw into the single output until render targets are emulated. "auto" = targets whose viewport covers the 1280x720 frame (main scene + UI; skips shadow maps and bloom), "all", or a size as logged in 'render target' lines (e.g. 640x736).
    bool RenderTargetSelected(const uint8_t *base, uint32_t dev, uint32_t size_word)
    {
      const std::string filter = REXCVAR_GET(sst_native_rt);

      if (filter == "all" || filter.empty())
        return true;

      if (filter == "auto")
      {
        const float xs = std::fabs(RegF(base, dev, PA_CL_VPORT_XSCALE));
        const float ys = std::fabs(RegF(base, dev, PA_CL_VPORT_YSCALE));
        return xs == 640.0f && ys == 360.0f;
      }

      const std::string size = std::to_string((size_word >> 18) + 1) + "x" + std::to_string(((size_word >> 3) & 0x7FFF) + 1);
      return filter == size;
    }

  }

  struct PipelineKey
  {
    const ShaderVariant *vs;
    const ShaderVariant *ps;
    uint64_t layout_hash;
    uint32_t blend;     // RB_BLENDCONTROL0
    uint8_t color_mask; // RB_COLOR_MASK & 0xF
    uint8_t depth_enable, depth_write, depth_func;
    uint8_t cull, front_ccw;
    uint8_t topology_type; // D3D12_PRIMITIVE_TOPOLOGY_TYPE
    uint8_t strip_cut;     // 0 none, 1 0xFFFF, 2 0xFFFFFFFF
  };

  // setup
  bool Renderer::Initialize(const D3D12Provider &provider, ShaderCache *shaders, const uint8_t *physical_membase, uint32_t width, uint32_t height)
  {
    provider_ = &provider;
    physical_membase_ = physical_membase;

    // Guest physical memory is writable host memory; memexport writes into it
    {
      int32_t threads = REXCVAR_GET(sst_native_memexport_threads);

      if (threads < 0)
        threads = std::clamp(int32_t(std::thread::hardware_concurrency()) / 2 - 1, 0, 6);

      memexport_ = std::make_unique<MemexportRunner>(const_cast<uint8_t *>(physical_membase), uint32_t(std::min(threads, 16)));
      REXLOG_INFO("[sst-native] memexport: {} worker threads", memexport_->worker_threads());
    }

    device_ = provider.GetDevice();
    shaders_ = shaders;
    width_ = width;
    height_ = height;

    // Outputs + RTVs
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kSlots;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_))))
      return false;

    rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = D3D12Presenter::kGuestOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    for (uint32_t i = 0; i < kSlots; ++i)
    {
      if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&outputs_[i]))))
        return false;

      device_->CreateRenderTargetView(outputs_[i].Get(), nullptr, Rtv(i));

      if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frames_[i].allocator))))
        return false;
    }

    // Depth
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_))))
      return false;

    desc.Format = kDepthFormat;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kDepthFormat;
    clear.DepthStencil.Depth = 1.0f;

    if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth_))))
      return false;

    device_->CreateDepthStencilView(depth_.Get(), nullptr, dsv_heap_->GetCPUDescriptorHandleForHeapStart());

    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr, IID_PPV_ARGS(&list_))))
      return false;

    list_->Close();

    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
      return false;

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!event_)
      return false;

    if (!CreateRootSignature())
    {
      REXLOG_ERROR("[sst-native] failed to create the root signature");
      return false;
    }

    CreateDebugPixelShader();

    if (!CreateDescriptorsAndPlaceholders())
    {
      REXLOG_ERROR("[sst-native] failed to create placeholder textures");
      return false;
    }

    return true;
  }

  bool Renderer::CreateRootSignature()
  {
    D3D12_ROOT_PARAMETER params[kRootCount] = {};
    const UINT cbv_registers[] = {0, 1, 2, 3, 4};

    for (UINT i = 0; i < 5; ++i)
    {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      params[i].Descriptor.ShaderRegister = cbv_registers[i];
      params[i].Descriptor.RegisterSpace = 4;
      params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_DESCRIPTOR_RANGE ranges[4] = {};

    for (UINT i = 0; i < 4; ++i)
    {
      ranges[i].RangeType = i < 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      ranges[i].NumDescriptors = UINT_MAX; // unbounded (resource binding tier 2+)
      ranges[i].BaseShaderRegister = 0;
      ranges[i].RegisterSpace = i;
      ranges[i].OffsetInDescriptorsFromTableStart = 0;
      D3D12_ROOT_PARAMETER &p = params[kRootTextures2D + i];
      p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      p.DescriptorTable.NumDescriptorRanges = 1;
      p.DescriptorTable.pDescriptorRanges = &ranges[i];
      p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = kRootCount;
    rs.pParameters = params;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; // F9 capture
    ComPtr<ID3DBlob> blob, error;

    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
    {
      if (error)
        REXLOG_ERROR("[sst-native] root signature: {}", std::string(static_cast<const char *>(error->GetBufferPointer()), error->GetBufferSize()));

      return false;
    }

    return SUCCEEDED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature_)));
  }

  void Renderer::CreateDebugPixelShader()
  {
    // Colour by view distance (SV_Position.w) in bands, slightly darker on steep depth slopes so shapes read without lighting or textures
    static const char kSource[] =
        "float4 main(float4 p : SV_Position) : SV_Target0 {\n"
        "  float d = p.w;\n"
        "  float band = frac(d * 0.1);\n"
        "  float edge = saturate(1.0 - 40.0 * (abs(ddx(p.z)) + abs(ddy(p.z))));\n"
        "  float3 c = float3(0.35 + 0.65 * band, 0.5 + 0.5 * sin(d * 0.37), 1.0 - 0.6 * band);\n"
        "  return float4(c * (0.4 + 0.6 * edge), 1.0);\n"
        "}\n";

    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;

    if (FAILED(provider_->DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
      return;

    const wchar_t *args[] = {L"-T", L"ps_6_0", L"-E", L"main", L"-Qstrip_reflect", L"-Qstrip_debug"};
    DxcBuffer buffer = {kSource, sizeof(kSource) - 1, DXC_CP_UTF8};
    Microsoft::WRL::ComPtr<IDxcResult> result;
    HRESULT status = E_FAIL;

    if (FAILED(compiler->Compile(&buffer, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result))) || FAILED(result->GetStatus(&status)) || FAILED(status))
      return;

    Microsoft::WRL::ComPtr<IDxcBlob> object;

    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
      return;

    const auto *b = static_cast<const uint8_t *>(object->GetBufferPointer());
    debug_ps_ = std::make_unique<ShaderVariant>();
    debug_ps_->dxil.assign(b, b + object->GetBufferSize());
    debug_ps_->state.store(ShaderVariant::State::kReady);
  }

  bool Renderer::CreateDescriptorsAndPlaceholders()
  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvHeapSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap_))))
      return false;

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = kSamplerHeapSize;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sampler_heap_))))
      return false;

    const UINT srv_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT sampler_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    // White 1x1 placeholders: 2D, 3D, cube (6 faces)
    struct Placeholder
    {
      D3D12_RESOURCE_DIMENSION dim;
      UINT16 depth_or_array;
      D3D12_SRV_DIMENSION view;
      uint32_t heap_index;
    } const kinds[] = {
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, D3D12_SRV_DIMENSION_TEXTURE2D, kPlaceholder2D},
        {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 1, D3D12_SRV_DIMENSION_TEXTURE3D, kPlaceholder3D},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 6, D3D12_SRV_DIMENSION_TEXTURECUBE, kPlaceholderCube},
    };

    // One upload buffer: 8 subresources, 512 bytes each (aligned for copies)
    D3D12_HEAP_PROPERTIES upload_heap = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 8 * 512;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;

    if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging))))
      return false;

    uint8_t *mapped = nullptr;
    staging->Map(0, nullptr, reinterpret_cast<void **>(&mapped));
    std::memset(mapped, 0xFF, 8 * 512);

    // Zero buffer for missing vertex inputs (also upload heap; never written)
    bd.Width = 256;

    if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&zero_buffer_))))
      return false;

    uint8_t *zero = nullptr;
    zero_buffer_->Map(0, nullptr, reinterpret_cast<void **>(&zero));
    std::memset(zero, 0, 256);
    zero_buffer_->Unmap(0, nullptr);

    frames_[0].allocator->Reset();
    list_->Reset(frames_[0].allocator.Get(), nullptr);
    uint32_t staging_index = 0;

    for (const Placeholder &k : kinds)
    {
      D3D12_RESOURCE_DESC td = {};
      td.Dimension = k.dim;
      td.Width = 1;
      td.Height = 1;
      td.DepthOrArraySize = k.depth_or_array;
      td.MipLevels = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      ComPtr<ID3D12Resource> tex;

      if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
        return false;

      const UINT subresources = k.dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : k.depth_or_array;

      for (UINT s = 0; s < subresources; ++s)
      {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = s;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = staging.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 512 * staging_index++;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256};
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }

      auto barrier = Transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      list_->ResourceBarrier(1, &barrier);

      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      sd.ViewDimension = k.view;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

      if (k.view == D3D12_SRV_DIMENSION_TEXTURE2D)
        sd.Texture2D.MipLevels = 1;

      if (k.view == D3D12_SRV_DIMENSION_TEXTURE3D)
        sd.Texture3D.MipLevels = 1;

      if (k.view == D3D12_SRV_DIMENSION_TEXTURECUBE)
        sd.TextureCube.MipLevels = 1;

      D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
      h.ptr += size_t(k.heap_index) * srv_size;
      device_->CreateShaderResourceView(tex.Get(), &sd, h);
      placeholder_textures_.push_back(tex);
    }

    // Fill the rest of the heap with the 2D placeholder so any index is valid
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      sd.Texture2D.MipLevels = 1;

      for (uint32_t i = 3; i < kSrvHeapSize; ++i)
      {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(i) * srv_size;
        device_->CreateShaderResourceView(placeholder_textures_[0].Get(), &sd, h);
      }
    }

    // Samplers: linear wrap everywhere
    D3D12_SAMPLER_DESC smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;

    for (uint32_t i = 0; i < kSamplerHeapSize; ++i)
    {
      D3D12_CPU_DESCRIPTOR_HANDLE h = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
      h.ptr += size_t(i) * sampler_size;
      device_->CreateSampler(&smp, h);
    }

    list_->Close();
    ID3D12CommandList *lists[] = {list_.Get()};
    provider_->GetDirectQueue()->ExecuteCommandLists(1, lists);
    provider_->GetDirectQueue()->Signal(fence_.Get(), ++fence_value_);
    WaitFence(fence_value_);
    staging->Unmap(0, nullptr);
    return true;
  }

  void Renderer::Shutdown()
  {
    if (fence_)
      WaitFence(fence_value_);

    if (event_)
      CloseHandle(event_);

    event_ = nullptr;
    pipelines_.clear();
    list_.Reset();

    for (auto &f : frames_)
    {
      f.allocator.Reset();
      f.pages.clear();
    }

    for (auto &o : outputs_)
      o.Reset();

    placeholder_textures_.clear();
    zero_buffer_.Reset();
    depth_.Reset();
    srv_heap_.Reset();
    sampler_heap_.Reset();
    rtv_heap_.Reset();
    dsv_heap_.Reset();
    root_signature_.Reset();
    fence_.Reset();
  }

  D3D12_CPU_DESCRIPTOR_HANDLE Renderer::Rtv(uint32_t slot) const
  {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(slot) * rtv_size_;
    return h;
  }

  void Renderer::WaitFence(uint64_t value)
  {
    if (!value || fence_->GetCompletedValue() >= value)
      return;

    if (SUCCEEDED(fence_->SetEventOnCompletion(value, event_)))
      WaitForSingleObject(event_, INFINITE);
  }

  Renderer::Upload Renderer::Allocate(size_t size, size_t alignment)
  {
    FrameResources &f = frames_[frame_res_];

    for (;;)
    {
      if (f.page < f.pages.size())
      {
        UploadPage &p = f.pages[f.page];
        const size_t start = (f.offset + alignment - 1) & ~(alignment - 1);

        if (start + size <= p.size)
        {
          f.offset = start + size;
          stats_.upload_bytes += size;
          return {p.cpu + start, p.gpu + start};
        }

        ++f.page;
        f.offset = 0;
        continue;
      }

      UploadPage p;
      p.size = std::max(kUploadPageSize, (size + 0xFFFF) & ~size_t(0xFFFF));
      D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
      D3D12_RESOURCE_DESC bd = {};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width = p.size;
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

      if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p.resource))))
      {
        return {};
      }

      p.resource->Map(0, nullptr, reinterpret_cast<void **>(&p.cpu));
      p.gpu = p.resource->GetGPUVirtualAddress();
      f.pages.push_back(std::move(p));
      f.page = f.pages.size() - 1;
      f.offset = 0;
    }
  }

  // frame
  void Renderer::BeginFrame()
  {
    {
      std::lock_guard lock(slot_mutex_);
      target_ = 0;

      while (int(target_) == latest_ || int(target_) == pinned_)
        ++target_;
    }

    frame_res_ = uint32_t(submitted_ % kSlots);
    FrameResources &f = frames_[frame_res_];
    WaitFence(f.fence_value);
    f.allocator->Reset();
    f.page = 0;
    f.offset = 0;
    list_->Reset(f.allocator.Get(), nullptr);

    auto to_rt = Transition(outputs_[target_].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &to_rt);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(target_);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // Start each frame defined (the game clears explicitly too).
    static constexpr float kBlack[4] = {0, 0, 0, 1};
    list_->ClearRenderTargetView(rtv, kBlack, 0, nullptr);
    list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, last_depth_clear_, 0, 0, nullptr);

    if (capture_requested_ && EnsureCaptureResources())
    {
      capture_requested_ = false;
      capture_active_ = true;
      captured_.clear();
      so_used_ = 0;
      list_->CopyBufferRegion(so_buffer_.Get(), kSoDataBytes, so_zero_.Get(), 0, kSoCounterBytes);
      auto to_so = Transition(so_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
      list_->ResourceBarrier(1, &to_so);

      if (!capture_quiet_)
        REXLOG_INFO("[sst-native] capture: recording frame {}", submitted_ + 1);
    }

    ID3D12DescriptorHeap *heaps[] = {srv_heap_.Get(), sampler_heap_.Get()};
    list_->SetDescriptorHeaps(2, heaps);
    list_->SetGraphicsRootSignature(root_signature_.Get());

    for (UINT i = 0; i < 3; ++i)
      list_->SetGraphicsRootDescriptorTable(kRootTextures2D + i, srv_heap_->GetGPUDescriptorHandleForHeapStart());

    list_->SetGraphicsRootDescriptorTable(kRootSamplers, sampler_heap_->GetGPUDescriptorHandleForHeapStart());
    const D3D12_RECT scissor = {0, 0, LONG(width_), LONG(height_)};
    list_->RSSetScissorRects(1, &scissor);
    recording_ = true;
  }

  void Renderer::EndFrame()
  {
    if (!recording_)
      BeginFrame();

    // --sst_native_check_late_writes: did any draw's index / vertex data change after the draw was issued (the console's GPU would have read it later)?
    for (const LateCheck &lc : late_checks_)
    {
      for (size_t r = 0; r < lc.ranges.size(); ++r)
      {
        const auto &g = lc.ranges[r];

        if (XXH3_64bits(g.ptr, g.size) == g.hash)
          continue;

        ++stats_.late_changed;

        if (g.index)
          ++stats_.late_changed_index;

        if (stats_.late_logged < 40)
        {
          ++stats_.late_logged;
          REXLOG_WARN("[sst-native] late write: {} data of draw (VS {:016X}, prim {}, count {}{}) "
                      "changed after the draw was issued, before the frame ended ({} bytes at "
                      "host {}, {} draws ago)",
                      g.index ? std::string("index") : fmt::format("vertex stream {}", lc.ranges[0].index ? r - 1 : r),
                      lc.vs, lc.prim, lc.count, lc.morph ? ", reads morph output" : "", g.size,
                      fmt::ptr(g.ptr), draw_seq_ - lc.draw);
        }
      }
    }
    stats_.late_checked += late_checks_.size();
    late_checks_.clear();
    auto to_src = Transition(outputs_[target_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(1, &to_src);
    const bool capturing = capture_active_;

    if (capturing)
    {
      D3D12_RESOURCE_BARRIER b[2] = {
          Transition(so_buffer_.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE),
          {}};

      list_->ResourceBarrier(1, b);
      list_->CopyResource(so_readback_.Get(), so_buffer_.Get());
      b[0] = Transition(so_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      list_->ResourceBarrier(1, b);
      capture_active_ = false;
    }

    list_->Close();
    ID3D12CommandQueue *queue = provider_->GetDirectQueue();
    ID3D12CommandList *lists[] = {list_.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence_.Get(), ++fence_value_);
    frames_[frame_res_].fence_value = fence_value_;
    ++submitted_;
    recording_ = false;
    memexport_ranges_.clear();

    if (capturing)
    {
      WaitFence(fence_value_);
      FinishCapture();
      captured_.clear();
    }

    // F9 (or --sst_native_capture_frame) captures the next frame
    capture_quiet_ = REXCVAR_GET(sst_native_capture_frame) == -1;

    if ((GetAsyncKeyState(VK_F9) & 1))
    {
      capture_requested_ = true;
      capture_quiet_ = false;
    }
    else if (capture_quiet_ || (REXCVAR_GET(sst_native_capture_frame) > 0 && submitted_ + 1 == uint64_t(REXCVAR_GET(sst_native_capture_frame))))
    {
      capture_requested_ = true;
    }

    memexport_written_prev_.swap(memexport_written_);
    clip_occurrence_.clear();
    memexport_written_.clear();

    std::lock_guard lock(slot_mutex_);
    latest_ = int(target_);
  }

  ID3D12Resource *Renderer::Acquire(uint32_t *slot, uint32_t *width, uint32_t *height)
  {
    std::lock_guard lock(slot_mutex_);

    if (latest_ < 0)
      return nullptr;

    pinned_ = latest_;
    *slot = uint32_t(latest_);
    *width = width_;
    *height = height_;
    return outputs_[latest_].Get();
  }

  void Renderer::Release(uint32_t slot)
  {
    std::lock_guard lock(slot_mutex_);

    if (pinned_ == int(slot))
      pinned_ = -1;
  }

  // clear
  void Renderer::Clear(const uint8_t *base, uint32_t device, uint32_t flags, uint32_t color, float z, uint32_t stencil)
  {
    ++stats_.clears;
    ++stats_.clear_flags[flags];

    if (!recording_)
      BeginFrame();

    const uint32_t rt0 = be32(base, device + kDevRT0);

    if (stats_.clear_logged++ < 12)
      REXLOG_INFO("[sst-native] clear flags {:X} colour {:08X} z {} stencil {} RT0 {:08X} [{:08X}]", flags, color, z, stencil, rt0, rt0 ? be32(base, rt0 + 0x24) : 0);

    if (rt0)
    {
      bool selected;

      if (REXCVAR_GET(sst_native_rt) == "auto" && main_rts_.count(rt0))
        selected = true;
      else if (REXCVAR_GET(sst_native_rt) == "auto" && other_rts_.count(rt0))
        selected = false;
      else
        selected = RenderTargetSelected(base, device, be32(base, rt0 + 0x24));

      if (!selected)
        return;
    }

    if ((flags & 0xF0) && rt0)
    {
      const float c[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f, float(color & 0xFF) / 255.0f, float((color >> 24) & 0xFF) / 255.0f};
      list_->ClearRenderTargetView(Rtv(target_), c, 0, nullptr);
    }

    D3D12_CLEAR_FLAGS ds = D3D12_CLEAR_FLAGS(0);

    if (flags & 0x3)
      ds |= D3D12_CLEAR_FLAG_DEPTH;

    if (flags & 0x4)
      ds |= D3D12_CLEAR_FLAG_STENCIL;

    if (ds & D3D12_CLEAR_FLAG_DEPTH)
      last_depth_clear_ = std::clamp(z, 0.0f, 1.0f);

    if (ds)
      list_->ClearDepthStencilView(dsv_heap_->GetCPUDescriptorHandleForHeapStart(), ds, std::clamp(z, 0.0f, 1.0f), UINT8(stencil), 0, nullptr);
  }

  // pipelines
  ID3D12PipelineState *Renderer::GetPipeline(const PipelineKey &key, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout, const ShaderVariant *vs, const ShaderVariant *ps)
  {
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));

    if (auto it = pipelines_.find(hash); it != pipelines_.end())
      return it->second.Get();

    if (failed_pipelines_.count(hash))
      return nullptr;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = root_signature_.Get();
    d.VS = {vs->dxil.data(), vs->dxil.size()};

    if (ps)
      d.PS = {ps->dxil.data(), ps->dxil.size()};

    const uint32_t b = key.blend;
    D3D12_RENDER_TARGET_BLEND_DESC &rt = d.BlendState.RenderTarget[0];
    rt.SrcBlend = BlendFactor(b & 0x1F, false);
    rt.BlendOp = BlendOp((b >> 5) & 7);
    rt.DestBlend = BlendFactor((b >> 8) & 0x1F, false);
    rt.SrcBlendAlpha = BlendFactor((b >> 16) & 0x1F, true);
    rt.BlendOpAlpha = BlendOp((b >> 21) & 7);
    rt.DestBlendAlpha = BlendFactor((b >> 24) & 0x1F, true);
    rt.BlendEnable = !(rt.SrcBlend == D3D12_BLEND_ONE && rt.DestBlend == D3D12_BLEND_ZERO && rt.BlendOp == D3D12_BLEND_OP_ADD && rt.SrcBlendAlpha == D3D12_BLEND_ONE && rt.DestBlendAlpha == D3D12_BLEND_ZERO && rt.BlendOpAlpha == D3D12_BLEND_OP_ADD);
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = key.color_mask;
    d.SampleMask = UINT_MAX;

    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = key.cull == 1 ? D3D12_CULL_MODE_FRONT : key.cull == 2 ? D3D12_CULL_MODE_BACK
                                                                                       : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = key.front_ccw;
    d.RasterizerState.DepthClipEnable = TRUE;

    d.DepthStencilState.DepthEnable = key.depth_enable;
    d.DepthStencilState.DepthWriteMask = key.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(key.depth_func + 1); // xenos 0-7

    d.InputLayout = {layout.data(), UINT(layout.size())};
    d.IBStripCutValue = key.strip_cut == 1 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF : key.strip_cut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                                                                            : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology_type);
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = D3D12Presenter::kGuestOutputFormat;
    d.DSVFormat = kDepthFormat;
    d.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));

    if (FAILED(hr))
    {
      failed_pipelines_.insert(hash);
      ++stats_.pipelines_failed;
      if (stats_.pipelines_failed <= 10)
      {
        std::string inputs;
        for (const auto &e : layout)
          inputs += fmt::format(" {}{}:{}@{}+{}", e.SemanticName, e.SemanticIndex, int(e.Format), e.InputSlot, e.AlignedByteOffset);

        REXLOG_WARN("[sst-native] pipeline creation failed ({:08X}): blend {:08X} topology {} "
                    "inputs{}",
                    uint32_t(hr), key.blend, key.topology_type, inputs);
      }

      return nullptr;
    }

    ++stats_.pipelines_created;
    return pipelines_.emplace(hash, std::move(pso)).first->second.Get();
  }

  // draw
  ShaderEntry *Renderer::ResolveShader(const uint8_t *base, uint32_t object, bool vertex)
  {
    if (!object)
      return nullptr;

    if (!PlausibleAddress(object))
      return shaders_->FindObject(object);

    // The object's copy of the container's virtual part identifies the shader. Objects are reused by address: if a new shader was created at the same address by a path the Create hooks don't see, the cached binding would run the old shader (seen as GPU / CPU disagreements and random mesh spikes).
    const uint32_t vcopy = object + (vertex ? 0x368 : 0x28);
    const uint32_t vsize = be32(base, vcopy + 4);
    const uint64_t vhash = vsize && vsize < 0x10000 ? XXH3_64bits(GuestPtr(base, vcopy), vsize) : 0;

    // The microcode can change under an unchanged header (variants sharing their constant / input layout): when the object's microcode pointer changes, compare the live microcode with the bound entry's (skipping the vertex fetches D3D patches at bind).
    const uint32_t ucode_ptr = be32(base, object + (vertex ? 0x20 : 0x18));

    auto microcode_matches = [&](const ShaderEntry *e) -> bool
    {
      const std::vector<uint8_t> &ct = e->container;

      if (ct.size() < 0x20)
        return false;

      auto ct32 = [&](size_t at) -> uint32_t
      {
        return at + 4 <= ct.size() ? (uint32_t(ct[at]) << 24) | (uint32_t(ct[at + 1]) << 16) | (uint32_t(ct[at + 2]) << 8) | ct[at + 3] : 0;
      };

      const uint32_t vsz = ct32(4);
      const uint32_t shader_off = ct32(0x18);
      const uint32_t phys_off = ct32(shader_off), bytes = ct32(size_t(shader_off) + 4);
      const uint32_t live = ToVirtual(ucode_ptr);

      if (!PlausibleAddress(live) || !bytes || size_t(vsz) + phys_off + bytes > ct.size())
        return false;

      const uint8_t *a = ct.data() + vsz + phys_off;
      const uint8_t *b = GuestPtr(base, live + (vertex ? e->ucode_offset : phys_off));
      std::vector<bool> skip(bytes / 12 + 1, false);

      for (const auto &in : e->vertex_inputs)
        if (in.address < skip.size())
          skip[in.address] = true;

      for (uint32_t i = 0; i * 12 + 12 <= bytes; ++i)
        if (!skip[i] && std::memcmp(a + 12 * i, b + 12 * i, 12) != 0)
          return false;

      return true;
    };

    if (ShaderEntry *e = shaders_->FindObject(object))
    {
      auto it = object_vhash_.find(object);

      if (it == object_vhash_.end())
      {
        object_vhash_.emplace(object, vhash); // first sighting of this binding
        object_ucode_[object] = ucode_ptr;
        return e;
      }

      if (it->second == vhash)
      {
        uint32_t &known = object_ucode_[object];

        if (known == ucode_ptr)
          return e;

        known = ucode_ptr;

        if (microcode_matches(e))
          return e;

        ++stats_.shader_rebinds;

        if (stats_.shader_rebinds_logged++ < 20)
          REXLOG_WARN("[sst-native] {} shader object {:08X}: same header but different microcode "
                      "(was {:016X}); rebuilding it from the object",
                      vertex ? "vertex" : "pixel", object, e->hash);
      }
      else
      {
        // Re-created through a hooked Create call: the binding is already new
        if (vsize <= e->container.size() && XXH3_64bits(e->container.data(), vsize) == vhash)
        {
          it->second = vhash;
          object_ucode_[object] = ucode_ptr;
          return e;
        }

        ++stats_.shader_rebinds;

        if (stats_.shader_rebinds_logged++ < 20)
          REXLOG_WARN("[sst-native] {} shader object {:08X} now holds a different shader (was "
                      "{:016X}); rebuilding it from the object",
                      vertex ? "vertex" : "pixel", object, e->hash);

        it->second = vhash;
        object_ucode_[object] = ucode_ptr;
      }
    }
    else
    {
      object_vhash_[object] = vhash;
      object_ucode_[object] = ucode_ptr;
    }

    // Object layout (CreateVertexShader / CreatePixelShader): a copy of the container's virtual part at +0x368 (VS) / +0x28 (PS), and the microcode (physical part) pointer at +0x20 (VS) / +0x18 (PS)
    const uint32_t copy = object + (vertex ? 0x368 : 0x28);
    const uint32_t flags = be32(base, copy);
    const uint32_t virtual_size = be32(base, copy + 4), physical_size = be32(base, copy + 8);
    const uint32_t ucode = ToVirtual(be32(base, object + (vertex ? 0x20 : 0x18)));

    if ((flags & 0xFFFFFF00u) != 0x102A1100u || bool(flags & 1) != vertex || !virtual_size || virtual_size + physical_size > (1u << 20) || !PlausibleAddress(ucode))
    {
      if (LogOnce(0xA000000000000000ull | object))
        REXLOG_WARN("[sst-native] {} shader object {:08X} not recognised (flags {:08X})", vertex ? "vertex" : "pixel", object, flags);

      return nullptr;
    }

    std::vector<uint8_t> container(size_t(virtual_size) + physical_size);
    std::memcpy(container.data(), GuestPtr(base, copy), virtual_size);
    std::memcpy(container.data() + virtual_size, GuestPtr(base, ucode), physical_size);
    ShaderEntry *e = shaders_->Register(container.data(), uint32_t(container.size()));
    shaders_->BindObject(object, e);
    ++stats_.shaders_from_objects;
    return e;
  }

  // Everything the CPU interpreter needs to run the bound vertex shader: the live microcode (vertex fetches patched at bind) and the VS state from the device mirror plus the shader's own literal constants. Returns why it failed, or nullptr.
  const char *Renderer::LoadVsState(const uint8_t *base, uint32_t dev, uint32_t vs_obj, const ShaderEntry *vs_entry, std::vector<uint32_t> &ucode, float *constants, uint32_t bools[8], uint32_t fetch[192], uint32_t loops[32], uint32_t *literals_out)
  {
    const std::vector<uint8_t> &ct = vs_entry->container;

    auto ct32 = [&](size_t at) -> uint32_t
    {
      return at + 4 <= ct.size() ? (uint32_t(ct[at]) << 24) | (uint32_t(ct[at + 1]) << 16) | (uint32_t(ct[at + 2]) << 8) | ct[at + 3] : 0;
    };

    const uint32_t ucode_bytes = ct32(size_t(ct32(0x18)) + 4);
    const uint32_t live_ucode = ToVirtual(be32(base, vs_obj + 0x20));

    if (!PlausibleAddress(live_ucode) || !ucode_bytes || ucode_bytes > 0x10000 || (ucode_bytes % 12))
      return "microcode not found";

    const uint32_t dwords = ucode_bytes / 4;
    ucode.resize(dwords);
    const uint8_t *uc = GuestPtr(base, live_ucode + vs_entry->ucode_offset);

    for (uint32_t i = 0; i < dwords; ++i)
    {
      uint32_t v;
      std::memcpy(&v, uc + 4 * i, 4);
      ucode[i] = __builtin_bswap32(v);
    }

    // Constants from the device mirror (big-endian)
    {
      const uint8_t *p = GuestPtr(base, dev + kDevVSConstants);

      for (uint32_t i = 0; i < 1024; ++i)
      {
        uint32_t v;
        std::memcpy(&v, p + 4 * i, 4);
        v = __builtin_bswap32(v);
        std::memcpy(&constants[i], &v, 4);
      }

      for (uint32_t i = 0; i < 8; ++i)
        bools[i] = be32(base, dev + kDevBools + 4 * i);

      for (uint32_t i = 0; i < 192; ++i)
        fetch[i] = be32(base, dev + kDevFetchConstants + 4 * i);
    }

    std::memset(loops, 0, 32 * sizeof(uint32_t));
    uint32_t literals = 0;
    const uint32_t virtual_size = ct32(4);
    const uint32_t table = ct32(0x14);

    if (table && table + 0x14 <= ct.size())
    {
      size_t at = size_t(table) + 0x14; // DefinitionTable::definitions

      for (; at + 8 <= ct.size() && ct32(at); at += 8)
      {
        const uint32_t w = ct32(at);
        const uint32_t reg = w >> 16, count = w & 0xFFFF;
        const size_t src = size_t(virtual_size) + ct32(at + 4);

        for (uint32_t i = 0; i < (count + 3) / 4; ++i)
        {
          if (reg + i >= 256 || src + 16 * (i + 1) > ct.size())
            break;

          for (uint32_t j = 0; j < 4; ++j)
          {
            const uint32_t v = ct32(src + 16 * i + 4 * j);
            std::memcpy(&constants[4 * (reg + i) + j], &v, 4);
          }

          ++literals;
        }
      }

      at += 4; // terminator

      for (; at + 4 <= ct.size() && ct32(at);)
      {
        const uint32_t w = ct32(at);
        const uint32_t reg = w >> 16, count = w & 0xFFFF;

        for (uint32_t i = 0; i < count && at + 8 + 4 * i <= ct.size(); ++i)
        {
          const uint32_t loop = (reg >= 8992 ? (reg - 8992) / 4 : 32) + i;

          if (loop < 32)
            loops[loop] = ct32(at + 4 + 4 * i);
        }

        at += 4 + 4 * size_t(count);
      }
    }

    if (literals_out)
      *literals_out = literals;

    return nullptr;
  }

  // Debug (--sst_native_check_vs): runs the draw's vertex shader on the CPU from the original microcode and looks for vertices far outside the view.
  // Keys (shader, first vertex buffer) that are only *sometimes* extreme point at spikes; they are reported every status interval, the first few draws in detail.
  void Renderer::CheckVsPositions(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, uint32_t first_vertex, uint32_t vertex_count, uint32_t stream0, bool reads_morph, const float *host, uint32_t host_inputs)
  {
    if (call.up_data || vertex_count > 65536)
      return;

    uint32_t bools[8], fetch[192], loops[32];
    check_constants_.resize(1024);

    if (LoadVsState(base, call.device, vs_obj, vs_entry, check_ucode_, check_constants_.data(), bools, fetch, loops, nullptr))
      return;

    check_indices_.resize(vertex_count);

    for (uint32_t i = 0; i < vertex_count; ++i)
      check_indices_[i] = first_vertex + i;

    check_positions_.resize(size_t(vertex_count) * 4);
    VsConstants vc;
    vc.float_constants = check_constants_.data();
    vc.bool_constants = bools;
    vc.loop_constants = loops;
    vc.fetch_constants = fetch;
    MemexportStats ignored;

    if (!memexport_->RunPositions(check_ucode_.data(), uint32_t(check_ucode_.size()), vc, check_indices_.data(), vertex_count, check_positions_.data(), &ignored))
      return;

    uint32_t extreme = 0, first = 0, exported = 0;

    for (uint32_t v = 0; v < vertex_count; ++v)
    {
      const float *p = &check_positions_[4 * size_t(v)];

      if (std::isnan(p[0]) && std::isnan(p[1]) && std::isnan(p[2]) && std::isnan(p[3]))
        continue;

      ++exported;
      const bool bad = !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || !std::isfinite(p[3]) || std::max({std::fabs(p[0]), std::fabs(p[1]), std::fabs(p[2])}) > 1.0e6f || (p[3] > 1.0e-3f && (std::fabs(p[0]) > 64.0f * p[3] || std::fabs(p[1]) > 64.0f * p[3]));

      if (bad && !extreme++)
        first = v;
    }

    if (!exported)
      return;

    // On-screen jumps: compare with this mesh's previous frame. A vertex that moves far on screen while the rest of the mesh barely moves is a spike (camera cuts move everything, so they don't count).
    {
      const uint64_t base_key = vs_entry->hash ^ (uint64_t(stream0) << 24) ^ (uint64_t(first_vertex) << 52) ^ vertex_count;
      const uint64_t hkey = base_key ^ (uint64_t(clip_occurrence_[base_key]++) << 40);
      ClipHistory &h = clip_history_[hkey];
      std::vector<float> ndc(size_t(vertex_count) * 2, std::nanf(""));

      for (uint32_t v = 0; v < vertex_count; ++v)
      {
        const float *p = &check_positions_[4 * size_t(v)];

        if (std::isfinite(p[3]) && p[3] > 1e-3f)
        {
          ndc[2 * v] = p[0] / p[3];
          ndc[2 * v + 1] = p[1] / p[3];
        }
      }

      if (h.ndc.size() == ndc.size() && submitted_ - h.frame == 1)
      {
        std::vector<float> moves;
        moves.reserve(vertex_count);

        for (uint32_t v = 0; v < vertex_count; ++v)
        {
          const float dx = ndc[2 * v] - h.ndc[2 * v], dy = ndc[2 * v + 1] - h.ndc[2 * v + 1];

          if (std::isfinite(dx) && std::isfinite(dy))
            moves.push_back(std::sqrt(dx * dx + dy * dy));
        }

        if (!moves.empty())
        {
          ++stats_.check_jump_checks;
          std::vector<float> sorted = moves;
          std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
          const float median = sorted[sorted.size() / 2];
          const float limit = std::max(0.5f, 20.0f * median);
          uint32_t jumps = 0, first_jump = 0;

          for (uint32_t v = 0; v < vertex_count; ++v)
          {
            const float dx = ndc[2 * v] - h.ndc[2 * v], dy = ndc[2 * v + 1] - h.ndc[2 * v + 1];

            if (std::isfinite(dx) && std::isfinite(dy) && std::sqrt(dx * dx + dy * dy) > limit)
              if (!jumps++)
                first_jump = v;
          }

          if (jumps)
          {
            ++stats_.check_jump_draws;

            if (stats_.check_jump_logged < 30)
            {
              ++stats_.check_jump_logged;
              const float *p = &check_positions_[4 * size_t(first_jump)];
              std::string inputs;

              for (uint32_t i = 0; i < std::min<uint32_t>(host_inputs, 6); ++i)
              {
                const float *in = host + (size_t(first_jump) * host_inputs + i) * 4;
                uint32_t b[4];
                std::memcpy(b, in, 16);
                inputs += fmt::format(" in{}=({} {} {} {} | {:08X} {:08X} {:08X} {:08X})", i, in[0], in[1], in[2], in[3], b[0], b[1], b[2], b[3]);
              }

              REXLOG_WARN("[sst-native] vertex jump (CPU-run shader): VS {:016X} buffer {:08X}{} "
                          "draw {} vertices from {}: {} vertices jumped (median move {:.4f}); "
                          "vertex {} screen ({:.3f} {:.3f}) -> ({:.3f} {:.3f}), clip ({} {} {} {});{}",
                          vs_entry->hash, stream0, reads_morph ? " (face)" : "", vertex_count,
                          first_vertex, jumps, median, first_vertex + first_jump,
                          h.ndc[2 * first_jump], h.ndc[2 * first_jump + 1], ndc[2 * first_jump],
                          ndc[2 * first_jump + 1], p[0], p[1], p[2], p[3], inputs);
            }
          }
        }
      }

      h.ndc.swap(ndc);
      h.frame = submitted_;

      if (clip_history_.size() > 8192)
        clip_history_.clear();
    }

    const uint64_t key = vs_entry->hash ^ (uint64_t(stream0) << 24);
    CheckEntry &e = check_entries_[key];
    e.vs = vs_entry->hash;
    e.stream = stream0;
    e.prim = call.prim;
    e.morph |= reads_morph;
    ++e.checked;
    ++stats_.check_draws;

    if (!extreme)
      return;

    ++e.flagged;
    ++stats_.check_flagged;

    if (e.flagged < e.checked && stats_.check_logged < 40)
    {
      ++stats_.check_logged;
      const float *p = &check_positions_[4 * size_t(first)];
      std::string inputs;

      for (uint32_t i = 0; i < std::min<uint32_t>(host_inputs, 4); ++i)
      {
        const float *h = host + (size_t(first) * host_inputs + i) * 4;
        uint32_t b[4];
        std::memcpy(b, h, 16);
        inputs += fmt::format(" in{}=({} {} {} {} | {:08X} {:08X} {:08X} {:08X})", i, h[0], h[1], h[2], h[3], b[0], b[1], b[2], b[3]);
      }

      REXLOG_WARN("[sst-native] spike? VS {:016X} prim {} {} draw {} vertices from {} (buffer "
                  "{:08X}{}): {} of {} vertices far outside the view, first vertex {} at clip "
                  "({} {} {} {});{} [key seen {} times, {} flagged]",
                  vs_entry->hash, call.prim, call.indexed ? "indexed" : "auto", vertex_count,
                  first_vertex, stream0, reads_morph ? ", morph output" : "", extreme, exported,
                  first_vertex + first, p[0], p[1], p[2], p[3], inputs, e.checked, e.flagged);
    }
  }

  void Renderer::CheckMorphJumps(const ShaderEntry *vs_entry, const DrawCall &call, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout, const uint8_t *host, uint32_t host_inputs, uint32_t first_vertex, uint32_t vertex_count, uint32_t stream0)
  {
    // Was the morph output changed between the morph and this draw?
    if (auto it = morph_writes_.find(stream0); it != morph_writes_.end() && it->second.hash && it->second.frame == submitted_)
    {
      const uint64_t now = XXH3_64bits(physical_membase_ + stream0, it->second.size);

      if (now != it->second.hash)
      {
        ++stats_.morph_modified;

        if (stats_.morph_modified_logged++ < 10)
          REXLOG_WARN("[sst-native] morph output {:08X} ({} bytes) changed between the morph ({} "
                      "draws ago) and the draw reading it (VS {:016X})",
                      stream0, it->second.size, draw_seq_ - it->second.draw, vs_entry->hash);
      }
    }

    int pos = -1;

    for (const auto &d : layout)
      if (d.InputSlot == 0 && d.SemanticName && std::strcmp(d.SemanticName, "POSITION") == 0 && d.SemanticIndex == 0)
        pos = int(d.AlignedByteOffset / 16);

    if (pos < 0 || !host_inputs)
      return;

    const uint64_t key = vs_entry->hash ^ (uint64_t(stream0) << 24) ^ (uint64_t(first_vertex) << 52) ^ vertex_count;
    MorphHistory &h = morph_history_[key];
    std::vector<float> cur(size_t(vertex_count) * 3);
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (uint32_t v = 0; v < vertex_count; ++v)
    {
      const float *p = reinterpret_cast<const float *>(host) + (size_t(v) * host_inputs + pos) * 4;

      for (int j = 0; j < 3; ++j)
      {
        cur[3 * v + j] = p[j];
        lo[j] = std::min(lo[j], p[j]);
        hi[j] = std::max(hi[j], p[j]);
      }
    }

    const float extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-3f});

    if (h.positions.size() == cur.size() && submitted_ - h.frame <= 1)
    {
      uint32_t jumps = 0, first = 0;
      float worst = 0;

      for (uint32_t v = 0; v < vertex_count; ++v)
      {
        const float dx = cur[3 * v] - h.positions[3 * v], dy = cur[3 * v + 1] - h.positions[3 * v + 1], dz = cur[3 * v + 2] - h.positions[3 * v + 2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (d > 0.25f * h.extent)
        {
          if (!jumps++)
            first = v;

          worst = std::max(worst, d);
        }
      }

      ++stats_.morph_jump_checks;

      if (jumps)
      {
        ++stats_.morph_jump_draws;

        if (stats_.morph_jump_logged < 30)
        {
          ++stats_.morph_jump_logged;
          std::string writes;
          auto it = morph_writes_.find(stream0);

          if (it != morph_writes_.end())
            writes = fmt::format("output last written frame {} ({} frames ago), {} draws ago{}", it->second.frame, submitted_ - it->second.frame, draw_seq_ - it->second.draw, it->second.cached ? " (cached)" : "");
          else
            writes = "output never written by a morph";

          const float *a = &h.positions[3 * first];
          const float *b = &cur[3 * first];

          REXLOG_WARN("[sst-native] morph jump: VS {:016X} buffer {:08X} draw {} vertices from {}: "
                      "{} vertices moved > 25% of the mesh size {:.2f} (worst {:.2f}); vertex {} "
                      "({:.3f} {:.3f} {:.3f}) -> ({:.3f} {:.3f} {:.3f}); previous draw of this "
                      "mesh {} frames / {} draws ago; {}",
                      vs_entry->hash, stream0, vertex_count, first_vertex, jumps, h.extent, worst,
                      first_vertex + first, a[0], a[1], a[2], b[0], b[1], b[2],
                      submitted_ - h.frame, draw_seq_ - h.draw, writes);
        }
      }
    }

    h.positions.swap(cur);
    h.extent = extent;
    h.frame = submitted_;
    h.draw = draw_seq_;
    (void)call;
  }

  // Element indices for shaders that fetch with a computed index: run the bound vertex shader on the CPU over the draw's vertices and record what each full vfetch fetched (fetch_records_, by instruction address)
  bool Renderer::ComputeFetchRecords(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, uint32_t first_vertex, uint32_t vertex_count)
  {
    uint32_t bools[8], fetch[192], loops[32];
    check_constants_.resize(1024);

    if (LoadVsState(base, call.device, vs_obj, vs_entry, check_ucode_, check_constants_.data(), bools, fetch, loops, nullptr))
      return false;

    check_indices_.resize(vertex_count);

    for (uint32_t i = 0; i < vertex_count; ++i)
      check_indices_[i] = first_vertex + i;

    VsConstants vc;
    vc.float_constants = check_constants_.data();
    vc.bool_constants = bools;
    vc.loop_constants = loops;
    vc.fetch_constants = fetch;
    return memexport_->RunFetchIndices(check_ucode_.data(), uint32_t(check_ucode_.size()), vc, check_indices_.data(), vertex_count, &fetch_records_);
  }

  void Renderer::RecordMemexportRanges(const uint8_t *base, uint32_t dev)
  {
    // Stream constants are VS float constants with fixed marker bits (xe_gpu_memexport_stream_t): dword0 bits 30-31 = 1, dword1 = 0x4B000000, dword2 bits 20-31 = 0x4B0, dword3 bits 23-31 = 0x96
    for (uint32_t c = 0; c < 256; ++c)
    {
      const uint32_t a = dev + kDevVSConstants + 16 * c;
      const uint32_t d0 = be32(base, a), d1 = be32(base, a + 4), d2 = be32(base, a + 8), d3 = be32(base, a + 12);

      if ((d0 >> 30) != 1 || d1 != 0x4B000000u || (d2 >> 20) != 0x4B0 || (d3 >> 23) != 0x96)
        continue;

      const uint32_t begin = (d0 & 0x3FFFFFFF) * 4;
      const uint32_t count = d3 & 0x7FFFFF;
      memexport_ranges_.push_back({begin, begin + count * 16}); // <= 16 bytes/element
    }
  }

  // On the console the GPU runs these shaders and writes their eM# exports to memory; the visible mesh draw then fetches those vertices. Here the shader is interpreted on the CPU (memexport.h) with the device's current state, writing guest memory directly before the dependent draw is recorded.
  void Renderer::RunMemexport(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry)
  {
    ++stats_.memexport_draws;
    const uint32_t dev = call.device;

    auto fail = [&](const char *why)
    {
      ++stats_.memexport_failed;
      RecordMemexportRanges(base, dev);

      if (LogOnce(0xC400000000000000ull ^ vs_entry->hash))
        REXLOG_WARN("[sst-native] memexport VS {:016X} not run: {}", vs_entry->hash, why);
    };

    if (!REXCVAR_GET(sst_native_memexport))
    {
      RecordMemexportRanges(base, dev);
      return;
    }

    if (call.up_data)
      return fail("DrawVerticesUP");

    if (call.count > kMaxDrawVertices)
      return fail("too many vertices");

    memexport_constants_.resize(256 * 4);
    uint32_t bools[8], fetch[192], loops[32], literals = 0;

    if (const char *why = LoadVsState(base, dev, vs_obj, vs_entry, memexport_ucode_, memexport_constants_.data(), bools, fetch, loops, &literals))
      return fail(why);

    const uint32_t dwords = uint32_t(memexport_ucode_.size());

    // Vertex indices
    memexport_indices_.resize(call.count);

    if (call.indexed)
    {
      const uint32_t ib = be32(base, dev + kDevIndices);

      if (!ib)
        return fail("no index buffer");

      const uint32_t common = be32(base, ib);
      const bool index32 = (common >> 31) & 1;
      const uint32_t index_endian = (common >> 29) & 3;
      const uint32_t ib_addr = ToVirtual(be32(base, ib + 0x18) & ~3u);

      if (!PlausibleAddress(ib_addr))
        return fail("bad index buffer");

      const uint32_t isz = index32 ? 4 : 2;
      const uint32_t first_byte = call.start * isz;
      const uint32_t aligned_begin = first_byte & ~3u;
      const uint32_t aligned_end = (first_byte + call.count * isz + 3) & ~3u;
      std::vector<uint8_t> raw(aligned_end - aligned_begin);
      SwapVertexData(index_endian, GuestPtr(base, ib_addr + aligned_begin), raw.data(), raw.size());
      const uint8_t *src = raw.data() + (first_byte - aligned_begin);

      for (uint32_t i = 0; i < call.count; ++i)
      {
        uint32_t v = 0;
        std::memcpy(&v, src + isz * i, isz);
        memexport_indices_[i] = (v + call.base_vertex) & 0xFFFFFF;
      }
    }
    else
    {
      for (uint32_t i = 0; i < call.count; ++i)
        memexport_indices_[i] = call.start + i;
    }

    // Output streams (for the cache and the log): stream constants in c0-c255
    struct Stream
    {
      uint32_t begin, size, format;
    };

    Stream streams[4];
    uint32_t stream_count = 0;

    for (uint32_t c = 0; c < 256 && stream_count < 4; ++c)
    {
      uint32_t d[4];
      std::memcpy(d, &memexport_constants_[4 * c], 16);

      if ((d[0] >> 30) != 1 || d[1] != 0x4B000000u || (d[2] >> 20) != 0x4B0 || (d[3] >> 23) != 0x96)
        continue;

      const uint32_t format = (d[2] >> 8) & 0x3F;
      const uint32_t element = MemexportElementSize(format);
      streams[stream_count++] = {(d[0] << 2) & 0x1FFFFFFFu, (d[3] & 0x7FFFFF) * (element ? element : 16), format};
    }

    // Cache: identical shader, constants, indices, input vertex data and an untouched previous output -> nothing to do
    const bool use_cache = REXCVAR_GET(sst_native_memexport_cache) && stream_count == 1 && streams[0].size && uint64_t(streams[0].begin) + streams[0].size <= 0x20000000u;
    uint64_t key = 0, input_hash = 0;

    if (use_cache)
    {
      XXH3_state_t *st = XXH3_createState();
      XXH3_64bits_reset(st);
      XXH3_64bits_update(st, memexport_ucode_.data(), memexport_ucode_.size() * 4);

      // Only the constants the shader reads (the rest of the file holds whatever other shaders left there, which changes between passes)
      uint64_t used[4];

      if (FloatConstantsRead(memexport_ucode_.data(), dwords, used))
      {
        for (uint32_t c = 0; c < 256; ++c)
          if ((used[c >> 6] >> (c & 63)) & 1)
            XXH3_64bits_update(st, &memexport_constants_[4 * c], 16);
      }
      else
      {
        XXH3_64bits_update(st, memexport_constants_.data(), memexport_constants_.size() * 4);
      }

      XXH3_64bits_update(st, bools, sizeof(bools));
      XXH3_64bits_update(st, loops, sizeof(loops));
      XXH3_64bits_update(st, memexport_indices_.data(), memexport_indices_.size() * 4);
      uint32_t slots[16];
      const uint32_t nslots = VertexFetchSlots(memexport_ucode_.data(), dwords, slots, 16);

      for (uint32_t i = 0; i < nslots; ++i)
        XXH3_64bits_update(st, &fetch[2 * slots[i]], 8);

      bool hashed_inputs = true;

      for (uint32_t i = 0; i < nslots; ++i)
      {
        const uint32_t phys = fetch[2 * slots[i]] & 0x1FFFFFFCu;
        const uint32_t size = fetch[2 * slots[i] + 1] & 0x03FFFFFCu;

        if (size > (8u << 20) || uint64_t(phys) + size > 0x20000000u)
        {
          hashed_inputs = false;
          break;
        }

        XXH3_64bits_update(st, physical_membase_ + phys, size);
      }

      input_hash = XXH3_64bits_digest(st);
      XXH3_freeState(st);
      key = vs_entry->hash ^ (uint64_t(streams[0].begin) << 20);

      if (hashed_inputs)
      {
        auto it = memexport_cache_.find(key);

        if (it != memexport_cache_.end() && it->second.input_hash == input_hash && it->second.output_begin == streams[0].begin && it->second.output_size == streams[0].size && it->second.output_hash == XXH3_64bits(physical_membase_ + streams[0].begin, streams[0].size))
        {
          ++stats_.memexport_cached;
          memexport_written_.push_back({streams[0].begin, streams[0].begin + streams[0].size});
          morph_writes_[streams[0].begin] = {submitted_, draw_seq_, true, streams[0].size, it->second.output_hash};
          return;
        }
      }
      else
      {
        key = 0;
      }
    }

    const auto t0 = std::chrono::steady_clock::now();
    VsConstants vc;
    vc.float_constants = memexport_constants_.data();
    vc.bool_constants = bools;
    vc.loop_constants = loops; // shader-defined only (the device mirror isn't known)
    vc.fetch_constants = fetch;
    const uint64_t elements_before = memexport_stats_.elements;

    if (!memexport_->Run(memexport_ucode_.data(), dwords, vc, memexport_indices_.data(), call.count, &memexport_stats_))
      return fail("interpreter refused the shader");

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    stats_.memexport_ms += ms;
    ++stats_.memexport_run;
    const uint64_t written = memexport_stats_.elements - elements_before;

    if (!written)
      return fail("shader wrote nothing");

    // Remember what was written this frame (consumer checks in Draw), and look for implausible output values
    for (uint32_t i = 0; i < stream_count; ++i)
    {
      memexport_written_.push_back({streams[i].begin, streams[i].begin + streams[i].size});
      const bool hashable = uint64_t(streams[i].begin) + streams[i].size <= 0x20000000u;
      morph_writes_[streams[i].begin] = {submitted_, draw_seq_, false, streams[i].size, hashable ? XXH3_64bits(physical_membase_ + streams[i].begin, streams[i].size) : 0};
    }

    if (stream_count == 1 && streams[0].format >= 36 && streams[0].format <= 38 && uint64_t(streams[0].begin) + streams[0].size <= 0x20000000u)
    {
      // (Values read as 8in32-swapped floats, the endian this game uses.
      const uint8_t *out = physical_membase_ + streams[0].begin;
      uint32_t bad = 0, first_bad = UINT32_MAX;
      float first_value = 0;

      for (uint32_t i = 0; i < streams[0].size / 4; ++i)
      {
        uint32_t v;
        std::memcpy(&v, out + 4 * i, 4);
        v = __builtin_bswap32(v);
        float f;
        std::memcpy(&f, &v, 4);

        if (!std::isfinite(f) || std::fabs(f) > 1.0e5f)
        {
          if (!bad++)
            first_bad = i, first_value = f;
        }
      }

      if (bad)
      {
        ++stats_.memexport_bad_outputs;

        if (stats_.memexport_bad_logged++ < 8)
        {
          std::string weights;

          for (uint32_t c = 1; c <= 4; ++c)
            weights += fmt::format(" ({:.3f} {:.3f} {:.3f} {:.3f})", memexport_constants_[4 * c], memexport_constants_[4 * c + 1], memexport_constants_[4 * c + 2], memexport_constants_[4 * c + 3]);

          REXLOG_WARN("[sst-native] memexport VS {:016X} at {:08X}: {} implausible values of {} "
                      "(first: float {} = {}); vertices {} start {}; c1-c4{}",
                      vs_entry->hash, streams[0].begin, bad, streams[0].size / 4, first_bad,
                      first_value, call.count, call.start, weights);
        }
      }
    }

    if (key)
    {
      MemexportCacheEntry &e = memexport_cache_[key];
      e.input_hash = input_hash;
      e.output_begin = streams[0].begin;
      e.output_size = streams[0].size;
      e.output_hash = XXH3_64bits(physical_membase_ + streams[0].begin, streams[0].size);
    }

    if (LogOnce(0xC500000000000000ull ^ vs_entry->hash) || LogOnce(0xC600000000000000ull ^ (uint64_t(streams[0].begin) << 8)))
      REXLOG_INFO("[sst-native] memexport VS {:016X}: {} vertices ({}) -> {} elements, format {} "
                  "at {:08X} ({} streams, {} literal constants, c255 {} {} {} {}); {:.3f} ms;{}",
                  vs_entry->hash, call.count, call.indexed ? "indexed" : "auto", written,
                  memexport_stats_.last_format, memexport_stats_.last_base, stream_count, literals,
                  memexport_constants_[1020], memexport_constants_[1021], memexport_constants_[1022],
                  memexport_constants_[1023], ms,
                  DescribeMicrocode(memexport_ucode_.data(), dwords));
  }

  void Renderer::Draw(const uint8_t *base, const DrawCall &call)
  {
    ++stats_.draws;
    ++draw_seq_;
    ++stats_.prims[call.prim];

    if (!call.count)
      return;

    const uint32_t dev = call.device;

    // Memory-export (morph) passes: no rendering, they write vertices a later draw reads. Run them whatever the render target.
    const uint32_t vs_obj = be32(base, dev + kDevVS), ps_obj = be32(base, dev + kDevPS);
    ShaderEntry *vs_entry = ResolveShader(base, vs_obj, true);

    if (vs_entry && vs_entry->memexport)
    {
      RunMemexport(base, call, vs_obj, vs_entry);
      return;
    }

    // Render target
    const uint32_t rt0 = be32(base, dev + kDevRT0);

    if (!rt0)
    {
      ++stats_.skip_rt;
      return;
    }

    const uint32_t rt_size = be32(base, rt0 + 0x24);
    ++stats_.render_targets[(uint64_t(rt0) << 32) | rt_size];

    if (LogOnce(0x9000000000000000ull | rt0))
      REXLOG_INFO("[sst-native] render target {:08X}: size word {:08X} (~{}x{}), depth {:08X}, "
                  "viewport scale {} {} {} offset {} {} {}, VTE {:08X}",
                  rt0, rt_size, (rt_size >> 18) + 1, ((rt_size >> 3) & 0x7FFF) + 1,
                  be32(base, dev + kDevDepth), RegF(base, dev, PA_CL_VPORT_XSCALE),
                  RegF(base, dev, PA_CL_VPORT_YSCALE), RegF(base, dev, PA_CL_VPORT_ZSCALE),
                  RegF(base, dev, PA_CL_VPORT_XOFFSET), RegF(base, dev, PA_CL_VPORT_YOFFSET),
                  RegF(base, dev, PA_CL_VPORT_ZOFFSET), Reg(base, dev, PA_CL_VTE_CNTL));

    const bool rt_selected = RenderTargetSelected(base, dev, rt_size);

    if (REXCVAR_GET(sst_native_rt) == "auto")
      (rt_selected ? main_rts_ : other_rts_).insert(rt0);

    if (!rt_selected)
    {
      ++stats_.skip_rt;
      return;
    }

    // Primitive type
    D3D_PRIMITIVE_TOPOLOGY topology;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
    bool convert = false; // TRIFAN / QUADLIST -> indexed triangle list

    switch (call.prim)
    {
    case 1:
      topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      break;

    case 2:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;

    case 3:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;

    case 4:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;

    case 6:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;

    case 5: // TRIFAN

    case 0xD: // QUADLIST
      if (!REXCVAR_GET(sst_native_quads))
      {
        ++stats_.skip_prim;
        return;
      }

      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      convert = true;
      break;

    default: // RECTLIST (8) and others: later
      ++stats_.skip_prim;
      return;
    }

    // Shaders
    ShaderEntry *ps_entry = ps_obj ? ResolveShader(base, ps_obj, false) : nullptr;

    if (!vs_entry || vs_entry->memexport || (ps_obj && !ps_entry))
    {
      ++stats_.skip_shader;

      if (stats_.skip_shader_logged++ < 8)
        REXLOG_INFO("[sst-native] skipped draw: VS {:08X} ({}) PS {:08X} ({}) prim {} count {}", vs_obj, !vs_entry ? "unknown" : vs_entry->memexport ? "memexport"
                                                                                                                                                     : "ok",
                    ps_obj, !ps_obj ? "none" : ps_entry ? "ok"
                                                        : "unknown",
                    call.prim, call.count);

      return;
    }

    // Vertex inputs, from the vertex shader's *live* fetch instructions: the Xbox D3D patches format / offset / stride / fetch constant / swizzle into the microcode when it binds the declaration (draw-time flush), so this is exactly what the GPU would fetch. Each input is decoded on the
    // CPU (DecodeElement) into 4 x 32 bits; swizzle changes the patch made relative to the original instruction become the component order.
    const uint32_t live_ucode = ToVirtual(be32(base, vs_obj + 0x20));

    if (!PlausibleAddress(live_ucode) || vs_entry->container.size() < 16)
    {
      ++stats_.skip_decl;
      return;
    }

    const uint32_t virtual_size = (uint32_t(vs_entry->container[4]) << 24) | (uint32_t(vs_entry->container[5]) << 16) | (uint32_t(vs_entry->container[6]) << 8) | vs_entry->container[7];

    struct FetchStream
    {
      uint32_t slot;   // fetch constant slot (2-dword units)
      uint32_t stride; // bytes
    };

    FetchStream fetch_streams[kMaxStreams];
    uint32_t fetch_stream_count = 0;

    auto stream_index = [&](uint32_t slot, uint32_t stride) -> int
    {
      for (uint32_t i = 0; i < fetch_stream_count; ++i)
        if (fetch_streams[i].slot == slot && fetch_streams[i].stride == stride)
          return int(i);

      if (fetch_stream_count == kMaxStreams)
        return -1;

      fetch_streams[fetch_stream_count] = {slot, stride};
      return int(fetch_stream_count++);
    };

    std::vector<sst::xenos::VertexInput> inputs = vs_entry->vertex_inputs;

    std::sort(inputs.begin(), inputs.end(), [](const auto &x, const auto &y)
              { return x.address < y.address; });

    struct HostElement
    {
      FetchElement fetch;
      int stream;          // -1: reads zeros
      uint32_t fetch_addr; // instruction index of the controlling full vfetch
    };

    // Shaders whose full vfetches don't fetch with the plain vertex index (r0.x)
    bool computed_fetch = false;
    uint32_t full_fetch_addr = 0;
    std::vector<HostElement> host_elements;
    std::string capture_detail; // F9 capture: inputs of this draw
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    layout.reserve(inputs.size());
    host_elements.reserve(inputs.size());
    const uint32_t vs_spec = 0; // R11G11B10 normals are decoded on the CPU
    uint64_t layout_hash_input[64];
    uint32_t layout_words = 0;
    int full_stream = -1; // the last full vfetch (mini fetches share its constant)

    // live fetch instructions
    struct LiveFetch
    {
      uint32_t addr, l0, l1, l2; // live (patched)
      uint32_t o0, o1;           // original (container)
    };

    std::vector<LiveFetch> live_fetches;
    live_fetches.reserve(inputs.size());

    auto container_dword = [&](size_t at) -> uint32_t
    {
      if (at + 4 > vs_entry->container.size())
        return 0;

      const uint8_t *p = vs_entry->container.data() + at;
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    };

    for (const auto &in : inputs)
    {
      const uint32_t at = live_ucode + vs_entry->ucode_offset + 12u * in.address;
      const size_t orig = size_t(virtual_size) + vs_entry->ucode_offset + 12u * in.address;
      live_fetches.push_back({in.address, be32(base, at), be32(base, at + 4), be32(base, at + 8), container_dword(orig), container_dword(orig + 4)});
    }

    auto written_mask = [](uint32_t swizzle_word)
    {
      uint32_t m = 0;

      for (int i = 0; i < 4; ++i)
        if (((swizzle_word >> (3 * i)) & 7) != 7)
          m |= 1u << i;

      return m;
    };

    bool remapped = false;

    for (const auto &in : inputs)
    {
      D3D12_INPUT_ELEMENT_DESC d = {};
      d.SemanticName = UsageSemantic(in.usage);
      d.SemanticIndex = in.usage_index;
      d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

      if (!d.SemanticName)
        continue;

      const LiveFetch *self = nullptr;

      for (const LiveFetch &lf : live_fetches)
        if (lf.addr == in.address)
          self = &lf;

      if (!self)
        continue;

      const uint32_t o1 = self->o1 ? self->o1 : self->l1; // original swizzle

      // Live instruction writing the original destination register
      const LiveFetch *match = self;

      if (self->o0 && (self->o0 & 0x1F) == 0)
      {
        const uint32_t dst = (self->o0 >> 12) & 0x3F, want = written_mask(o1);
        const bool self_ok = (self->l0 & 0x1F) == 0 && ((self->l0 >> 12) & 0x3F) == dst && (written_mask(self->l1) & want);

        if (!self_ok)
        {
          const LiveFetch *best = nullptr;
          int best_score = -1;

          for (const LiveFetch &lf : live_fetches)
          {
            if ((lf.l0 & 0x1F) != 0 || ((lf.l0 >> 12) & 0x3F) != dst)
              continue;

            const int score = __builtin_popcount(written_mask(lf.l1) & want);

            if (score > best_score)
              best = &lf, best_score = score;
          }

          if (best && best != self)
          {
            match = best;
            remapped = true;
          }
        }
      }

      const uint32_t l0 = match->l0, l1 = match->l1, l2 = match->l2;

      // Its full fetch (a mini fetch uses the last full fetch before it).
      const LiveFetch *full = nullptr;

      if ((l0 & 0x1F) == 0 && !((l1 >> 30) & 1))
      {
        full = match;
      }
      else
      {
        for (const LiveFetch &lf : live_fetches)
          if (lf.addr < match->addr && (lf.l0 & 0x1F) == 0 && !((lf.l1 >> 30) & 1) && (!full || lf.addr > full->addr))
            full = &lf;
      }

      if (full)
      {
        const uint32_t slot = ((full->l0 >> 20) & 0x1F) * 3 + ((full->l0 >> 25) & 3);
        full_stream = stream_index(slot, (full->l2 & 0xFF) * 4);
        full_fetch_addr = full->addr;

        // Source register bits 5-10 (+ relative bit 11), source swizzle 30-31.
        if (((full->l0 >> 5) & 0x7F) != 0 || ((full->l0 >> 30) & 3) != 0)
          computed_fetch = true;
      }
      else
      {
        full_stream = -1;
      }

      HostElement h;
      h.stream = -1;
      h.fetch_addr = 0;

      if ((l0 & 0x1F) == 0)
      {
        const int32_t offset = int32_t(l2 << 1) >> 9; // bits 8-30, signed, in dwords
        FetchElement &f = h.fetch;
        f.format = (l1 >> 16) & 0x3F;
        f.is_signed = (l1 >> 12) & 1;
        f.normalized = !((l1 >> 13) & 1);
        f.no_zero = (l1 >> 14) & 1;
        f.exp_adjust = int32_t(l1 << 2) >> 26;
        bool conflict = false;
        uint8_t set = 0;

        for (int i = 0; i < 4; ++i)
        {
          const uint32_t o = (o1 >> (3 * i)) & 7, l = (l1 >> (3 * i)) & 7;

          if (o >= 4)
            continue; // the shader doesn't read the input for this destination

          const uint8_t hw = uint8_t(l >= 6 ? 4 : l);

          if ((set >> o) & 1)
            conflict |= f.sel[o] != hw;

          f.sel[o] = hw;
          set |= uint8_t(1u << o);
        }

        if (conflict && LogOnce(0xE100000000000000ull | (uint64_t(o1) << 24) | (l1 & 0xFFF)))
          REXLOG_WARN("[sst-native] {}{}: conflicting swizzles (original {:03X} patched {:03X})", d.SemanticName, d.SemanticIndex, o1 & 0xFFF, l1 & 0xFFF);

        f.out = !UsageIsUint(in.usage) ? FetchElement::Out::kFloat : in.usage == 2 ? FetchElement::Out::kUint
                                                                                   : FetchElement::Out::kFloatBits;

        if (DeclFormatSize(f.format) && offset >= 0 && full_stream >= 0)
        {
          f.offset = uint32_t(offset) * 4;
          h.stream = full_stream;
          h.fetch_addr = full_fetch_addr;
        }

        if (capture_active_)
          capture_detail += fmt::format(" {}{}[live {:08X} {:08X} {:08X} orig {:08X} fmt {} sel {}{}{}{} "
                                        "off {}]",
                                        d.SemanticName, d.SemanticIndex, l0, l1, l2, o1, f.format,
                                        f.sel[0], f.sel[1], f.sel[2], f.sel[3], offset * 4);

        if (LogOnce(0xE200000000000000ull | (uint64_t(in.usage) << 40) | (uint64_t(l1 & 0x3FFFFFFF))))
          REXLOG_INFO("[sst-native] vertex input {}{}: format {} {} {} exp {} swizzle {:03X}->{:03X} "
                      "(sel {}{}{}{}) offset {} stride {}",
                      d.SemanticName, d.SemanticIndex, f.format, f.is_signed ? "signed" : "unsigned",
                      f.normalized ? "norm" : "int", f.exp_adjust, o1 & 0xFFF, l1 & 0xFFF,
                      f.sel[0], f.sel[1], f.sel[2], f.sel[3], offset * 4, (l2 & 0xFF) * 4);
      }

      if (h.stream < 0 && LogOnce(0xE000000000000000ull | (uint64_t(vs_entry->hash & 0xFFFFFFFF) << 16) | in.address))
        REXLOG_WARN("[sst-native] VS {:016X} input {}{} at {}: unusable fetch {:08X} {:08X} {:08X}", vs_entry->hash, d.SemanticName, d.SemanticIndex, in.address, l0, l1, l2);

      d.Format = UsageIsUint(in.usage) ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;

      if (h.stream >= 0)
      {
        d.InputSlot = 0;
        d.AlignedByteOffset = UINT(16 * host_elements.size());
        host_elements.push_back(h);
      }
      else
      {
        d.InputSlot = kZeroInputSlot;
        d.AlignedByteOffset = 0;
      }

      layout.push_back(d);

      if (layout_words + 1 < std::size(layout_hash_input))
        layout_hash_input[layout_words++] = (uint64_t(in.usage) << 56) | (uint64_t(in.usage_index) << 48) | (uint64_t(d.Format) << 40) | (uint64_t(d.InputSlot) << 32) | d.AlignedByteOffset;
    }

    if (remapped)
    {
      ++stats_.fetch_remapped;

      if (LogOnce(0xCC00000000000000ull ^ vs_entry->hash))
        REXLOG_INFO("[sst-native] VS {:016X}: the bound declaration moved inputs to other fetch "
                    "slots; inputs matched to the live fetches by destination register",
                    vs_entry->hash);
    }

    // Render state from the device's register mirror
    const uint32_t depthcontrol = Reg(base, dev, RB_DEPTHCONTROL);
    const uint32_t colorcontrol = Reg(base, dev, RB_COLORCONTROL);
    const uint32_t mode = Reg(base, dev, PA_SU_SC_MODE_CNTL);
    const bool has_depth = be32(base, dev + kDevDepth) != 0;

    uint32_t ps_spec = 0;
    const bool alpha_test = (colorcontrol >> 3) & 1;
    const uint32_t alpha_func = colorcontrol & 7;

    if (alpha_test && alpha_func == 0)
    {
      ++stats_.skip_cull;
      return;
    }

    if (alpha_test && alpha_func != 7)
      ps_spec |= sst::xenos::kSpecAlphaTest;

    if ((mode & 3) == 3)
    {
      ++stats_.skip_cull;
      return;
    }

    // Shader variants (compiled on worker threads; skip until ready)
    const ShaderVariant *vs = shaders_->GetVariant(vs_entry, vs_spec);
    const ShaderVariant *ps = ps_entry ? shaders_->GetVariant(ps_entry, ps_spec) : nullptr;
    const bool debug_shading = REXCVAR_GET(sst_native_debug_shading) && debug_ps_;

    if (!vs || (ps_entry && !ps))
    {
      ++stats_.skip_pending;
      return;
    }

    // Pipeline
    PipelineKey key;
    std::memset(&key, 0, sizeof(key));

    if (debug_shading && ps)
      ps = debug_ps_.get();

    key.vs = vs;
    key.ps = ps;
    key.layout_hash = XXH3_64bits(layout_hash_input, layout_words * sizeof(uint64_t));
    key.blend = Reg(base, dev, RB_BLENDCONTROL0);

    if (REXCVAR_GET(sst_native_skip_blended) && ps && (key.blend & 0xFFFF) != 0x0001 && (Reg(base, dev, RB_COLOR_MASK) & 0xF))
    {
      ++stats_.skip_cull;
      return;
    }

    key.color_mask = ps ? uint8_t(Reg(base, dev, RB_COLOR_MASK) & 0xF) : 0;

    if (LogOnce(0xD000000000000000ull | (uint64_t(key.color_mask) << 32) | key.blend))
      REXLOG_INFO("[sst-native] new state: colour mask {:X} blend {:08X} depth {:08X} colour "
                  "control {:08X} mode {:08X}",
                  key.color_mask, key.blend, depthcontrol, colorcontrol, mode);

    key.depth_enable = has_depth && ((depthcontrol >> 1) & 1);
    key.depth_write = key.depth_enable && ((depthcontrol >> 2) & 1);
    key.depth_func = key.depth_enable ? uint8_t((depthcontrol >> 4) & 7) : 7;
    key.cull = uint8_t(mode & 3);
    key.front_ccw = ((mode >> 2) & 1) ? 0 : 1;
    key.topology_type = uint8_t(topology_type);

    // Indices, decoded with the index buffer's own swap mode: DrawIndexedVertices puts (Common >> 29) & 3 into the index DMA's swap field (0 none, 1 8in16, 2 8in32, 3 16in32), so 16-bit indices are not always 8in16
    std::vector<uint32_t> converted; // TRIFAN / QUADLIST
    std::vector<uint32_t> indices;   // little-endian, as the GPU reads them
    const uint8_t *ib_src = nullptr; // guest index data (late-write check)
    size_t ib_src_bytes = 0;
    bool index32 = false;
    uint32_t vmin = 0, vmax = 0;

    if (call.indexed)
    {
      const uint32_t ib = be32(base, dev + kDevIndices);

      if (!ib || call.count > kMaxDrawIndices)
      {
        ++stats_.skip_data;
        return;
      }

      const uint32_t common = be32(base, ib);
      index32 = (common >> 31) & 1;
      const uint32_t index_endian = (common >> 29) & 3;
      const uint32_t ib_addr = ToVirtual(be32(base, ib + 0x18) & ~3u);

      if (!PlausibleAddress(ib_addr))
      {
        ++stats_.skip_data;
        return;
      }

      if (LogOnce(0xC100000000000000ull | (index32 ? 4 : 0) | index_endian))
        REXLOG_INFO("[sst-native] index buffer: {}-bit, swap mode {} (Common {:08X})", index32 ? 32 : 16, index_endian, common);

      // Swap whole dwords around the referenced range, then read little-endian
      const uint32_t isz = index32 ? 4 : 2;
      const uint32_t first_byte = call.start * isz;
      const uint32_t aligned_begin = first_byte & ~3u;
      const uint32_t aligned_end = (first_byte + call.count * isz + 3) & ~3u;
      std::vector<uint8_t> raw(aligned_end - aligned_begin);
      ib_src = GuestPtr(base, ib_addr + aligned_begin);
      ib_src_bytes = raw.size();
      SwapVertexData(index_endian, GuestPtr(base, ib_addr + aligned_begin), raw.data(), raw.size());
      const uint8_t *src = raw.data() + (first_byte - aligned_begin);
      indices.resize(call.count);

      for (uint32_t i = 0; i < call.count; ++i)
      {
        if (index32)
        {
          std::memcpy(&indices[i], src + 4 * i, 4);
        }
        else
        {
          uint16_t v;
          std::memcpy(&v, src + 2 * i, 2);
          indices[i] = v;
        }
      }

      const uint32_t restart = index32 ? 0xFFFFFFFFu : 0xFFFFu;
      const bool strip = call.prim == 6 || call.prim == 3;
      vmin = UINT32_MAX;
      vmax = 0;

      for (uint32_t v : indices)
      {
        if (strip && v == restart)
          continue;

        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
      }

      if (vmin > vmax || vmax - vmin >= kMaxDrawVertices)
      {
        ++stats_.skip_data;
        return;
      }

      if (strip)
        key.strip_cut = index32 ? 2 : 1;
    }
    else
    {
      vmin = 0; // relative to call.start
      vmax = call.count - 1;

      if (call.count > kMaxDrawVertices)
      {
        ++stats_.skip_data;
        return;
      }
    }

    ID3D12PipelineState *pso = GetPipeline(key, layout, vs, ps);

    if (!pso)
    {
      ++stats_.skip_pipeline;
      return;
    }

    if (!recording_)
      BeginFrame();

    // Vertex data: resolve each fetch constant, then decode the referenced vertex range into one host stream (16 bytes per input)
    const uint32_t first_vertex = call.indexed ? call.base_vertex + vmin : call.start;
    const uint32_t vertex_count = vmax - vmin + 1;

    struct ResolvedStream
    {
      const uint8_t *src;
      uint32_t endian;
      uint32_t size; // bytes
    };

    ResolvedStream resolved[kMaxStreams] = {};
    bool reads_morph = false;

    for (uint32_t i = 0; i < fetch_stream_count; ++i)
    {
      const FetchStream &fs = fetch_streams[i];
      uint32_t size;

      if (call.up_data && fs.slot == 95)
      {
        // DrawVerticesUP: stream 0 data comes from the caller's buffer
        if (!PlausibleAddress(call.up_data))
        {
          ++stats_.skip_data;
          return;
        }

        resolved[i] = {GuestPtr(base, call.up_data), 2, 0};
        size = call.up_stride * (call.start + call.count);
      }
      else
      {
        const uint32_t dw0 = be32(base, dev + kDevFetchConstants + 8 * fs.slot);
        const uint32_t dw1 = be32(base, dev + kDevFetchConstants + 8 * fs.slot + 4);
        const uint32_t phys = dw0 & 0x1FFFFFFCu;
        size = dw1 & 0x03FFFFFCu;

        if ((dw0 & 3) != 3 || !phys)
        {
          ++stats_.skip_data;

          if (LogOnce(0xF000000000000000ull | fs.slot))
            REXLOG_WARN("[sst-native] fetch constant {} is not a vertex buffer: {:08X} {:08X}", fs.slot, dw0, dw1);

          return;
        }
        for (const auto &[mb, me] : memexport_ranges_)
        {
          if (phys < me && phys + size > mb)
          {
            ++stats_.skip_memexport;
            return;
          }
        }

        // Diagnostics for draws reading morph output: is it this frame's, and does the draw stay inside what the morph wrote?
        {
          const uint64_t read_begin = uint64_t(phys) + uint64_t(first_vertex) * fs.stride;
          const uint64_t read_end = uint64_t(phys) + (uint64_t(first_vertex) + vertex_count) * fs.stride;
          bool overlaps_now = false, inside = false;

          for (const auto &[mb, me] : memexport_written_)
          {
            if (read_begin < me && read_end > mb)
            {
              overlaps_now = true;
              inside |= read_begin >= mb && read_end <= me;
            }
          }

          if (overlaps_now)
          {
            reads_morph = true;
            ++stats_.morph_consumers;

            if (!inside)
            {
              ++stats_.morph_consumer_overrun;

              if (LogOnce(0xC700000000000000ull ^ (uint64_t(phys) << 16) ^ read_end))
                REXLOG_WARN("[sst-native] draw reads past morph output: fetch {:08X} stride {} "
                            "vertices {}+{} (bytes {:08X}-{:08X})",
                            phys, fs.stride, first_vertex, vertex_count, uint32_t(read_begin),
                            uint32_t(read_end));
            }
          }
          else
          {
            for (const auto &[mb, me] : memexport_written_prev_)
            {
              if (read_begin < me && read_end > mb)
              {
                ++stats_.morph_consumer_stale;

                if (LogOnce(0xC800000000000000ull ^ (uint64_t(phys) << 16)))
                  REXLOG_WARN("[sst-native] draw reads morph output written last frame, not "
                              "this one (yet): fetch {:08X} stride {} vertices {}+{}",
                              phys, fs.stride, first_vertex, vertex_count);

                break;
              }
            }
          }
        }

        resolved[i] = {physical_membase_ + phys, dw1 & 3, 0};

        if (LogOnce(0xC200000000000000ull | (dw1 & 3)))
          REXLOG_INFO("[sst-native] vertex buffer swap mode {} (fetch {:08X} {:08X})", dw1 & 3, dw0, dw1);
      }

      resolved[i].size = size;
      const uint64_t end = (uint64_t(first_vertex) + vertex_count) * fs.stride;

      if (!fs.stride || (end > size && !computed_fetch))
      {
        ++stats_.skip_data;

        if (LogOnce(0xB000000000000000ull | fs.slot))
          REXLOG_WARN("[sst-native] draw reads past vertex buffer (fetch {} size {}, vertices {}+{} "
                      "stride {})",
                      fs.slot, size, first_vertex, vertex_count, fs.stride);

        return;
      }
    }

    if (!host_elements.empty())
    {
      const uint32_t host_stride = uint32_t(16 * host_elements.size());
      const Upload up = Allocate(size_t(vertex_count) * host_stride, 16);

      if (!up.cpu)
      {
        ++stats_.skip_data;
        return;
      }

      uint32_t *out = reinterpret_cast<uint32_t *>(up.cpu);

      if (computed_fetch && !call.up_data && ComputeFetchRecords(base, call, vs_obj, vs_entry, first_vertex, vertex_count))
      {
        ++stats_.computed_fetch_draws;

        for (uint32_t v = 0; v < vertex_count; ++v)
        {
          for (const HostElement &h : host_elements)
          {
            const FetchStream &fs = fetch_streams[h.stream];
            const ResolvedStream &rs = resolved[h.stream];
            auto it = fetch_records_.find(h.fetch_addr);
            const int32_t record = it != fetch_records_.end() && v < it->second.size() ? it->second[v] : -1;
            const uint64_t at = uint64_t(record) * fs.stride;

            if (record >= 0 && at + h.fetch.offset + DeclFormatSize(h.fetch.format) <= rs.size)
            {
              DecodeElement(h.fetch, rs.endian, rs.src + at, out);
            }
            else
            {
              out[0] = out[1] = out[2] = out[3] = 0;
            }

            out += 4;
          }
        }

        if (LogOnce(0xCB00000000000000ull ^ vs_entry->hash))
          REXLOG_INFO("[sst-native] VS {:016X} fetches with a computed index (e.g. one record per "
                      "particle quad); element indices taken from running it on the CPU",
                      vs_entry->hash);
      }
      else
      {
        for (uint32_t v = 0; v < vertex_count; ++v)
        {
          for (const HostElement &h : host_elements)
          {
            const FetchStream &fs = fetch_streams[h.stream];
            const ResolvedStream &rs = resolved[h.stream];
            DecodeElement(h.fetch, rs.endian, rs.src + size_t(first_vertex + v) * fs.stride, out);
            out += 4;
          }
        }
      }

      if (reads_morph)
        CheckMorphJumps(vs_entry, call, layout, up.cpu, uint32_t(host_elements.size()), first_vertex, vertex_count, fetch_stream_count ? uint32_t(resolved[0].src - physical_membase_) : 0);

      if (reads_morph)
      {
        const float *f = reinterpret_cast<const float *>(up.cpu);
        const size_t n = size_t(vertex_count) * host_elements.size();
        uint32_t bad = 0;
        size_t first = 0;

        for (size_t i = 0; i < n; ++i)
        {
          if (host_elements[i % host_elements.size()].fetch.out != FetchElement::Out::kFloat)
            continue;

          for (uint32_t j = 0; j < 4; ++j)
            if (!std::isfinite(f[4 * i + j]) || std::fabs(f[4 * i + j]) > 1.0e5f)
            {
              if (!bad++)
                first = 4 * i + j;
            }
        }

        if (bad)
        {
          ++stats_.morph_consumer_bad;

          if (stats_.morph_consumer_bad_logged++ < 8)
            REXLOG_WARN("[sst-native] morph consumer VS {:016X}: {} implausible input values "
                        "(first at vertex {} input {} = {}), vertices {}+{}",
                        vs_entry->hash, bad, first / (4 * host_elements.size()),
                        (first / 4) % host_elements.size(), f[first], first_vertex, vertex_count);
        }
      }

      if (REXCVAR_GET(sst_native_check_late_writes))
      {
        LateCheck lc;
        lc.vs = vs_entry->hash;
        lc.prim = call.prim;
        lc.count = call.count;
        lc.morph = reads_morph;
        lc.draw = draw_seq_;

        if (ib_src)
          lc.ranges.push_back({ib_src, ib_src_bytes, XXH3_64bits(ib_src, ib_src_bytes), true});

        for (uint32_t i = 0; i < fetch_stream_count; ++i)
        {
          const uint8_t *p = resolved[i].src + size_t(first_vertex) * fetch_streams[i].stride;
          const size_t n = size_t(vertex_count) * fetch_streams[i].stride;
          lc.ranges.push_back({p, n, XXH3_64bits(p, n), false});
        }

        late_checks_.push_back(std::move(lc));
      }

      if (REXCVAR_GET(sst_native_check_vs_all) || (REXCVAR_GET(sst_native_check_vs) && reads_morph))
        CheckVsPositions(base, call, vs_obj, vs_entry, first_vertex, vertex_count, fetch_stream_count ? uint32_t(resolved[0].src - physical_membase_) : 0, reads_morph, reinterpret_cast<const float *>(up.cpu), uint32_t(host_elements.size()));

      // Sample of what the first few indexed 3D draws feed the GPU, to check positions / indices / matrices by eye
      if (call.indexed && stats_.sample_logged < 6 && !host_elements.empty())
      {
        ++stats_.sample_logged;
        const float *f = reinterpret_cast<const float *>(up.cpu);
        std::string idx;

        for (uint32_t i = 0; i < std::min<uint32_t>(9, call.count); ++i)
          idx += fmt::format(" {}", indices[i]);

        std::string verts;

        for (uint32_t v = 0; v < std::min<uint32_t>(3, vertex_count); ++v)
        {
          const float *p = f + size_t(v) * host_elements.size() * 4;
          verts += fmt::format(" ({:.3f} {:.3f} {:.3f} {:.3f})", p[0], p[1], p[2], p[3]);
        }

        float c[16];

        for (int i = 0; i < 16; ++i)
        {
          const uint32_t w = be32(base, dev + kDevVSConstants + 4 * i);
          std::memcpy(&c[i], &w, 4);
        }

        REXLOG_INFO("[sst-native] sample draw: VS {:016X} prim {} base {} start {} count {} "
                    "range {}-{}; indices{}; first input of first vertices{}; c0-c3 "
                    "[{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} "
                    "{:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}]",
                    vs_entry->hash, call.prim, call.base_vertex, call.start, call.count, vmin, vmax,
                    idx, verts, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10],
                    c[11], c[12], c[13], c[14], c[15]);
      }

      capture_host_ = capture_active_ ? reinterpret_cast<const float *>(up.cpu) : nullptr;
      const D3D12_VERTEX_BUFFER_VIEW vbv = {up.gpu, vertex_count * host_stride, host_stride};
      list_->IASetVertexBuffers(0, 1, &vbv);
    }

    const D3D12_VERTEX_BUFFER_VIEW zero_vbv = {zero_buffer_->GetGPUVirtualAddress(), 256, 0};
    list_->IASetVertexBuffers(kZeroInputSlot, 1, &zero_vbv);

    // Index buffer (swapped / converted)
    uint32_t draw_count = call.count;
    bool draw_indexed = call.indexed;

    if (convert)
    {
      // Source indices relative to first_vertex
      auto source = [&](uint32_t i) -> uint32_t
      {
        return call.indexed ? indices[i] - vmin : i;
      };

      if (call.prim == 5)
      {
        for (uint32_t i = 2; i < call.count; ++i)
          converted.insert(converted.end(), {source(0), source(i - 1), source(i)});
      }
      else
      {
        for (uint32_t q = 0; q + 3 < call.count; q += 4)
          converted.insert(converted.end(), {source(q), source(q + 1), source(q + 2), source(q), source(q + 2), source(q + 3)});
      }

      if (converted.empty())
        return;

      const Upload up = Allocate(converted.size() * 4, 16);

      if (!up.cpu)
        return;

      std::memcpy(up.cpu, converted.data(), converted.size() * 4);
      const D3D12_INDEX_BUFFER_VIEW ibv = {up.gpu, UINT(converted.size() * 4), DXGI_FORMAT_R32_UINT};
      list_->IASetIndexBuffer(&ibv);
      draw_count = uint32_t(converted.size());
      draw_indexed = true;
    }
    else if (call.indexed)
    {
      const size_t bytes = size_t(call.count) * (index32 ? 4 : 2);
      const Upload up = Allocate(bytes, 16);

      if (!up.cpu)
        return;

      if (index32)
      {
        std::memcpy(up.cpu, indices.data(), bytes);
      }
      else
      {
        for (uint32_t i = 0; i < call.count; ++i)
        {
          const uint16_t v = uint16_t(indices[i]);
          std::memcpy(up.cpu + 2 * i, &v, 2);
        }
      }

      const D3D12_INDEX_BUFFER_VIEW ibv = {up.gpu, UINT(bytes), index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
      list_->IASetIndexBuffer(&ibv);
    }

    float cap_half_x = 0, cap_half_y = 0; // (F9 capture)

    // Constants: device mirror (big-endian float4 x 256 per stage) + shared
    const Upload vsc = Allocate(4096), psc = Allocate(4096), shared = Allocate(512);

    if (!vsc.cpu || !psc.cpu || !shared.cpu)
      return;

    SwapVertexData(2, GuestPtr(base, dev + kDevVSConstants), vsc.cpu, 4096);
    SwapVertexData(2, GuestPtr(base, dev + kDevPSConstants), psc.cpu, 4096);
    {
      uint32_t sc[72] = {};

      for (int i = 0; i < 16; ++i)
      {
        sc[i] = kPlaceholder2D;
        sc[16 + i] = kPlaceholder3D;
        sc[32 + i] = kPlaceholderCube;
        sc[48 + i] = 0; // sampler
      }

      const uint32_t vs_bools = be32(base, dev + kDevBools);
      const uint32_t ps_bools = be32(base, dev + kDevBools + 16);
      sc[64] = (vs_bools & 0xFFFF) | (ps_bools << 16);
      sc[65] = 0; // g_SwappedTexcoords: the stream swap already matches Xenos order
      float half_x = 0, half_y = 0;
      float alpha_ref = RegF(base, dev, RB_ALPHA_REF);
      std::memcpy(&sc[68], &alpha_ref, 4);

      // g_VertexIndexBase: SV_VertexID + this = the Xenos vertex index (r0.x).
      sc[69] = first_vertex;

      // Viewport (needed for the half-pixel offset too).
      const uint32_t vte = Reg(base, dev, PA_CL_VTE_CNTL);

      float xs = RegF(base, dev, PA_CL_VPORT_XSCALE), xo = RegF(base, dev, PA_CL_VPORT_XOFFSET);
      float ys = RegF(base, dev, PA_CL_VPORT_YSCALE), yo = RegF(base, dev, PA_CL_VPORT_YOFFSET);
      float zs = RegF(base, dev, PA_CL_VPORT_ZSCALE), zo = RegF(base, dev, PA_CL_VPORT_ZOFFSET);
      D3D12_VIEWPORT vp = {0, 0, float(width_), float(height_), 0, 1};

      if ((vte & 1) && xs != 0 && ys != 0 && std::isfinite(xs) && std::isfinite(ys))
      {
        vp.TopLeftX = xo - std::fabs(xs);
        vp.Width = 2 * std::fabs(xs);
        vp.TopLeftY = yo - std::fabs(ys);
        vp.Height = 2 * std::fabs(ys);
        vp.MinDepth = std::clamp(zo, 0.0f, 1.0f);
        vp.MaxDepth = std::clamp(zo + zs, 0.0f, 1.0f);

        if (ys > 0 && LogOnce(0xC000000000000001ull))
          REXLOG_WARN("[sst-native] positive viewport y scale ({}): image may be flipped", ys);
      }
      else if (LogOnce(0xC000000000000000ull | vte))
      {
        REXLOG_INFO("[sst-native] viewport transform off (PA_CL_VTE_CNTL {:08X}), using full "
                    "target",
                    vte);
      }

      if (REXCVAR_GET(sst_native_half_pixel) && vp.Width > 0 && vp.Height > 0)
      {
        half_x = 1.0f / vp.Width;
        half_y = -1.0f / vp.Height;
      }

      std::memcpy(&sc[66], &half_x, 4);
      std::memcpy(&sc[67], &half_y, 4);
      cap_half_x = half_x;
      cap_half_y = half_y;
      std::memcpy(shared.cpu, sc, sizeof(sc));
      list_->RSSetViewports(1, &vp);
    }

    const float blend_factor[4] = {RegF(base, dev, RB_BLEND_RED), RegF(base, dev, RB_BLEND_RED + 1), RegF(base, dev, RB_BLEND_RED + 2), RegF(base, dev, RB_BLEND_RED + 3)};
    list_->OMSetBlendFactor(blend_factor);
    list_->SetPipelineState(pso);
    list_->SetGraphicsRootConstantBufferView(kRootVSConstants, vsc.gpu);
    list_->SetGraphicsRootConstantBufferView(kRootVSConstantsRaw, vsc.gpu);
    list_->SetGraphicsRootConstantBufferView(kRootPSConstants, psc.gpu);
    list_->SetGraphicsRootConstantBufferView(kRootPSConstantsRaw, psc.gpu);
    list_->SetGraphicsRootConstantBufferView(kRootShared, shared.gpu);
    list_->IASetPrimitiveTopology(topology);

    if (draw_indexed)
      list_->DrawIndexedInstanced(draw_count, 1, 0, convert ? 0 : -int32_t(vmin), 0);
    else
      list_->DrawInstanced(draw_count, 1, 0, 0);

    ++stats_.drawn;

    if (!ps)
      ++stats_.depth_only;

    if (capture_active_ && !host_elements.empty() && !call.up_data)
    {
      if (const float *hv = capture_host_; hv)
      {
        capture_detail += " | host vertex 0:";

        for (size_t i = 0; i < host_elements.size(); ++i)
          capture_detail += fmt::format(" ({} {} {} {})", hv[4 * i], hv[4 * i + 1], hv[4 * i + 2], hv[4 * i + 3]);
      }

      if (fetch_stream_count)
      {
        const uint8_t *r = resolved[0].src + size_t(first_vertex) * fetch_streams[0].stride;
        capture_detail += " | stream 0 raw:";

        for (int i = 0; i < 4; ++i)
          capture_detail += fmt::format(" {:08X}", be32raw(r + 4 * i));
      }

      capture_detail_ = std::move(capture_detail);
    }

    if (capture_active_ && !host_elements.empty() && !call.up_data)
      CaptureDraw(base, call, vs_obj, vs_entry, vs, layout, key.layout_hash, first_vertex, vertex_count, fetch_stream_count ? uint32_t(resolved[0].src - physical_membase_) : 0, reads_morph, cap_half_x, cap_half_y);
  }

  bool Renderer::EnsureCaptureResources()
  {
    if (so_buffer_)
      return true;

    const size_t total = kSoDataBytes + kSoCounterBytes;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT}, up = {D3D12_HEAP_TYPE_UPLOAD}, rb = {D3D12_HEAP_TYPE_READBACK};

    if (FAILED(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&so_buffer_))) || FAILED(device_->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&so_readback_))))
    {
      so_buffer_.Reset();
      return false;
    }

    bd.Width = kSoCounterBytes;

    if (FAILED(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&so_zero_))))
    {
      so_buffer_.Reset();
      return false;
    }

    void *p = nullptr;
    so_zero_->Map(0, nullptr, &p);
    std::memset(p, 0, kSoCounterBytes);
    so_zero_->Unmap(0, nullptr);
    return true;
  }

  ID3D12PipelineState *Renderer::GetSoPipeline(const ShaderVariant *vs, uint64_t layout_hash, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout)
  {
    const uint64_t key = XXH3_64bits(&vs, sizeof(vs)) ^ layout_hash;

    if (auto it = so_pipelines_.find(key); it != so_pipelines_.end())
      return it->second.Get();

    static const D3D12_SO_DECLARATION_ENTRY kEntry = {0, "SV_Position", 0, 0, 4, 0};
    static const UINT kStride = 16;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = root_signature_.Get();
    d.VS = {vs->dxil.data(), vs->dxil.size()};
    d.StreamOutput.pSODeclaration = &kEntry;
    d.StreamOutput.NumEntries = 1;
    d.StreamOutput.pBufferStrides = &kStride;
    d.StreamOutput.NumStrides = 1;
    d.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = FALSE;
    d.InputLayout = {layout.data(), UINT(layout.size())};
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = D3D12Presenter::kGuestOutputFormat;
    d.DSVFormat = kDepthFormat;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));

    if (FAILED(hr))
    {
      if (LogOnce(0xCA00000000000000ull ^ key))
        REXLOG_WARN("[sst-native] capture: stream-output pipeline failed ({:08X})", uint32_t(hr));

      so_pipelines_.emplace(key, nullptr);
      return nullptr;
    }

    return so_pipelines_.emplace(key, std::move(pso)).first->second.Get();
  }

  void Renderer::CaptureDraw(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, const ShaderVariant *vs, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout, uint64_t layout_hash, uint32_t first_vertex, uint32_t vertex_count, uint32_t stream0, bool reads_morph, float half_x, float half_y)
  {
    if (captured_.size() >= kSoMaxDraws || so_used_ + size_t(vertex_count) * 16 > kSoDataBytes)
      return;

    ID3D12PipelineState *pso = GetSoPipeline(vs, layout_hash, layout);

    if (!pso)
      return;

    CapturedDraw c;
    c.vs_hash = vs_entry->hash;
    c.prim = call.prim;
    c.count = call.count;
    c.indexed = call.indexed;
    c.first_vertex = first_vertex;
    c.vertex_count = vertex_count;
    c.stream0 = stream0;
    c.morph = reads_morph;
    c.half_x = half_x;
    c.half_y = half_y;
    c.so_offset = so_used_;
    c.seq = draw_seq_;
    c.detail = std::move(capture_detail_);

    // CPU: the original microcode over the same vertices
    uint32_t bools[8], fetch[192], loops[32];
    check_constants_.resize(1024);

    if (!LoadVsState(base, call.device, vs_obj, vs_entry, check_ucode_, check_constants_.data(), bools, fetch, loops, nullptr))
    {
      std::vector<uint32_t> idx(vertex_count);

      for (uint32_t i = 0; i < vertex_count; ++i)
        idx[i] = first_vertex + i;

      c.cpu.resize(size_t(vertex_count) * 4);
      VsConstants vc;
      vc.float_constants = check_constants_.data();
      vc.bool_constants = bools;
      vc.loop_constants = loops;
      vc.fetch_constants = fetch;
      MemexportStats ignored;

      if (!memexport_->RunPositions(check_ucode_.data(), uint32_t(check_ucode_.size()), vc, idx.data(), vertex_count, c.cpu.data(), &ignored))
        c.cpu.clear();
    }

    // GPU: same pipeline inputs, drawn as points into the stream-output buffer
    const D3D12_GPU_VIRTUAL_ADDRESS so = so_buffer_->GetGPUVirtualAddress();
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW view = {so + c.so_offset, UINT64(vertex_count) * 16, so + kSoDataBytes + captured_.size() * 16};

    list_->SOSetTargets(0, 1, &view);
    list_->SetPipelineState(pso);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    list_->DrawInstanced(vertex_count, 1, 0, 0);
    list_->SOSetTargets(0, 0, nullptr);
    so_used_ += size_t(vertex_count) * 16;
    captured_.push_back(std::move(c));
  }

  void Renderer::FinishCapture()
  {
    const uint8_t *data = nullptr;
    const D3D12_RANGE range = {0, kSoDataBytes + kSoCounterBytes};

    if (FAILED(so_readback_->Map(0, &range, reinterpret_cast<void **>(const_cast<uint8_t **>(&data)))))
    {
      REXLOG_WARN("[sst-native] capture: readback failed");
      return;
    }

    uint32_t mismatched = 0, gpu_nan = 0, compared = 0;
    uint64_t vertices = 0;

    struct Reach
    {
      float extent;
      size_t index;
    };

    std::vector<Reach> reach;

    for (size_t d = 0; d < captured_.size(); ++d)
    {
      const CapturedDraw &c = captured_[d];
      uint32_t filled;
      std::memcpy(&filled, data + kSoDataBytes + d * 16, 4);
      const uint32_t n = std::min<uint32_t>(filled / 16, c.vertex_count);
      vertices += n;
      const float *g = reinterpret_cast<const float *>(data + c.so_offset);
      uint32_t bad = 0, first_bad = 0, nan = 0;
      float worst = 0, extent = 0;

      for (uint32_t v = 0; v < n; ++v)
      {
        const float *gp = g + 4 * size_t(v);
        const bool gfinite = std::isfinite(gp[0]) && std::isfinite(gp[1]) && std::isfinite(gp[2]) && std::isfinite(gp[3]);

        if (!gfinite)
          ++nan;

        if (gfinite && gp[3] > 1e-3f)
          extent = std::max({extent, std::fabs(gp[0] / gp[3]), std::fabs(gp[1] / gp[3])});

        if (c.cpu.empty())
          continue;

        const float *cp = &c.cpu[4 * size_t(v)];

        if (std::isnan(cp[0]) && std::isnan(cp[3]))
          continue; // no position export on the CPU

        // The translated shader adds the half-pixel offset to xy
        const float cx = cp[0] + c.half_x * cp[3], cy = cp[1] + c.half_y * cp[3];
        const bool cfinite = std::isfinite(cx) && std::isfinite(cy) && std::isfinite(cp[2]) && std::isfinite(cp[3]);
        float diff;

        if (gfinite != cfinite)
        {
          diff = INFINITY;
        }
        else if (!gfinite)
        {
          diff = 0;
        }
        else
        {
          const float scale = std::max({1.0f, std::fabs(cp[3]), std::fabs(cx), std::fabs(cy)});
          diff = std::max({std::fabs(gp[0] - cx), std::fabs(gp[1] - cy), std::fabs(gp[2] - cp[2]), std::fabs(gp[3] - cp[3])}) /
                 scale;
        }

        if (diff > 1e-3f)
        {
          if (!bad++)
            first_bad = v;

          worst = std::max(worst, diff);
        }
      }

      if (!c.cpu.empty())
        ++compared;

      if (nan)
        ++gpu_nan;

      reach.push_back({extent, d});

      if (bad)
      {
        ++mismatched;

        if (mismatched <= 40 && capture_mismatch_logged_++ < 200)
        {
          const float *gp = g + 4 * size_t(first_bad);
          const float *cp = &c.cpu[4 * size_t(first_bad)];
          REXLOG_WARN("[sst-native] capture MISMATCH: draw #{} VS {:016X} prim {} {} {} vertices "
                      "from {} (buffer {:08X}{}): {} of {} vertices differ (worst {:.4g}); "
                      "vertex {}: GPU ({} {} {} {}) CPU ({} {} {} {});{}",
                      c.seq, c.vs_hash, c.prim, c.indexed ? "indexed" : "auto", c.vertex_count,
                      c.first_vertex, c.stream0, c.morph ? ", face" : "", bad, n, worst,
                      c.first_vertex + first_bad, gp[0], gp[1], gp[2], gp[3],
                      cp[0] + c.half_x * cp[3], cp[1] + c.half_y * cp[3], cp[2], cp[3], c.detail);
        }
      }
    }

    if (capture_quiet_)
    {
      if (mismatched && capture_frames_logged_++ < 300)
        REXLOG_WARN("[sst-native] capture: frame {}: {} of {} draws differ between GPU and CPU", submitted_, mismatched, compared);

      const D3D12_RANGE none = {0, 0};
      so_readback_->Unmap(0, &none);
      return;
    }

    std::sort(reach.begin(), reach.end(), [](const Reach &a, const Reach &b)
              { return a.extent > b.extent; });
    REXLOG_INFO("[sst-native] capture: {} draws, {} vertices from the GPU; {} compared with the "
                "CPU, {} differ; {} draws with non-finite GPU positions. Draws reaching farthest "
                "outside the view (|x/w| or |y/w|):",
                captured_.size(), vertices, compared, mismatched, gpu_nan);

    for (size_t i = 0; i < reach.size() && i < 25; ++i)
    {
      const CapturedDraw &c = captured_[reach[i].index];
      REXLOG_INFO("[sst-native]   {:.1f}: draw #{} VS {:016X} prim {} {} vertices from {} "
                  "(buffer {:08X}{})",
                  reach[i].extent, c.seq, c.vs_hash, c.prim, c.vertex_count, c.first_vertex,
                  c.stream0, c.morph ? ", face" : "");
    }

    const D3D12_RANGE none = {0, 0};
    so_readback_->Unmap(0, &none);
  }

  // stats
  void Renderer::LogStats(uint64_t frame)
  {
    auto &s = stats_;
    REXLOG_INFO("[sst-native] frame {}: {} draws, {} drawn ({} depth-only); skipped: shader {} "
                "pending {} fetch {} prim {} rt {} pipeline {} data {} cull {} memexport-target {}; {} clears; "
                "pipelines {} ({} failed); shaders rebuilt from objects {}; upload {:.1f} MB",
                frame, s.draws, s.drawn, s.depth_only, s.skip_shader, s.skip_pending, s.skip_decl,
                s.skip_prim, s.skip_rt, s.skip_pipeline, s.skip_data, s.skip_cull, s.skip_memexport,
                s.clears,
                s.pipelines_created, s.pipelines_failed, s.shaders_from_objects,
                double(s.upload_bytes) / (1 << 20));
    std::string prims;

    for (auto &[p, n] : s.prims)
      prims += fmt::format(" {:X}:{}", p, n);

    std::string clears;

    for (auto &[f, n] : s.clear_flags)
      clears += fmt::format(" {:X}:{}", f, n);

    REXLOG_INFO("[sst-native]   prims{} | clear flags{}", prims, clears);
    const MemexportStats &m = memexport_stats_;

    REXLOG_INFO("[sst-native]   memexport: {} draws, {} run ({:.2f} ms total, {:.3f} ms/frame), {} "
                "cached, {} not run; totals: {} vertices, {} elements, bad address {}, bad format "
                "{}, out of range {}, fetch out of bounds {}, malformed {}, per-vertex {}",
                s.memexport_draws, s.memexport_run, s.memexport_ms,
                s.memexport_ms / double(std::max<uint64_t>(1, frame - last_stats_frame_)),
                s.memexport_cached, s.memexport_failed, m.vertices, m.elements, m.bad_address,
                m.bad_format, m.out_of_range, m.fetch_oob, m.malformed, m.per_vertex_draws);

    if (s.fetch_remapped)
      REXLOG_INFO("[sst-native]   draws whose inputs sat in other fetch slots (matched by "
                  "register): {}",
                  s.fetch_remapped);

    if (s.computed_fetch_draws)
      REXLOG_INFO("[sst-native]   draws fetching with a computed index (CPU-resolved): {}", s.computed_fetch_draws);

    if (s.shader_rebinds)
      REXLOG_INFO("[sst-native]   shader objects reused for a different shader: {}", s.shader_rebinds);

    REXLOG_INFO("[sst-native]   morph checks: {} draws with implausible output; consumers {} "
                "(reading past the output {}, stale {}, implausible inputs {}); jump checks {}, "
                "draws with jumping vertices {}; output changed before use {}",
                s.memexport_bad_outputs, s.morph_consumers, s.morph_consumer_overrun,
                s.morph_consumer_stale, s.morph_consumer_bad, s.morph_jump_checks,
                s.morph_jump_draws, s.morph_modified);

    if (morph_history_.size() > 4096)
      morph_history_.clear();

    last_stats_frame_ = frame;

    if (REXCVAR_GET(sst_native_check_late_writes))
      REXLOG_INFO("[sst-native]   late writes: {} draws checked, {} data ranges changed after the "
                  "draw ({} of them index data)",
                  s.late_checked, s.late_changed, s.late_changed_index);

    if (REXCVAR_GET(sst_native_check_vs) || REXCVAR_GET(sst_native_check_vs_all))
    {
      REXLOG_INFO("[sst-native]   VS check: {} on-screen jump checks, {} draws with a vertex "
                  "jumping alone",
                  s.check_jump_checks, s.check_jump_draws);

      std::vector<const CheckEntry *> sometimes;

      for (const auto &[k, e] : check_entries_)

        if (e.flagged && e.flagged < e.checked)
          sometimes.push_back(&e);

      std::sort(sometimes.begin(), sometimes.end(), [](auto *a, auto *b)
                { return a->flagged > b->flagged; });

      REXLOG_INFO("[sst-native]   VS check: {} draws run on the CPU, {} with far-out vertices; {} "
                  "shader/buffer pairs only sometimes far out:",
                  s.check_draws, s.check_flagged, sometimes.size());

      for (size_t i = 0; i < sometimes.size() && i < 12; ++i)
        REXLOG_INFO("[sst-native]     VS {:016X} buffer {:08X} prim {}{}: {} of {} draws", sometimes[i]->vs, sometimes[i]->stream, sometimes[i]->prim, sometimes[i]->morph ? " (morph output)" : "", sometimes[i]->flagged, sometimes[i]->checked);

      check_entries_.clear();
    }

    std::vector<std::pair<uint64_t, uint64_t>> rts(s.render_targets.begin(), s.render_targets.end());

    std::sort(rts.begin(), rts.end(), [](auto &a, auto &b)
              { return a.second > b.second; });

    std::string rt_text;

    for (size_t i = 0; i < rts.size() && i < 6; ++i)
    {
      const uint32_t size = uint32_t(rts[i].first);
      rt_text += fmt::format(" {:08X}[{:08X} ~{}x{}]:{}", uint32_t(rts[i].first >> 32), size, (size >> 18) + 1, ((size >> 3) & 0x7FFF) + 1, rts[i].second);
    }

    REXLOG_INFO("[sst-native]   render target 0 (object[size word]:draws){}", rt_text);

    // Per-interval counters
    const uint64_t created = s.pipelines_created, failed = s.pipelines_failed;
    const uint64_t logged = s.skip_shader_logged, clear_logged = s.clear_logged, sample_logged = s.sample_logged, bad_logged = s.memexport_bad_logged, consumer_bad_logged = s.morph_consumer_bad_logged, check_logged = s.check_logged, jump_logged = s.morph_jump_logged, modified_logged = s.morph_modified_logged, check_jump_logged = s.check_jump_logged, late_logged = s.late_logged, rebinds_logged = s.shader_rebinds_logged;
    s = Stats{};
    s.clear_logged = clear_logged;
    s.sample_logged = sample_logged;
    s.memexport_bad_logged = bad_logged;
    s.check_logged = check_logged;
    s.morph_jump_logged = jump_logged;
    s.morph_modified_logged = modified_logged;
    s.check_jump_logged = check_jump_logged;
    s.late_logged = late_logged;
    s.shader_rebinds_logged = rebinds_logged;
    s.morph_consumer_bad_logged = consumer_bad_logged;
    s.pipelines_created = created;
    s.pipelines_failed = failed;
    s.skip_shader_logged = logged;
  }
}