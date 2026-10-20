// Native renderer: blits, presenting frames, FPS counter

#include "gpu/native/renderer_internal.h"

namespace sst::native
{
  using namespace renderer_detail;

  // The blit layout (gfx::Layout::kBlit): b0 = 8 32-bit constants (source offset, flags, gamma; source rectangle for FXAA), t0 = source, s0 = linear clamp
  bool Renderer::CreateBlitResources()
  {
    static const char kVS[] = "float4 main(uint id : SV_VertexID) : SV_Position {\n"
                              "  float2 uv = float2((id << 1) & 2, id & 2);\n"
                              "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
                              "}\n";

    static const char kPS[] = "float4 main(float4 p : SV_Position) : SV_Target {\n"
                              "  float4 c = src.Load(int3(int2(p.xy) + offset.xy, 0));\n"
                              "  if (offset.z & 1) c = c.bgra;\n"
                              "  if (offset.w != 0) c.rgb = pow(saturate(c.rgb), asfloat(offset.w));\n"
                              "  return c;\n"
                              "}\n";

    // offset.z: bit 0 swap red / blue, bits 8-15 source samples, bit 16 depth
    static const char kPSMS[] = "float4 main(float4 p : SV_Position) : SV_Target {\n"
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
    static const char kPSFxaa[] = "static float2 lo_uv, hi_uv;\n"
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

    // The blit layout's bindings: 'offset' / 'rect' (the 8 constants), 'src' (the source: Texture2D, or Texture2DMS with BLIT_MS), 'lin' (linear clamp). D3D12: root constants b0, t0, static sampler s0. Vulkan: push constants (+ the source's and the sampler's slots in the bindless tables, sets 0 and 3)
    static const char kPrelude[] = "#ifdef __spirv__\n"
                                   "struct BlitConstants { int4 offset; int4 rect; uint src_index; uint smp_index; };\n"
                                   "[[vk::push_constant]] BlitConstants g_blit;\n"
                                   "#define offset g_blit.offset\n"
                                   "#define rect g_blit.rect\n"
                                   "#ifdef BLIT_MS\n"
                                   "[[vk::binding(0, 0)]] Texture2DMS<float4> g_textures[];\n"
                                   "#else\n"
                                   "[[vk::binding(0, 0)]] Texture2D<float4> g_textures[];\n"
                                   "#endif\n"
                                   "[[vk::binding(0, 3)]] SamplerState g_samplers[];\n"
                                   "#define src g_textures[g_blit.src_index]\n"
                                   "#define lin g_samplers[g_blit.smp_index]\n"
                                   "#else\n"
                                   "cbuffer C : register(b0) { int4 offset; int4 rect; };\n"
                                   "#ifdef BLIT_MS\n"
                                   "Texture2DMS<float4> src : register(t0);\n"
                                   "#else\n"
                                   "Texture2D<float4> src : register(t0);\n"
                                   "#endif\n"
                                   "SamplerState lin : register(s0);\n"
                                   "#endif\n";

    auto compile = [&](const char *source, const wchar_t *profile, std::vector<uint8_t> &out, bool ms = false)
    {
      const bool pixel = profile[0] == L'p';
      const std::string full = pixel ? std::string(ms ? "#define BLIT_MS\n" : "") + kPrelude + source : std::string(source);
      return CompileRendererShader(full.c_str(), profile, out);
    };

    // FPS counter: offset.xy = top-left (host pixels), offset.z = font pixel size, offset.w = frames per second x 10. 3x5 digits, drawn on a dark box
    static const char kPSText[] = "static const uint kFont[11] = {0x7b6f, 0x2c97, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef,\n"
                                  "                               0x7249, 0x7bef, 0x7bcf, 0x2};\n"
                                  "float4 main(float4 p : SV_Position) : SV_Target {\n"
                                  "  int2 q = (int2(p.xy) - offset.xy) / max(offset.z, 1) - int2(1, 1);\n"
                                  "  uint chars[5]; uint n = 0;\n"
                                  "  uint v = uint(offset.w) / 10, t = uint(offset.w) % 10;\n"
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

    blit_ready_ = compile(kVS, L"vs_6_0", blit_vs_) && compile(kPS, L"ps_6_0", blit_ps_) && compile(kPSText, L"ps_6_0", blit_ps_text_) && compile(kPSMS, L"ps_6_0", blit_ps_ms_, true) && compile(kPSFxaa, L"ps_6_0", blit_ps_fxaa_);
    return blit_ready_;
  }

  gfx::Pipeline *Renderer::GetBlitPipeline(DXGI_FORMAT format, uint32_t kind)
  {
    const uint32_t pso_key = uint32_t(format) | (kind << 16);
    auto it = blit_pipelines_.find(pso_key);

    if (it != blit_pipelines_.end())
      return it->second.get();

    const std::vector<uint8_t> &ps = kind == 1 ? blit_ps_ms_ : kind == 2 ? blit_ps_fxaa_
                                                           : kind == 3   ? blit_ps_text_
                                                                         : blit_ps_;
    gfx::PipelineDesc d;
    d.layout = gfx::Layout::kBlit;
    d.vs = {blit_vs_.data(), blit_vs_.size(), 0};
    d.ps = {ps.data(), ps.size(), 0};
    d.blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    if (kind == 3)
    {
      D3D12_RENDER_TARGET_BLEND_DESC &b = d.blend.RenderTarget[0];
      b.BlendEnable = TRUE;
      b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      b.BlendOp = D3D12_BLEND_OP_ADD;
      b.SrcBlendAlpha = D3D12_BLEND_ONE;
      b.DestBlendAlpha = D3D12_BLEND_ZERO;
      b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      b.LogicOp = D3D12_LOGIC_OP_NOOP;
    }

    d.raster.FillMode = D3D12_FILL_MODE_SOLID;
    d.raster.CullMode = D3D12_CULL_MODE_NONE;
    d.raster.DepthClipEnable = TRUE;
    d.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.rtv = format;
    d.samples = 1;
    gfx::PipelineRef pso = gfx_->CreatePipeline(d);
    return blit_pipelines_.emplace(pso_key, pso).first->second.get();
  }

  void Renderer::Blit(uint32_t src_srv, int32_t src_x, int32_t src_y, uint32_t rtv, DXGI_FORMAT format, const D3D12_RECT &dst, bool swap_rb, float gamma, uint32_t src_samples, bool fxaa)
  {
    // src_samples: bits 0-7 MSAA samples of the source, 0x100 = depth source
    const uint32_t samples = std::max(src_samples & 0xFF, 1u);
    const uint32_t kind = samples > 1 ? 1 : fxaa ? 2
                                                 : 0;
    gfx::Pipeline *pso = GetBlitPipeline(format, kind);

    if (!pso || dst.right <= dst.left || dst.bottom <= dst.top)
      return;

    list_->SetLayout(gfx::Layout::kBlit);
    list_->SetPipeline(*pso);
    int32_t gamma_bits = 0;

    if (gamma > 0 && gamma != 1.0f)
      std::memcpy(&gamma_bits, &gamma, 4);

    const int32_t flags = (swap_rb ? 1 : 0) | int32_t(samples << 8) | int32_t((src_samples & 0x100) << 8);
    const int32_t offset[8] = {int32_t(src_x - dst.left), int32_t(src_y - dst.top), flags, gamma_bits, src_x, src_y, int32_t(src_x + (dst.right - dst.left)), int32_t(src_y + (dst.bottom - dst.top))};
    list_->SetBlitConstants(offset, 8);
    list_->SetBlitSource(src_srv);
    list_->SetTargets(rtv, gfx::CommandList::kNoView);

    // The triangle covers the whole viewport; viewport + scissor = destination
    const D3D12_VIEWPORT vp = {float(dst.left), float(dst.top), float(dst.right - dst.left), float(dst.bottom - dst.top), 0, 1};
    list_->SetViewport(vp);
    list_->SetScissor(dst);
    list_->SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    NoteDraw(0, 1);
    list_->Draw(3, 0);
    RestoreDrawState();
  }

  void Renderer::RestoreDrawState()
  {
    list_->SetLayout(gfx::Layout::kDraw);
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
        list_->Barrier(*present_source_->resource, present_source_->state, kShaderRead);
        present_source_->state = kShaderRead;
      }

      Blit(present_source_->srv, 0, 0, Rtv(target_), kOutputFormat, full, present_source_->rb_swapped, gamma_);
    }
    else if (last_color_rt_ && last_color_rt_->srv)
    {
      SetRTState(*last_color_rt_, kShaderRead);
      Blit(last_color_rt_->srv, 0, 0, Rtv(target_), kOutputFormat, full, false, gamma_, last_color_rt_->samples);
      SetRTState(*last_color_rt_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    last_color_rt_ = nullptr;

    if (REXCVAR_GET(sst_fps_counter))
      DrawFpsCounter(full);
  }

  void Renderer::DrawFpsCounter(const D3D12_RECT &output)
  {
    // Rate of new frames handed to the display, updated twice a second
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t frames = shown_frames_.load(std::memory_order_relaxed);
    constexpr double kFreq = 1e9;

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

    gfx::Pipeline *pso = GetBlitPipeline(kOutputFormat, 3);

    if (!pso)
      return;

    const LONG out_h = output.bottom - output.top;
    const int32_t px = std::max<int32_t>(2, int32_t(out_h / 240));
    const int32_t chars = fps_tenths_ >= 1000 ? 5 : fps_tenths_ >= 100 ? 4
                                                                       : 3;
    const D3D12_RECT box = {px * 4, px * 4, px * 4 + px * (chars * 4 + 1), px * 4 + px * 7};
    list_->SetLayout(gfx::Layout::kBlit);
    list_->SetPipeline(*pso);
    const int32_t c[4] = {int32_t(box.left), int32_t(box.top), px, int32_t(fps_tenths_)};
    list_->SetBlitConstants(c, 4);
    list_->SetTargets(Rtv(target_), gfx::CommandList::kNoView);
    const D3D12_VIEWPORT vp = {float(box.left), float(box.top), float(box.right - box.left), float(box.bottom - box.top), 0, 1};
    list_->SetViewport(vp);
    list_->SetScissor(box);
    list_->SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    NoteDraw(0, 1);
    list_->Draw(3, 0);
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

  gfx::Texture *Renderer::Acquire(uint32_t *slot, uint32_t *width, uint32_t *height)
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
        return outputs_[acquired_slot_].get();
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
    return outputs_[pick].get();
  }

  void Renderer::Release(uint32_t slot)
  {
    std::lock_guard lock(slot_mutex_);

    if (pinned_ == int(slot))
      pinned_ = -1;
  }
}