// Native renderer: setup, shutdown, upload memory, frame begin / end, stats

#include "gpu/native/renderer_internal.h"
#include "gpu/native/pipeline_workers.h"
#include "gpu/native/renderer_debug.h"

REXCVAR_DEFINE_BOOL(sst_native_quads, true, "SST", "Native renderer: draw QUADLIST / TRIFAN primitives (converted to triangle lists). Off until textures work: full-screen passes would cover the scene");
REXCVAR_DEFINE_BOOL(sst_native_debug_shading, false, "SST", "Native renderer: replace the game's pixel shaders with distance bands (checks geometry independently of lighting / textures)");
REXCVAR_DEFINE_BOOL(sst_native_half_pixel, true, "SST", "Native renderer: shift geometry by half a pixel (D3D9 pixel centres)");
REXCVAR_DEFINE_BOOL(sst_native_memexport, true, "SST", "Native renderer: run memory-export (morph) vertex shaders on the CPU so characters' faces and morphing meshes appear");
REXCVAR_DEFINE_BOOL(sst_native_present_queue, true, "SST", "Native renderer (render thread): show every frame once, in order, keeping one in reserve after a late frame (smooth, ~1 frame more latency). false = always show the newest frame");
REXCVAR_DEFINE_INT32(sst_native_present_wait_ms, 4, "SST", "Native renderer: at a swap, wait up to this long for the render thread to finish a new frame before showing the previous one again (0 = never wait)");
REXCVAR_DEFINE_INT32(sst_native_geometry_cache_mb, 384, "SST", "Native renderer: video memory for decoded vertex / index data kept between frames (0 = decode every draw)");
REXCVAR_DEFINE_BOOL(sst_native_pipeline_async, true, "SST", "Native renderer: create new pipelines on background threads (a draw whose pipeline isn't ready yet is skipped for those frames instead of stalling)");
REXCVAR_DEFINE_BOOL(sst_native_pipeline_library, true, "SST", "Native renderer: keep created pipelines in cache/pipelines.bin so later runs load them instead of compiling");
REXCVAR_DEFINE_INT32(sst_native_rt_trace, 0, "SST", "Native renderer debug: log the render-target passes, clears and resolves of this frame number (F10 traces the next frame)");
REXCVAR_DEFINE_BOOL(sst_native_render_targets, true, "SST", "Native renderer: emulate render targets and resolves (shadows, bloom, post-processing). false = everything drawn straight into the output (the earlier mode, with --sst_native_rt choosing targets)");
REXCVAR_DEFINE_BOOL(sst_native_memexport_async, true, "SST", "Native renderer: run memory-export (morph) shaders on a background thread; draws reading their output wait for them");
REXCVAR_DEFINE_BOOL(sst_native_diagnostics, false, "SST", "Native renderer debug: extra consistency checks on morph (face) output (costs CPU time)");
REXCVAR_DEFINE_BOOL(sst_native_memexport_cache, true, "SST", "Native renderer: skip a memory-export draw when its inputs and previous output are unchanged");
REXCVAR_DEFINE_STRING(sst_native_memexport_dump, "", "SST", "Native renderer debug: write the first few runs of each memory-export (morph) shader to this folder (microcode, constants, fetch constants, indices, input vertex data and the output) for offline benchmarking of the interpreter");
REXCVAR_DEFINE_INT32(sst_native_memexport_threads, -1, "SST", "Native renderer: extra threads for memory-export (morph) shaders; -1 = automatic (half the CPU threads minus one, at most 6)");
REXCVAR_DEFINE_BOOL(sst_native_check_vs, false, "SST", "Native renderer debug: run the vertex shaders of face (morph) draws on the CPU too and log vertices that jump on screen while the mesh stays put");
REXCVAR_DEFINE_BOOL(sst_native_check_vs_all, false, "SST", "Native renderer debug: like sst_native_check_vs for every draw (slow)");
REXCVAR_DEFINE_BOOL(sst_native_depth_flip_vs, true, "SST", "Native renderer: flip the game's reversed depth in the vertex shader (z = w - z) instead of with an inverted viewport depth range, which loses small depth gaps on Intel GPUs (eyes, moustaches, decals)");
REXCVAR_DEFINE_STRING(sst_native_trace_vs, "", "SST", "Native renderer debug: log the state of draws with this vertex shader hash (16 hex digits): vertex streams, decoded vertices, textures (first 40)");
REXCVAR_DEFINE_STRING(sst_native_skip_vs, "", "SST", "Native renderer debug: skip draws with this vertex shader hash (16 hex digits)");
REXCVAR_DEFINE_BOOL(sst_native_debug_markers, false, "SST", "Native renderer debug: label every draw for RenderDoc / PIX (shader hashes, depth / colour state)");
REXCVAR_DEFINE_BOOL(sst_native_debug_bisect, false, "SST", "Native renderer debug: find the pixel shader that draws something wrong. Home = start (takes the pixel shaders drawn since the last stop; half of them are hidden), Page Up = the problem is still visible, Page Down = the problem is gone, End = stop. Each step is logged");
REXCVAR_DEFINE_BOOL(sst_native_skip_blended, false, "SST", "Native renderer debug: skip alpha-blended draws (effects, glows, light beams), which look like solid white shapes until textures are emulated");
REXCVAR_DEFINE_BOOL(sst_native_check_late_writes, false, "SST", "Native renderer debug: report draws whose index / vertex data the game changes after issuing the draw (within the frame)");
REXCVAR_DEFINE_INT32(sst_native_capture_frame, 0, "SST", "Native renderer debug: capture this frame number (as F9 does): every draw's vertex positions from the GPU are compared with the original shader run on the CPU. -1 = every frame, logging only disagreements (slow)");
REXCVAR_DEFINE_BOOL(sst_native_ui_fit, true, "SST", "Native renderer: keep 2D UI in a 16:9 box at other aspect ratios (debug: false stretches it, to tell UI fitting apart from game-side issues)");
REXCVAR_DEFINE_BOOL(sst_native_wide_targets, true, "SST", "Native renderer: render targets take the display's shape (2520x1080 at 21:9 and 1080p) instead of a 16:9 image stretched to it");
REXCVAR_DEFINE_BOOL(sst_native_scissor, true, "SST", "Native renderer: apply the game's scissor rectangle (menus clip panels with it)");

// VIDEO page settings (src/menu/graphics_menu.cpp)
REXCVAR_DEFINE_STRING(sst_native_rt, "auto", "SST", "Native renderer: which render targets draw into the output until render targets are emulated: auto (viewport covers 1280x720), all, or a size as logged in 'render target' lines (e.g. 640x736)");

namespace sst::native
{
  using namespace renderer_detail;

  void SetGuestOverlays(const GuestOverlay *overlays, uint32_t count)
  {
    t_overlays = overlays;
    t_overlay_count = overlays ? count : 0;
  }

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

    {
      static Renderer *diagnostics = nullptr;
      diagnostics = this;

      sst::gpu::SetDiagnosticsDevice(device_, [](uint64_t frame, uint32_t draw)
                                     { return diagnostics ? diagnostics->DrawLabel(frame, draw) : std::string(); });
    }

    shaders_ = shaders;
    width_ = width;
    height_ = height;

    // Outputs + RTVs
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kOutputs;

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

    for (uint32_t i = 0; i < kOutputs; ++i)
    {
      if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&outputs_[i]))))
        return false;

      device_->CreateRenderTargetView(outputs_[i].Get(), nullptr, Rtv(i));
      output_scale_[i] = Scale2{};
    }

    for (uint32_t i = 0; i < kSlots; ++i)
    {
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
    CreateTimestampQueries();

    if (!CreateBlitResources())
      REXLOG_WARN("[sst-native] render-target resources unavailable: drawing into the output only");

    if (!CreateDescriptorsAndPlaceholders())
    {
      REXLOG_ERROR("[sst-native] failed to create placeholder textures");
      return false;
    }

    textures_ = std::make_unique<TextureCache>();
    textures_->Initialize(device_, physical_membase, srv_heap_.Get(), kSrvReserved, kSrvHeapSize - kSrvReserved, sampler_heap_.Get(), kSamplerReserved, kSamplerHeapSize - kSamplerReserved);
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
        {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 1, D3D12_SRV_DIMENSION_TEXTURE3D, kPlaceholder3DBlack},
    };

    // One upload buffer: 9 subresources, 512 bytes each (aligned for copies); the last one is black
    D3D12_HEAP_PROPERTIES upload_heap = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 9 * 512;
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
    std::memset(mapped + 8 * 512, 0, 512);

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

      for (uint32_t i = 4; i < kSrvReserved; ++i)
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

    for (uint32_t i = 0; i < kSamplerReserved; ++i)
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
    if (memexport_)
      CollectMemexports(true);

    if (fence_)
      WaitFence(fence_value_);

    if (event_)
      CloseHandle(event_);

    event_ = nullptr;

    // finishes compiles, saves the pipeline cache
    pipeline_workers_.reset();
    pending_pipelines_.clear();
    pipelines_.clear();

    if (textures_)
      textures_->Shutdown();

    geo_pool_.Reset();
    geo_entries_.clear();
    color_rts_.clear();
    depth_rts_.clear();
    resolved_.clear();
    retired_rts_.clear();
    blit_pipelines_.clear();
    textures_.reset();
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
          return {p.cpu + start, p.gpu + start, p.resource.Get(), start};
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

      p.resource->SetName(L"sst upload page");
      p.resource->Map(0, nullptr, reinterpret_cast<void **>(&p.cpu));
      p.gpu = p.resource->GetGPUVirtualAddress();
      f.pages.push_back(std::move(p));
      f.page = f.pages.size() - 1;
      f.offset = 0;
    }
  }

  void Renderer::BeginFrame()
  {
    {
      // VIDEO page settings. Targets / resolve textures are re-made at a new size on their next use. Internal resolution in lines of the game's frame (720, or 1024 in its 4:3 mode)
      const float lines = float(std::clamp(REXCVAR_GET(sst_resolution), 240, 4320)) / float(height_);
      const float scene_lines = lines * float(std::clamp(REXCVAR_GET(sst_render_scale), 25, 100)) / 100.0f;

      // Width follows the display's shape (RESOLUTION is the number of lines): at 21:9, 1080p renders 2520x1080 rather than a 1920x1080 image stretched. (Narrower displays - 16:10 - keep the full 16:9 width.)
      const float wide = REXCVAR_GET(sst_native_wide_targets) ? std::max(DisplayStretch(), 1.0f) : 1.0f;
      const Scale2 scale{lines * wide, lines}, scene{scene_lines * wide, scene_lines};

      // The stacked shadow map is a 3D texture: D3D12 caps those at 2048 texels
      const int shadow = std::clamp(REXCVAR_GET(sst_shadow_size), 0, 2048);
      const float shadow_k = shadow ? float(shadow) / 1024.0f : 0.125f;
      const Scale2 shadow_scale{shadow_k, shadow_k};
      wide_x_ = wide;

      if (scale != scale_ || scene != scene_scale_ || shadow_scale != shadow_scale_ || shadows_off_ != (shadow == 0))
      {
        REXLOG_INFO("[sst-native] internal resolution {}x{}, 3D {}x{}, shadow map {}", PxU(width_, scale.x), PxU(height_, scale.y), PxU(width_, scene.x), PxU(height_, scene.y), shadow ? fmt::format("{0}x{0}", shadow) : "off");
        scale_ = scale;
        scene_scale_ = scene;
        shadow_scale_ = shadow_scale;
        shadows_off_ = shadow == 0;
      }

      // Brightness 0-100 (50 = as rendered): the game's own curve is x^(1 / (0.9 + 0.006 b)) (its gamma ramp, which the port does not emulate), applied relative to its default (b = 50)
      const int b = std::clamp(REXCVAR_GET(sst_brightness), 0, 100);
      gamma_ = (0.9f + 0.006f * 50) / (0.9f + 0.006f * float(b));
      fps30_ = REXCVAR_GET(sst_fps_limit) <= 30;
      const int aa_type = REXCVAR_GET(sst_aa_type);
      const uint32_t msaa = aa_type == 2 ? uint32_t(std::clamp(REXCVAR_GET(sst_aa_level), 2, 8)) : 1;

      if (msaa != msaa_ || fxaa_ != (aa_type == 1) || bloom_off_ != !REXCVAR_GET(sst_bloom))
        REXLOG_INFO("[sst-native] anti-aliasing {}, bloom {}", aa_type == 1 ? std::string("FXAA") : msaa > 1 ? fmt::format("{}x MSAA", msaa)
                                                                                                             : std::string("off"),
                    REXCVAR_GET(sst_bloom) ? "on" : "off");

      msaa_ = msaa;
      fxaa_ = aa_type == 1;
      bloom_off_ = !REXCVAR_GET(sst_bloom);
      const int aspect = DisplayAspect();
      const float s = DisplayStretch();
      ui_fit_x_ = s > 1.0f ? 1.0f / s : 1.0f;

      // Narrower than 16:9 (16:10): the 2D layer is stretched to the full height (11 % taller) instead of fitted into a 16:9 box. The rules that decide what to fit are built around the screen's width (full-width layers stay full), so a vertical fit split bars from their texts and left uncovered bands above and below the menus; stretched, everything stays together
      ui_fit_y_ = 1.0f;

      if (aspect != aspect_)
        REXLOG_INFO("[sst-native] aspect ratio {}: UI fit {:.3f} x {:.3f}", std::array<const char *, 5>{"4:3", "16:9", "16:10", "21:9", "32:9"}[aspect], ui_fit_x_, ui_fit_y_);

      aspect_ = aspect;
    }

    {
      std::lock_guard lock(slot_mutex_);

      // A slot that holds nothing still to be shown: not the one being copied (pinned), not the last one shown (a swap may show it again), not a frame waiting its turn; else the oldest waiting frame (dropped)
      int pick = -1, oldest_waiting = -1;

      for (uint32_t i = 0; i < kOutputs; ++i)
      {
        const int s = int(i);

        if (s == pinned_ || s == acquired_slot_ || s == latest_)
          continue;

        if (output_seq_[i] > acquired_seq_)
        {
          if (oldest_waiting < 0 || output_seq_[i] < output_seq_[oldest_waiting])
            oldest_waiting = s;

          continue;
        }

        pick = s;
        break;
      }

      if (pick < 0)
        pick = oldest_waiting >= 0 ? oldest_waiting : (latest_ + 1) % int(kOutputs);

      if (pick == pinned_)
        pick = (pick + 1) % int(kOutputs);

      target_ = uint32_t(pick);
      output_seq_[target_] = 0;

      if (output_scale_[target_] != scale_ || depth_scale_ != scale_)
        ApplyScale();
    }

    frame_res_ = uint32_t(submitted_ % kSlots);
    FrameResources &f = frames_[frame_res_];

    {
      const uint64_t t0 = Tick();
      WaitFence(f.fence_value);
      prof_.ticks[kPhFenceWait] += Tick() - t0;
    }

    ReadGpuTime(frame_res_);
    constant_cache_.clear();
    bound_targets_ = 0;

    {
      const uint64_t done = fence_->GetCompletedValue();

      std::erase_if(retired_rts_, [&](const auto &r)
                    { return r.first <= done; });
    }

    // decayed after the last submission
    geo_state_ = D3D12_RESOURCE_STATE_COMMON;

    if (geo_entries_.size() > 400000)
    {
      // forget old keys (data is re-stored on next use)
      geo_entries_.clear();
      ++geo_generation_;
      geo_offset_ = 0;
    }

    f.allocator->Reset();
    f.page = 0;
    f.offset = 0;
    list_->Reset(f.allocator.Get(), nullptr);

    {
      DrawLabels &labels = draw_labels_[(submitted_ + 1) % std::size(draw_labels_)];
      labels.frame = submitted_ + 1;
      labels.draws.clear();
      const std::wstring name = L"sst frame " + std::to_wstring(labels.frame);
      list_->SetName(name.c_str());
    }

    if (timestamps_)
      list_->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame_res_);

    pass_marks_[frame_res_].clear();
    pass_pair_ = 0;
    MarkPass(0, 0, 0, 3);

    auto to_rt = Transition(outputs_[target_].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &to_rt);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(target_);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // Start each frame defined (the game clears explicitly too)
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

    textures_->BeginFrame(submitted_ + 1, fence_->GetCompletedValue(), fence_value_ + 1);
    ID3D12DescriptorHeap *heaps[] = {srv_heap_.Get(), sampler_heap_.Get()};
    list_->SetDescriptorHeaps(2, heaps);
    list_->SetGraphicsRootSignature(root_signature_.Get());

    for (UINT i = 0; i < 3; ++i)
      list_->SetGraphicsRootDescriptorTable(kRootTextures2D + i, srv_heap_->GetGPUDescriptorHandleForHeapStart());

    list_->SetGraphicsRootDescriptorTable(kRootSamplers, sampler_heap_->GetGPUDescriptorHandleForHeapStart());
    const D3D12_RECT scissor = Scaled({0, 0, LONG(width_), LONG(height_)}, output_scale_[target_]);
    list_->RSSetScissorRects(1, &scissor);
    recording_ = true;
  }

  void Renderer::AddTextureSnapshot(uint32_t begin, uint32_t end, const uint8_t *data)
  {
    if (textures_)
      textures_->AddSnapshot(begin, end, data);
  }

  void Renderer::AddTextureUnchanged(uint32_t base_page, uint64_t hash)
  {
    if (textures_)
      textures_->AddUnchanged(base_page, hash);
  }

  void Renderer::EndFrame()
  {
    PhaseClock clk(prof_, kPhSubmit);

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

          REXLOG_WARN("[sst-native] late write: {} data of draw (VS {:016X}, prim {}, count {}{}) changed after the draw was issued, before the frame ended ({} bytes at host {}, {} draws ago)",
                      g.index ? std::string("index") : fmt::format("vertex stream {}", lc.ranges[0].index ? r - 1 : r), lc.vs, lc.prim, lc.count, lc.morph ? ", reads morph output" : "", g.size, fmt::ptr(g.ptr), draw_seq_ - lc.draw);
        }
      }
    }

    stats_.late_checked += late_checks_.size();
    late_checks_.clear();

    MarkPass(0, 0, 0, 4);

    if (RenderTargetsEnabled())
      PresentRenderTargets();

    auto to_src = Transition(outputs_[target_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(1, &to_src);
    const bool capturing = capture_active_;

    if (capturing)
    {
      D3D12_RESOURCE_BARRIER b[2] = {Transition(so_buffer_.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE), {}};
      list_->ResourceBarrier(1, b);
      list_->CopyResource(so_readback_.Get(), so_buffer_.Get());
      b[0] = Transition(so_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      list_->ResourceBarrier(1, b);
      capture_active_ = false;
    }

    if (timestamps_)
    {
      list_->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame_res_ + 1);
      list_->ResolveQueryData(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame_res_, 2, timestamp_readback_.Get(), 16 * frame_res_);
      timestamp_pending_[frame_res_] = true;

      if (pass_ts_)
      {
        std::vector<PassMark> &marks = pass_marks_[frame_res_];
        list_->EndQuery(pass_ts_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame_res_ * kPassMarks + uint32_t(marks.size()));
        marks.push_back({0, 0, 0, 4}); // end (closes the last segment; not counted itself)
        list_->ResolveQueryData(pass_ts_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame_res_ * kPassMarks, uint32_t(marks.size()), pass_readback_.Get(), 8 * uint64_t(frame_res_) * kPassMarks);
      }
    }

    list_->Close();
    ID3D12CommandQueue *queue = provider_->GetDirectQueue();
    ID3D12CommandList *lists[] = {list_.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence_.Get(), ++fence_value_);
    frames_[frame_res_].fence_value = fence_value_;

    if (sst::gpu::DumpDeviceRemoved())
      REXLOG_ERROR("[sst-native] the graphics device was lost at frame {} (see the [sst-gpu] lines above)", submitted_ + 1);

    ++submitted_;
    recording_ = false;
    memexport_ranges_.clear();

    if (capturing)
    {
      WaitFence(fence_value_);
      FinishCapture();
      captured_.clear();
    }

    Bisect().Poll();

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

    // Render-target trace: log the frame that just ended; F10 (or --sst_native_rt_trace) traces the next one
    if (rt_trace_.active)
    {
      TraceClosePass();
      REXLOG_INFO("[rt-trace] ---- frame {} ({} events) ----", rt_trace_.frame, rt_trace_.lines.size());

      for (const std::string &line : rt_trace_.lines)
        REXLOG_INFO("[rt-trace] {}", line);

      REXLOG_INFO("[rt-trace] ---- end of frame {} ----", rt_trace_.frame);
      rt_trace_.active = false;
      rt_trace_.lines.clear();
    }

    if ((GetAsyncKeyState(VK_F10) & 1) || (REXCVAR_GET(sst_native_rt_trace) > 0 && submitted_ + 1 == uint64_t(REXCVAR_GET(sst_native_rt_trace))))
    {
      rt_trace_.active = true;
      rt_trace_.frame = submitted_ + 1;
      rt_trace_.has_pass = false;
      rt_trace_.pass_index = 0;
      rt_trace_.lines.clear();
    }

    CollectMemexports(false);
    memexport_written_prev_.swap(memexport_written_);
    clip_occurrence_.clear();
    memexport_written_.clear();

    {
      std::lock_guard lock(slot_mutex_);
      latest_ = int(target_);
      output_seq_[target_] = ++latest_seq_;
    }

    frame_cv_.notify_all();

    // Frame pacing: wall time between Swaps
    const auto now = std::chrono::steady_clock::now();

    if (prof_.last_frame.time_since_epoch().count())
    {
      const double ms = std::chrono::duration<double, std::milli>(now - prof_.last_frame).count();
      prof_.frame_ms_sum += ms;
      prof_.frame_ms_max = std::max(prof_.frame_ms_max, ms);
      ++prof_.frames;

      if (ms > 17.5)
        ++prof_.over_60;

      if (ms > 20.0)
        ++prof_.over_20;

      if (ms > 34.0)
        ++prof_.over_30;
    }

    prof_.last_frame = now;

    if (textures_)
      textures_->ClearSnapshots(); // their memory (the frame's arena) is recycled after this
  }

  Renderer::Renderer() = default;
  Renderer::~Renderer() = default;

  void Renderer::LogStats(uint64_t frame)
  {
    auto &s = stats_;
    LogProfile();

    if (pipeline_workers_)
    {
      REXLOG_INFO("[sst-native]   pipelines: {} ready ({} from the pipeline cache), {} compiling, {:.1f} ms of background work this period, draws skipped while compiling {}, UI-like pipelines built at once {} ({:.1f} ms), cache file {} KB",
                  s.pipelines_created, s.pipelines_from_cache, pending_pipelines_.size(), s.pipeline_ms, s.skip_pipeline_pending, s.pipelines_urgent, s.pipeline_urgent_ms, pipeline_workers_->saved_bytes >> 10);

      pipeline_workers_->SaveInBackground();
    }

    if (textures_)
      REXLOG_INFO("[sst-native] {}; draws with textures {} (placeholders {}, shadow map stand-ins {}); scissored draws {}", textures_->StatusLine(), s.textured_draws, s.texture_placeholders, s.shadow_placeholders, s.scissored_draws);

    REXLOG_INFO("[sst-native] frame {}: {} draws, {} drawn ({} depth-only); skipped: shader {} pending {} fetch {} prim {} rt {} pipeline {} data {} cull {} memexport-target {}; {} clears; pipelines {} ({} failed); shaders rebuilt from objects {}; upload {:.1f} MB; resolves {}; vertex buffers from draw-time copies {}",
                frame, s.draws, s.drawn, s.depth_only, s.skip_shader, s.skip_pending, s.skip_decl, s.skip_prim, s.skip_rt, s.skip_pipeline, s.skip_data, s.skip_cull, s.skip_memexport, s.clears, s.pipelines_created, s.pipelines_failed, s.shaders_from_objects, double(s.upload_bytes) / (1 << 20), s.resolves, s.vertex_copies_used);

    if (s.measured_draws)
      REXLOG_INFO("[sst-native] ui fit: {} draws measured on the CPU ({} 3D, stopped after the probe; {} big batches sampled)", s.measured_draws, s.measured_3d_early, s.measured_sampled);

    std::string prims;

    for (auto &[p, n] : s.prims)
      prims += fmt::format(" {:X}:{}", p, n);

    std::string clears;

    for (auto &[f, n] : s.clear_flags)
      clears += fmt::format(" {:X}:{}", f, n);

    REXLOG_INFO("[sst-native]   prims{} | clear flags{}", prims, clears);
    const MemexportStats &m = memexport_stats_;

    REXLOG_INFO("[sst-native]   memexport: {} draws, {} run ({} in the background, render thread waited {} times), constants: {} files uploaded, {} reused", s.memexport_draws, s.memexport_run, s.memexport_async, s.memexport_waits, s.constants_uploaded, s.constants_reused);
    REXLOG_INFO("[sst-native]   render targets: {} host targets made, resolves {} done / {} skipped, rectangle lists {}, frames shown from the front-buffer resolve {}", s.host_rts, s.resolves_done, s.resolves_skipped, s.rects_converted, s.presented_from_resolve);

    REXLOG_INFO("[sst-native]   geometry cache: vertex hits {}, index hits {} ({} trusted without re-hashing), stored {} ({:.1f} MB), dynamic (decoded each time) {}, flushes {}; {} keys, {:.0f} of {} MB used",
                s.geo_vertex_hits, s.geo_index_hits, s.geo_trusted, s.geo_stores, double(s.geo_store_bytes) / (1 << 20), s.geo_dynamic, s.geo_flushes, geo_entries_.size(), double(geo_offset_) / (1 << 20), geo_pool_size_ >> 20);

    REXLOG_INFO("[sst-native]   memexport: {} draws, {} run ({:.2f} ms total, {:.3f} ms/frame), {} cached, {} not run; totals: {} vertices, {} elements, bad address {}, bad format {}, out of range {}, fetch out of bounds {}, malformed {}, per-vertex {}",
                s.memexport_draws, s.memexport_run, s.memexport_ms, s.memexport_ms / double(std::max<uint64_t>(1, frame - last_stats_frame_)), s.memexport_cached, s.memexport_failed, m.vertices, m.elements, m.bad_address, m.bad_format, m.out_of_range, m.fetch_oob, m.malformed, m.per_vertex_draws);

    if (!memexport_costs_.empty())
    {
      std::vector<std::pair<uint64_t, MemexportShaderCost>> costs(memexport_costs_.begin(), memexport_costs_.end());

      std::sort(costs.begin(), costs.end(), [](const auto &a, const auto &b)
                { return a.second.ms > b.second.ms; });

      std::string text;

      for (size_t i = 0; i < costs.size() && i < 6; ++i)
      {
        const MemexportShaderCost &c = costs[i].second;
        text += fmt::format(" {:016X}: {:.1f} ms, {} runs ({} cached), {:.0f} ns/vertex;", costs[i].first, c.ms, c.runs, c.cached, c.vertices ? 1e6 * c.ms / double(c.vertices) : 0.0);
      }

      REXLOG_INFO("[sst-native]   memexport by shader:{}", text);
      memexport_costs_.clear();
    }

    if (s.fetch_remapped)
      REXLOG_INFO("[sst-native]   draws whose inputs sat in other fetch slots (matched by register): {}", s.fetch_remapped);

    if (s.computed_fetch_draws)
      REXLOG_INFO("[sst-native]   draws fetching with a computed index (CPU-resolved): {}", s.computed_fetch_draws);

    if (s.shader_rebinds)
      REXLOG_INFO("[sst-native]   shader objects reused for a different shader: {}", s.shader_rebinds);

    REXLOG_INFO("[sst-native]   morph checks: {} draws with implausible output; consumers {} (reading past the output {}, stale {}, implausible inputs {}); jump checks {}, draws with jumping vertices {}; output changed before use {}",
                s.memexport_bad_outputs, s.morph_consumers, s.morph_consumer_overrun, s.morph_consumer_stale, s.morph_consumer_bad, s.morph_jump_checks, s.morph_jump_draws, s.morph_modified);

    if (morph_history_.size() > 4096)
      morph_history_.clear();

    last_stats_frame_ = frame;

    if (REXCVAR_GET(sst_native_check_late_writes))
      REXLOG_INFO("[sst-native]   late writes: {} draws checked, {} data ranges changed after the draw ({} of them index data)", s.late_checked, s.late_changed, s.late_changed_index);

    if (REXCVAR_GET(sst_native_check_vs) || REXCVAR_GET(sst_native_check_vs_all))
    {
      REXLOG_INFO("[sst-native]   VS check: {} on-screen jump checks, {} draws with a vertex jumping alone", s.check_jump_checks, s.check_jump_draws);

      std::vector<const CheckEntry *> sometimes;

      for (const auto &[k, e] : check_entries_)
      {
        if (e.flagged && e.flagged < e.checked)
          sometimes.push_back(&e);
      }

      std::sort(sometimes.begin(), sometimes.end(), [](auto *a, auto *b)
                { return a->flagged > b->flagged; });

      REXLOG_INFO("[sst-native]   VS check: {} draws run on the CPU, {} with far-out vertices; {} shader/buffer pairs only sometimes far out:", s.check_draws, s.check_flagged, sometimes.size());

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
    const uint64_t created = s.pipelines_created, failed = s.pipelines_failed, from_cache = s.pipelines_from_cache;
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
    s.pipelines_from_cache = from_cache;
    s.skip_shader_logged = logged;
  }
}