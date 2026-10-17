// Native renderer: blits, presenting frames, FPS counter

#include "gpu/native/renderer_internal.h"

namespace sst::native
{
  using namespace renderer_detail;

  bool Renderer::CreateBlitResources()
  {
    // Root signature: b0 = 8 root constants (source offset, flags, gamma; source rectangle for FXAA), t0 = source
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 8};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC linear = {};
    linear.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linear.AddressU = linear.AddressV = linear.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linear.MaxLOD = D3D12_FLOAT32_MAX;
    linear.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &linear;
    ComPtr<ID3DBlob> blob, error;

    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) || FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&blit_root_))))
      return false;

    static const char kVS[] = "float4 main(uint id : SV_VertexID) : SV_Position {\n"
                              "  float2 uv = float2((id << 1) & 2, id & 2);\n"
                              "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
                              "}\n";

    static const char kPS[] = "cbuffer C : register(b0) { int4 offset; };\n"
                              "Texture2D<float4> src : register(t0);\n"
                              "float4 main(float4 p : SV_Position) : SV_Target {\n"
                              "  float4 c = src.Load(int3(int2(p.xy) + offset.xy, 0));\n"
                              "  if (offset.z & 1) c = c.bgra;\n"
                              "  if (offset.w != 0) c.rgb = pow(saturate(c.rgb), asfloat(offset.w));\n"
                              "  return c;\n"
                              "}\n";

    // offset.z: bit 0 swap red / blue, bits 8-15 source samples, bit 16 depth
    static const char kPSMS[] = "cbuffer C : register(b0) { int4 offset; };\n"
                                "Texture2DMS<float4> src : register(t0);\n"
                                "float4 main(float4 p : SV_Position) : SV_Target {\n"
                                "  int2 at = int2(p.xy) + offset.xy;\n"
                                "  uint n = max((uint(offset.z) >> 8) & 0xFF, 1u);\n"
                                "  float4 c = src.Load(at, 0);\n"
                                "  if (((uint(offset.z) >> 16) & 1) == 0) {\n"
                                "    for (uint i = 1; i < n; ++i) c += src.Load(at, i);\n"
                                "    c /= float(n);\n"
                                "  }\n"
                                "  if (offset.z & 1) c = c.bgra;\n"
                                "  if (offset.w != 0) c.rgb = pow(saturate(c.rgb), asfloat(offset.w));\n"
                                "  return c;\n"
                                "}\n";

    // FXAA (the compact 'console' variant: 4 diagonal luma taps, one blur direction, two / four taps along it)
    static const char kPSFxaa[] = "cbuffer C : register(b0) { int4 offset; int4 rect; };\n"
                                  "Texture2D<float4> src : register(t0);\n"
                                  "SamplerState lin : register(s0);\n"
                                  "static float2 lo_uv, hi_uv;\n"
                                  "float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
                                  // Taps stay inside the copied rectangle: with tiled rendering the rest of the source holds another tile's (older) pixels
                                  "float3 S(float2 uv) { return src.SampleLevel(lin, clamp(uv, lo_uv, hi_uv), 0).rgb; }\n"
                                  "float4 main(float4 p : SV_Position) : SV_Target {\n"
                                  "  uint w, h; src.GetDimensions(w, h);\n"
                                  "  float2 rcp = 1.0 / float2(w, h);\n"
                                  "  lo_uv = (float2(rect.xy) + 0.5) * rcp;\n"
                                  "  hi_uv = (float2(rect.zw) - 0.5) * rcp;\n"
                                  "  float2 uv = (floor(p.xy) + float2(offset.xy) + 0.5) * rcp;\n"
                                  "  float4 m = src.SampleLevel(lin, uv, 0);\n"
                                  "  float nw = Luma(S(uv + float2(-0.5, -0.5) * rcp));\n"
                                  "  float ne = Luma(S(uv + float2(0.5, -0.5) * rcp));\n"
                                  "  float sw = Luma(S(uv + float2(-0.5, 0.5) * rcp));\n"
                                  "  float se = Luma(S(uv + float2(0.5, 0.5) * rcp));\n"
                                  "  float lm = Luma(m.rgb);\n"
                                  "  float lo = min(lm, min(min(nw, ne), min(sw, se)));\n"
                                  "  float hi = max(lm, max(max(nw, ne), max(sw, se)));\n"
                                  "  float4 c = m;\n"
                                  "  if (hi - lo >= max(0.0312, hi * 0.125)) {\n"
                                  "    float2 dir = float2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));\n"
                                  "    float reduce = max((nw + ne + sw + se) * (0.25 * 0.125), 1.0 / 128.0);\n"
                                  "    float k = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
                                  "    dir = clamp(dir * k, -8.0, 8.0) * rcp;\n"
                                  "    float3 a = 0.5 * (S(uv + dir * (1.0 / 3.0 - 0.5)) + S(uv + dir * (2.0 / 3.0 - 0.5)));\n"
                                  "    float3 b = a * 0.5 + 0.25 * (S(uv - dir * 0.5) + S(uv + dir * 0.5));\n"
                                  "    float lb = Luma(b);\n"
                                  "    c.rgb = (lb < lo || lb > hi) ? a : b;\n"
                                  "  }\n"
                                  "  if (offset.z & 1) c = c.bgra;\n"
                                  "  return c;\n"
                                  "}\n";

    ComPtr<IDxcCompiler3> compiler;

    if (FAILED(provider_->DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
      return false;

    auto compile = [&](const char *source, const wchar_t *profile, std::vector<uint8_t> &out)
    {
      const wchar_t *args[] = {L"-T", profile, L"-E", L"main", L"-Qstrip_reflect", L"-Qstrip_debug"};
      DxcBuffer buffer = {source, std::strlen(source), DXC_CP_UTF8};
      ComPtr<IDxcResult> result;
      HRESULT status = E_FAIL;

      if (FAILED(compiler->Compile(&buffer, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result))) || FAILED(result->GetStatus(&status)) || FAILED(status))
        return false;

      ComPtr<IDxcBlob> object;

      if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
        return false;

      const auto *b = static_cast<const uint8_t *>(object->GetBufferPointer());
      out.assign(b, b + object->GetBufferSize());
      return true;
    };

    // FPS counter: c.xy = top-left (host pixels), c.z = font pixel size, c.w = frames per second x 10. 3x5 digits, drawn on a dark box
    static const char kPSText[] = "cbuffer C : register(b0) { int4 c; };\n"
                                  "static const uint kFont[11] = {0x7b6f, 0x2c97, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef,\n"
                                  "                               0x7249, 0x7bef, 0x7bcf, 0x2};\n"
                                  "float4 main(float4 p : SV_Position) : SV_Target {\n"
                                  "  int2 q = (int2(p.xy) - c.xy) / max(c.z, 1) - int2(1, 1);\n"
                                  "  uint chars[5]; uint n = 0;\n"
                                  "  uint v = uint(c.w) / 10, t = uint(c.w) % 10;\n"
                                  "  if (v >= 100) chars[n++] = (v / 100) % 10;\n"
                                  "  if (v >= 10) chars[n++] = (v / 10) % 10;\n"
                                  "  chars[n++] = v % 10; chars[n++] = 10; chars[n++] = t;\n"
                                  "  float4 col = float4(0, 0, 0, 0.55);\n"
                                  "  if (q.x >= 0 && q.y >= 0 && q.y < 5) {\n"
                                  "    uint i = uint(q.x) / 4, x = uint(q.x) % 4;\n"
                                  "    if (i < n && x < 3 && ((kFont[chars[i]] >> (14 - (uint(q.y) * 3 + x))) & 1))\n"
                                  "      col = float4(1, 1, 1, 1);\n"
                                  "  }\n"
                                  "  return col;\n"
                                  "}\n";

    if (!compile(kVS, L"vs_6_0", blit_vs_) || !compile(kPS, L"ps_6_0", blit_ps_) || !compile(kPSText, L"ps_6_0", blit_ps_text_) || !compile(kPSMS, L"ps_6_0", blit_ps_ms_) || !compile(kPSFxaa, L"ps_6_0", blit_ps_fxaa_))
    {
      blit_root_.Reset();
      return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kRtRtvCount;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rt_rtv_heap_))))
      return false;

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = kRtDsvCount;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rt_dsv_heap_))))
      return false;

    dsv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    srv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
  }

  ID3D12PipelineState *Renderer::GetBlitPipeline(DXGI_FORMAT format, uint32_t kind)
  {
    const uint32_t pso_key = uint32_t(format) | (kind << 16);
    auto it = blit_pipelines_.find(pso_key);

    if (it != blit_pipelines_.end())
      return it->second.Get();

    const std::vector<uint8_t> &ps = kind == 1 ? blit_ps_ms_ : kind == 2 ? blit_ps_fxaa_
                                                           : kind == 3   ? blit_ps_text_
                                                                         : blit_ps_;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = blit_root_.Get();
    d.VS = {blit_vs_.data(), blit_vs_.size()};
    d.PS = {ps.data(), ps.size()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    if (kind == 3)
    {
      D3D12_RENDER_TARGET_BLEND_DESC &b = d.BlendState.RenderTarget[0];
      b.BlendEnable = TRUE;
      b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      b.BlendOp = D3D12_BLEND_OP_ADD;
      b.SrcBlendAlpha = D3D12_BLEND_ONE;
      b.DestBlendAlpha = D3D12_BLEND_ZERO;
      b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      b.LogicOp = D3D12_LOGIC_OP_NOOP;
    }

    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = format;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;

    if (FAILED(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))))
      pso = nullptr;

    return blit_pipelines_.emplace(pso_key, pso).first->second.Get();
  }

  void Renderer::Blit(uint32_t src_srv, int32_t src_x, int32_t src_y, D3D12_CPU_DESCRIPTOR_HANDLE rtv, DXGI_FORMAT format, const D3D12_RECT &dst, bool swap_rb, float gamma, uint32_t src_samples, bool fxaa)
  {
    // src_samples: bits 0-7 MSAA samples of the source, 0x100 = depth source
    const uint32_t samples = std::max(src_samples & 0xFF, 1u);
    const uint32_t kind = samples > 1 ? 1 : fxaa ? 2
                                                 : 0;
    ID3D12PipelineState *pso = GetBlitPipeline(format, kind);

    if (!pso || dst.right <= dst.left || dst.bottom <= dst.top)
      return;

    list_->SetGraphicsRootSignature(blit_root_.Get());
    list_->SetPipelineState(pso);
    int32_t gamma_bits = 0;

    if (gamma > 0 && gamma != 1.0f)
      std::memcpy(&gamma_bits, &gamma, 4);

    const int32_t flags = (swap_rb ? 1 : 0) | int32_t(samples << 8) | int32_t((src_samples & 0x100) << 8);
    const int32_t offset[8] = {src_x - dst.left, src_y - dst.top, flags, gamma_bits, src_x, src_y, src_x + (dst.right - dst.left), src_y + (dst.bottom - dst.top)};
    list_->SetGraphicsRoot32BitConstants(0, 8, offset, 0);
    list_->SetGraphicsRootDescriptorTable(1, SrvGpu(src_srv));
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // The triangle covers the whole viewport; viewport + scissor = destination
    const D3D12_VIEWPORT vp = {float(dst.left), float(dst.top), float(dst.right - dst.left), float(dst.bottom - dst.top), 0, 1};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &dst);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    NoteDraw(0, 1);
    list_->DrawInstanced(3, 1, 0, 0);
    RestoreDrawState();
  }

  void Renderer::RestoreDrawState()
  {
    list_->SetGraphicsRootSignature(root_signature_.Get());

    for (UINT i = 0; i < 3; ++i)
      list_->SetGraphicsRootDescriptorTable(kRootTextures2D + i, srv_heap_->GetGPUDescriptorHandleForHeapStart());

    list_->SetGraphicsRootDescriptorTable(kRootSamplers, sampler_heap_->GetGPUDescriptorHandleForHeapStart());
    bound_targets_ = 0;
  }

  // End of frame: show the game's front buffer (its last full-size resolve), or failing that the last colour target drawn to.
  void Renderer::PresentRenderTargets()
  {
    const D3D12_RECT full = Scaled({0, 0, LONG(width_), LONG(height_)}, output_scale_[target_]);

    // (Looked up by page: the texture may have been replaced since the resolve.)
    ResolvedTexture *present_source_ = nullptr;

    if (present_page_)
    {
      auto it = resolved_.find(present_page_);

      if (it != resolved_.end())
        present_source_ = it->second.get();
    }

    present_page_ = 0;

    if (present_source_ && present_source_->srv && !present_source_->volume)
    {
      ++stats_.presented_from_resolve;

      if (present_source_->state != kShaderRead)
      {
        auto b = Transition(present_source_->resource.Get(), present_source_->state, kShaderRead);
        list_->ResourceBarrier(1, &b);
        present_source_->state = kShaderRead;
      }

      Blit(present_source_->srv, 0, 0, Rtv(target_), D3D12Presenter::kGuestOutputFormat, full, present_source_->rb_swapped, gamma_);
    }
    else if (last_color_rt_ && last_color_rt_->srv)
    {
      SetRTState(*last_color_rt_, kShaderRead);
      Blit(last_color_rt_->srv, 0, 0, Rtv(target_), D3D12Presenter::kGuestOutputFormat, full, false, gamma_, last_color_rt_->samples);
      SetRTState(*last_color_rt_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    last_color_rt_ = nullptr;

    if (REXCVAR_GET(sst_fps_counter))
      DrawFpsCounter(full);
  }

  void Renderer::DrawFpsCounter(const D3D12_RECT &output)
  {
    // Rate of new frames handed to the display, updated twice a second
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const uint64_t now = uint64_t(qpc.QuadPart), frames = shown_frames_.load(std::memory_order_relaxed);

    static const double kFreq = []
    {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return double(f.QuadPart);
    }();

    if (!fps_last_tick_)
    {
      fps_last_tick_ = now;
      fps_last_frames_ = frames;
    }
    else if (double(now - fps_last_tick_) >= kFreq * 0.5)
    {
      const double fps = double(frames - fps_last_frames_) * kFreq / double(now - fps_last_tick_);
      fps_tenths_ = uint32_t(std::clamp(fps * 10.0 + 0.5, 0.0, 9999.0));
      fps_last_tick_ = now;
      fps_last_frames_ = frames;
    }

    ID3D12PipelineState *pso = GetBlitPipeline(D3D12Presenter::kGuestOutputFormat, 3);

    if (!pso)
      return;

    const LONG out_h = output.bottom - output.top;
    const int32_t px = std::max<int32_t>(2, int32_t(out_h / 240));
    const int32_t chars = fps_tenths_ >= 1000 ? 5 : fps_tenths_ >= 100 ? 4
                                                                       : 3;
    const D3D12_RECT box = {px * 4, px * 4, px * 4 + px * (chars * 4 + 1), px * 4 + px * 7};
    list_->SetGraphicsRootSignature(blit_root_.Get());
    list_->SetPipelineState(pso);
    const int32_t c[4] = {box.left, box.top, px, int32_t(fps_tenths_)};
    list_->SetGraphicsRoot32BitConstants(0, 4, c, 0);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(target_);
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp = {float(box.left), float(box.top), float(box.right - box.left), float(box.bottom - box.top), 0, 1};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &box);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    NoteDraw(0, 1);
    list_->DrawInstanced(3, 1, 0, 0);
    RestoreDrawState();
  }

  // VIDEO > ASPECT RATIO: 0 4:3, 1 16:9, 2 16:10, 3 21:9, 4 32:9. 4:3 is the game's own 4:3 mode (a 1280x1024 frame, from startup); 4:3 picked later waits for a restart (16:9 until then)
  int Renderer::DisplayAspect() const
  {
    int aspect = std::clamp(REXCVAR_GET(sst_aspect), 0, 4);

    if (height_ > 720)
      aspect = 0;
    else if (aspect == 0)
      aspect = 1;

    return aspect;
  }

  // How much wider than the game's frame the display is (1 in its 4:3 mode)
  float Renderer::DisplayStretch() const
  {
    static constexpr float kAspects[5] = {4.0f / 3.0f, 16.0f / 9.0f, 1.6f, 21.0f / 9.0f, 32.0f / 9.0f};
    return height_ > 720 ? 1.0f : kAspects[DisplayAspect()] / (16.0f / 9.0f);
  }

  ID3D12Resource *Renderer::Acquire(uint32_t *slot, uint32_t *width, uint32_t *height)
  {
    std::unique_lock lock(slot_mutex_);

    if (latest_ < 0)
      return nullptr;

    // Threaded: every frame is shown once, in order. The render thread finishes frame N-1 around the game's swap N; when it is a little late, wait for it. If it is later than that, the last frame is shown again and the late one next time - the queue then holds a frame in reserve, which absorbs the next late frame instead of dropping it. More than 2 waiting: catch up. VIDEO > FPS LIMIT 30: every other display frame repeats the last one; the others show the newest frame
    if (fps30_.load(std::memory_order_relaxed) && acquired_slot_ >= 0)
    {
      if (fps30_acquires_++ & 1)
      {
        ++acquires_;
        ++acquire_repeats_;
        pinned_ = acquired_slot_;
        *slot = uint32_t(acquired_slot_);
        *width = PxU(width_, output_scale_[acquired_slot_].x);
        *height = PxU(height_, output_scale_[acquired_slot_].y);
        return outputs_[acquired_slot_].Get();
      }
    }

    const bool queued = threaded_ && REXCVAR_GET(sst_native_present_queue) && !fps30_.load(std::memory_order_relaxed);
    uint64_t want = latest_seq_;

    if (queued && acquired_seq_)
    {
      want = acquired_seq_ + 1;

      if (latest_seq_ < want)
      {
        const int wait_ms = REXCVAR_GET(sst_native_present_wait_ms);

        if (wait_ms > 0)
        {
          frame_cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), [&]
                             { return latest_seq_ >= want; });
        }
      }

      if (latest_seq_ > acquired_seq_ + 2)
        want = latest_seq_ - 1;
    }

    int pick = -1;

    if (latest_seq_ >= want)
    {
      for (uint32_t i = 0; i < kOutputs; ++i)
      {
        if (output_seq_[i] >= want && output_seq_[i] <= latest_seq_ && (pick < 0 || output_seq_[i] < output_seq_[pick]))
          pick = int(i);
      }
    }

    ++acquires_;

    if (pick < 0)
    {
      // Nothing new: show the last frame again
      ++acquire_repeats_;
      pick = acquired_slot_ >= 0 ? acquired_slot_ : latest_;
    }
    else
    {
      if (acquired_seq_ && output_seq_[pick] > acquired_seq_ + 1)
        acquire_skips_ += output_seq_[pick] - acquired_seq_ - 1;

      acquired_seq_ = output_seq_[pick];
      acquired_slot_ = pick;
      shown_frames_.fetch_add(1, std::memory_order_relaxed);
    }

    pinned_ = pick;
    *slot = uint32_t(pick);
    *width = PxU(width_, output_scale_[pick].x);
    *height = PxU(height_, output_scale_[pick].y);
    return outputs_[pick].Get();
  }

  void Renderer::Release(uint32_t slot)
  {
    std::lock_guard lock(slot_mutex_);

    if (pinned_ == int(slot))
      pinned_ = -1;
  }
}