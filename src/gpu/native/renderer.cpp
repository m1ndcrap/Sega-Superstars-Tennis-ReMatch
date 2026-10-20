// Native renderer: setup, shutdown, upload memory, frame begin / end, stats

#include "gpu/native/renderer_internal.h"
#include "gpu/native/pipeline_workers.h"
#include "gpu/native/renderer_debug.h"

#include "gpu/native/dxc.h"

#if defined(_WIN32)
#include "gpu/native/gfx/d3d12/d3d12_gfx.h"
#endif

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
  bool Renderer::Initialize(std::unique_ptr<gfx::Device> device, ShaderCache *shaders, const uint8_t *physical_membase, uint32_t width, uint32_t height)
  {
    gfx_ = std::move(device);
    physical_membase_ = physical_membase;

    if (!gfx_)
      return false;

    // Guest physical memory is writable host memory; memexport writes into it
    {
      int32_t threads = REXCVAR_GET(sst_native_memexport_threads);

      if (threads < 0)
        threads = std::clamp(int32_t(std::thread::hardware_concurrency()) / 2 - 1, 0, 6);

      memexport_ = std::make_unique<MemexportRunner>(const_cast<uint8_t *>(physical_membase), uint32_t(std::min(threads, 16)));
      REXLOG_INFO("[sst-native] memexport: {} worker threads", memexport_->worker_threads());
    }

#if defined(_WIN32)
    if (ID3D12Device *d3d = gfx::d3d12::NativeDevice(*gfx_))
    {
      static Renderer *diagnostics = nullptr;
      diagnostics = this;

      sst::gpu::SetDiagnosticsDevice(d3d, [](uint64_t frame, uint32_t draw)
                                     { return diagnostics ? diagnostics->DrawLabel(frame, draw) : std::string(); });
    }
#endif

    shaders_ = shaders;
    width_ = width;
    height_ = height;

    // View tables: shader-visible SRVs / samplers (bindless), RTVs (the outputs, then render targets), DSVs (the output depth, then depth targets)
    if (!gfx_->CreateTables({kSrvHeapSize, kSamplerHeapSize, kOutputs + kRtRtvCount, 1 + kRtDsvCount}) || !gfx_->CreateLayouts())
    {
      REXLOG_ERROR("[sst-native] failed to create the descriptor tables / root signatures");
      return false;
    }

    // Outputs + RTVs
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    for (uint32_t i = 0; i < kOutputs; ++i)
    {
      outputs_[i] = gfx_->CreateTexture(desc, D3D12_RESOURCE_STATE_COPY_SOURCE);

      if (!outputs_[i])
        return false;

      gfx_->WriteRtv(Rtv(i), *outputs_[i], nullptr);
      output_scale_[i] = Scale2{};
    }

    list_ = gfx_->CreateCommandList(kSlots);

    if (!list_)
      return false;

    // Depth
    desc.Format = kDepthFormat;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kDepthFormat;
    clear.DepthStencil.Depth = 1.0f;
    depth_ = gfx_->CreateTexture(desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);

    if (!depth_)
      return false;

    gfx_->WriteDsv(kOutputDsv, *depth_, nullptr);
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
    textures_->Initialize(*gfx_, physical_membase, kSrvReserved, kSrvHeapSize - kSrvReserved, kSamplerReserved, kSamplerHeapSize - kSamplerReserved);
    return true;
  }

  bool Renderer::CreateDescriptorsAndPlaceholders()
  {
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
    gfx::BufferRef staging = gfx_->CreateBuffer(9 * 512, gfx::HeapType::kUpload, D3D12_RESOURCE_STATE_GENERIC_READ);

    if (!staging)
      return false;

    std::memset(staging->cpu, 0xFF, 8 * 512);
    std::memset(staging->cpu + 8 * 512, 0, 512);

    // Zero buffer for missing vertex inputs (also upload heap; never written)
    zero_buffer_ = gfx_->CreateBuffer(256, gfx::HeapType::kUpload, D3D12_RESOURCE_STATE_GENERIC_READ);

    if (!zero_buffer_)
      return false;

    std::memset(zero_buffer_->cpu, 0, 256);
    list_->Begin(0, "sst placeholders");
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
      gfx::TextureRef tex = gfx_->CreateTexture(td, D3D12_RESOURCE_STATE_COPY_DEST);

      if (!tex)
        return false;

      const UINT subresources = k.dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : k.depth_or_array;

      for (UINT s = 0; s < subresources; ++s)
      {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        fp.Offset = 512 * staging_index++;
        fp.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256};
        list_->CopyBufferToTexture(*tex, s, *staging, fp);
      }

      list_->Barrier(*tex, D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);

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

      gfx_->WriteSrv(k.heap_index, *tex, sd);
      placeholder_textures_.push_back(tex);
    }

    // Fill the rest of the reserved slots with the 2D placeholder so any index is valid
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      sd.Texture2D.MipLevels = 1;

      for (uint32_t i = 4; i < kSrvReserved; ++i)
        gfx_->WriteSrv(i, *placeholder_textures_[0], sd);
    }

    // Samplers: linear wrap everywhere
    D3D12_SAMPLER_DESC smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;

    for (uint32_t i = 0; i < kSamplerReserved; ++i)
      gfx_->WriteSampler(i, smp);

    list_->End();
    gfx_->WaitFence(gfx_->Submit(*list_));
    return true;
  }

  void Renderer::Shutdown()
  {
    if (memexport_)
      CollectMemexports(true);

    if (gfx_)
      gfx_->WaitFence(gfx_->LastSubmitted());

    // finishes compiles, saves the pipeline cache
    pipeline_workers_.reset();
    pending_pipelines_.clear();
    pipelines_.clear();

    if (textures_)
      textures_->Shutdown();

    geo_pool_.reset();
    geo_entries_.clear();
    color_rts_.clear();
    depth_rts_.clear();
    resolved_.clear();
    retired_rts_.clear();
    blit_pipelines_.clear();
    textures_.reset();
    list_.reset();

    for (auto &f : frames_)
      f.pages.clear();

    for (auto &o : outputs_)
      o.reset();

    placeholder_textures_.clear();
    zero_buffer_.reset();
    depth_.reset();
    so_pipelines_.clear();
    so_buffer_.reset();
    so_readback_.reset();
    so_zero_.reset();
    timestamps_.reset();
    timestamp_readback_.reset();
    pass_ts_.reset();
    pass_readback_.reset();
    debug_ps_.reset();
    gfx_.reset();
  }

  bool Renderer::CompileRendererShader(const char *source, const wchar_t *profile, std::vector<uint8_t> &out)
  {
    const gfx::ShaderFormat format = gfx_->shader_format();
    dxc::Ref<IDxcCompiler3> compiler;

    if (FAILED(dxc::CreateInstance(format, CLSID_DxcCompiler, __uuidof(IDxcCompiler3), compiler.put())))
      return false;

    // SPIR-V: the source picks its Vulkan bindings under __spirv__ (renderer_present.cpp); vertex shaders flip y like the translated ones
    std::vector<const wchar_t *> args = {L"-T", profile, L"-E", L"main", L"-Qstrip_debug"};

    if (format == gfx::ShaderFormat::kSpirv)
    {
      args.insert(args.end(), {L"-spirv", L"-fvk-use-dx-layout", L"-fspv-target-env=vulkan1.2"});

      if (profile[0] == L'v')
        args.push_back(pack::kDxcSpirvVsArg);
    }
    else
    {
      args.push_back(L"-Qstrip_reflect");
    }

    std::string errors;

    if (!dxc::Compile(compiler.get(), source, args.data(), uint32_t(args.size()), out, &errors))
    {
      REXLOG_ERROR("[sst-native] renderer shader ({}): {}", format == gfx::ShaderFormat::kSpirv ? "SPIR-V" : "DXIL", errors);
      return false;
    }

    return true;
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
          return {p.cpu + start, p.gpu + start, p.buffer.get(), start};
        }

        ++f.page;
        f.offset = 0;
        continue;
      }

      UploadPage p;
      p.size = std::max(kUploadPageSize, (size + 0xFFFF) & ~size_t(0xFFFF));
      p.buffer = gfx_->CreateBuffer(p.size, gfx::HeapType::kUpload, D3D12_RESOURCE_STATE_GENERIC_READ);

      if (!p.buffer)
        return {};

      p.buffer->SetName("sst upload page");
      p.cpu = p.buffer->cpu;
      p.gpu = p.buffer->gpu;
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
      gfx_->WaitFence(f.fence_value);
      prof_.ticks[kPhFenceWait] += Tick() - t0;
    }

    ReadGpuTime(frame_res_);
    constant_cache_.clear();
    bound_targets_ = 0;

    {
      const uint64_t done = gfx_->CompletedFence();

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

    f.page = 0;
    f.offset = 0;

    {
      DrawLabels &labels = draw_labels_[(submitted_ + 1) % std::size(draw_labels_)];
      labels.frame = submitted_ + 1;
      labels.draws.clear();
      list_->Begin(frame_res_, "sst frame " + std::to_string(labels.frame));
    }

    if (timestamps_)
      list_->WriteTimestamp(*timestamps_, 2 * frame_res_);

    pass_marks_[frame_res_].clear();
    pass_pair_ = 0;
    MarkPass(0, 0, 0, 3);

    list_->Barrier(*outputs_[target_], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->SetTargets(Rtv(target_), kOutputDsv);

    // Start each frame defined (the game clears explicitly too)
    static constexpr float kBlack[4] = {0, 0, 0, 1};
    list_->ClearRtv(Rtv(target_), kBlack);
    list_->ClearDsv(kOutputDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, last_depth_clear_, 0);

    if (capture_requested_ && EnsureCaptureResources())
    {
      capture_requested_ = false;
      capture_active_ = true;
      captured_.clear();
      so_used_ = 0;
      list_->CopyBuffer(*so_buffer_, kSoDataBytes, *so_zero_, 0, kSoCounterBytes);
      list_->Barrier(*so_buffer_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);

      if (!capture_quiet_)
        REXLOG_INFO("[sst-native] capture: recording frame {}", submitted_ + 1);
    }

    textures_->BeginFrame(submitted_ + 1, gfx_->CompletedFence(), gfx_->LastSubmitted() + 1);
    list_->SetLayout(gfx::Layout::kDraw);
    const D3D12_RECT scissor = Scaled({0, 0, LONG(width_), LONG(height_)}, output_scale_[target_]);
    list_->SetScissor(scissor);
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

    list_->Barrier(*outputs_[target_], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const bool capturing = capture_active_;

    if (capturing)
    {
      list_->Barrier(*so_buffer_, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list_->CopyBufferAll(*so_readback_, *so_buffer_);
      list_->Barrier(*so_buffer_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      capture_active_ = false;
    }

    if (timestamps_)
    {
      list_->WriteTimestamp(*timestamps_, 2 * frame_res_ + 1);
      list_->ResolveTimestamps(*timestamps_, 2 * frame_res_, 2, *timestamp_readback_, 16 * frame_res_);
      timestamp_pending_[frame_res_] = true;

      if (pass_ts_)
      {
        std::vector<PassMark> &marks = pass_marks_[frame_res_];
        list_->WriteTimestamp(*pass_ts_, frame_res_ * kPassMarks + uint32_t(marks.size()));
        marks.push_back({0, 0, 0, 4}); // end (closes the last segment; not counted itself)
        list_->ResolveTimestamps(*pass_ts_, frame_res_ * kPassMarks, uint32_t(marks.size()), *pass_readback_, 8 * uint64_t(frame_res_) * kPassMarks);
      }
    }

    list_->End();
    frames_[frame_res_].fence_value = gfx_->Submit(*list_);

#if defined(_WIN32)
    if (sst::gpu::DumpDeviceRemoved())
      REXLOG_ERROR("[sst-native] the graphics device was lost at frame {} (see the [sst-gpu] lines above)", submitted_ + 1);
#endif

    ++submitted_;
    recording_ = false;
    memexport_ranges_.clear();

    if (capturing)
    {
      gfx_->WaitFence(gfx_->LastSubmitted());
      FinishCapture();
      captured_.clear();
    }

    Bisect().Poll();

    // F9 (or --sst_native_capture_frame) captures the next frame
    capture_quiet_ = REXCVAR_GET(sst_native_capture_frame) == -1;

    if (DebugKeyPressed(0x78)) // F9
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

    if (DebugKeyPressed(0x79) || (REXCVAR_GET(sst_native_rt_trace) > 0 && submitted_ + 1 == uint64_t(REXCVAR_GET(sst_native_rt_trace))))
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