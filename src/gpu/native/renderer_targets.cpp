// Native renderer: render targets, clears and resolves

#include "gpu/native/renderer_internal.h"

namespace sst::native
{
  using namespace renderer_detail;

  bool Renderer::RenderTargetsEnabled() const
  {
    return REXCVAR_GET(sst_native_render_targets) && blit_ready_;
  }

  uint32_t Renderer::AllocateRtSrv()
  {
    if (rt_srv_next_ < kRtSrvFirst)
      rt_srv_next_ = kRtSrvFirst;

    return rt_srv_next_ < kSrvReserved ? rt_srv_next_++ : 0;
  }

  void Renderer::SetRTState(HostRT &rt, D3D12_RESOURCE_STATES state)
  {
    if (rt.state == state)
      return;

    list_->Barrier(*rt.resource, rt.state, state);
    rt.state = state;
  }

  Renderer::HostRT *Renderer::GetHostRT(const uint8_t *base, uint32_t surface, bool depth, Scale2 scale, uint32_t samples, uint32_t min_width, uint32_t min_height)
  {
    if (!PlausibleAddress(surface))
      return nullptr;

    auto &map = depth ? depth_rts_ : color_rts_;

    // Logical size: the surface's own size (size word at +0x24) or what is drawn into it, whichever is larger
    const uint32_t size = be32(base, surface + 0x24);
    const uint32_t sw = (size >> 18) + 1, sh = ((size >> 3) & 0x7FFF) + 1;
    const uint32_t w = std::clamp(std::max(sw, min_width), 1u, 4096u);
    const uint32_t h = std::clamp(std::max(sh, min_height), 1u, 4096u);
    const uint32_t guest_format = (be32(base, surface + 0x1C) >> 16) & 0xF;

    const bool frame_16 = !depth && guest_format == 0 && samples == 1 && sw >= width_ && sh >= height_ && REXCVAR_GET(sst_native_frame_16bit) && Frame16Supported();
    const DXGI_FORMAT color_format = frame_16 ? DXGI_FORMAT_R16G16B16A16_UNORM : ColorTargetFormat(guest_format);
    samples = UsableSamples(depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : color_format, samples);
    auto it = map.find(surface);

    if (it != map.end())
    {
      HostRT &rt = it->second;

      if (rt.width >= w && rt.height >= h && rt.scale == scale && rt.samples == samples && (depth || (rt.guest_format == guest_format && rt.format == color_format)))
        return &rt;

      // Grows (or changes format): make a new one; the old one may still be in use by submitted frames, so keep it alive until the GPU is done
      if (!depth && rt.guest_format != guest_format && LogOnce(0xA800000000000000ull ^ (uint64_t(surface) << 8) ^ guest_format))
        REXLOG_INFO("[sst-native] render target {:08X} changed format {} -> {}", surface, rt.guest_format, guest_format);

      retired_rts_.push_back({gfx_->LastSubmitted() + 1, rt.resource});
    }

    HostRT rt;
    rt.depth = depth;
    rt.width = std::max(w, it != map.end() ? it->second.width : 0u);
    rt.height = std::max(h, it != map.end() ? it->second.height : 0u);
    rt.guest_format = guest_format;
    rt.scale = scale;
    rt.samples = samples;
    rt.format = depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : color_format;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = PxU(rt.width, scale.x);
    desc.Height = PxU(rt.height, scale.y);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = depth ? DXGI_FORMAT_R32G8X24_TYPELESS : rt.format;
    desc.SampleDesc.Count = samples;
    desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = rt.format;
    rt.state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt.resource = gfx_->CreateTexture(desc, rt.state, &clear);

    if (!rt.resource)
    {
      if (LogOnce(0xA900000000000000ull ^ surface))
        REXLOG_WARN("[sst-native] could not create the {}x{} {} target for surface {:08X}", rt.width, rt.height, depth ? "depth" : "colour", surface);

      return nullptr;
    }

    {
      rt.resource->SetName(fmt::format("sst {} target {:08X} {}x{}", depth ? "depth" : "colour", surface, rt.width, rt.height));
    }

    // Views: reuse the slots of the target being replaced
    if (it != map.end())
    {
      rt.view = it->second.view;
      rt.srv = it->second.srv;
    }
    else
    {
      rt.view = depth ? rt_dsv_next_++ : rt_rtv_next_++;
      rt.srv = AllocateRtSrv();

      if ((depth && rt.view >= kRtDsvCount) || (!depth && rt.view >= kRtRtvCount) || !rt.srv)
      {
        if (LogOnce(0xAA00000000000000ull))
          REXLOG_WARN("[sst-native] out of render-target descriptors");

        return nullptr;
      }
    }

    if (depth)
    {
      D3D12_DEPTH_STENCIL_VIEW_DESC dd = {};
      dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
      dd.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
      gfx_->WriteDsv(RtDsv(rt.view), *rt.resource, &dd);
    }
    else
    {
      gfx_->WriteRtv(RtRtv(rt.view), *rt.resource, nullptr);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : rt.format;
    sd.ViewDimension = samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    gfx_->WriteSrv(rt.srv, *rt.resource, sd);
    ++stats_.host_rts;
    REXLOG_INFO("[sst-native] render target {:08X}: {}x{} {}{} (surface {}x{}, guest format {}){}", surface, rt.width, rt.height, depth ? "depth D32S8" : "colour", samples > 1 ? fmt::format(" {}x MSAA", samples) : std::string(), sw, sh, guest_format, scale != Scale2{} ? fmt::format(", host {}x{}", PxU(rt.width, scale.x), PxU(rt.height, scale.y)) : std::string());
    bound_targets_ = 0;
    HostRT &stored = map[surface] = std::move(rt);

    if (!recording_)
      return &stored;

    // New contents are undefined: start from zero like EDRAM after a clear
    if (depth)
    {
      list_->ClearDsv(RtDsv(stored.view), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0);
    }
    else
    {
      static constexpr float kZero[4] = {};
      list_->ClearRtv(RtRtv(stored.view), kZero);
    }

    return &stored;
  }

  Renderer::ResolvedTexture *Renderer::GetResolvedTexture(uint32_t page, uint32_t guest_format, uint32_t width, uint32_t height, uint32_t slices, bool volume, Scale2 scale, bool keep_16bit)
  {
    const DXGI_FORMAT guest_host_format = ResolveTextureFormat(guest_format);
    const DXGI_FORMAT format = keep_16bit && guest_host_format == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R16G16B16A16_UNORM : guest_host_format;
    auto it = resolved_.find(page);

    if (it != resolved_.end())
    {
      ResolvedTexture &t = *it->second;

      if (t.format == format && t.width == width && t.height == height && t.slices == slices && t.volume == volume && t.scale == scale)
        return &t;

      textures_->RemoveExternal(page);
      retired_rts_.push_back({gfx_->LastSubmitted() + 1, t.resource});
    }

    auto t = std::make_unique<ResolvedTexture>();
    t->format = format;
    t->width = width;
    t->height = height;
    t->slices = slices;
    t->volume = volume;
    t->guest_format = guest_format;
    t->scale = scale;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = PxU(width, scale.x);
    desc.Height = PxU(height, scale.y);
    desc.DepthOrArraySize = UINT16(slices);
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    t->state = kShaderRead;
    t->resource = gfx_->CreateTexture(desc, t->state);

    if (!t->resource)
    {
      if (LogOnce(0xAB00000000000000ull ^ page))
        REXLOG_WARN("[sst-native] could not create the {}x{}x{} resolve texture at {:08X}", width, height, slices, page << 12);

      resolved_.erase(page);
      return nullptr;
    }

    {
      t->resource->SetName(fmt::format("sst resolve texture {:08X} {}x{}x{}", page << 12, width, height, slices));
    }

    // Views: one RTV per slice, an identity SRV (presenting). Reuse slots
    if (it != resolved_.end())
    {
      t->rtvs = it->second->rtvs;
      t->srv = it->second->srv;
    }

    while (t->rtvs.size() < slices)
    {
      if (rt_rtv_next_ >= kRtRtvCount)
        return nullptr;

      t->rtvs.push_back(rt_rtv_next_++);
    }

    if (!t->srv)
      t->srv = AllocateRtSrv();

    for (uint32_t i = 0; i < slices; ++i)
    {
      D3D12_RENDER_TARGET_VIEW_DESC rd = {};
      rd.Format = format;

      if (volume)
      {
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        rd.Texture3D.FirstWSlice = i;
        rd.Texture3D.WSize = 1;
      }
      else
      {
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      }

      gfx_->WriteRtv(RtRtv(t->rtvs[i]), *t->resource, &rd);
    }

    if (t->srv)
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = format;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

      if (volume)
      {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D.MipLevels = 1;
      }
      else
      {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
      }

      gfx_->WriteSrv(t->srv, *t->resource, sd);
    }

    TextureCache::External e;
    e.resource = t->resource.get();
    e.dxgi_format = uint32_t(format);
    e.width = width;
    e.height = height;
    e.slices = slices;
    e.volume = volume;
    textures_->SetExternal(page, e);
    REXLOG_INFO("[sst-native] resolve texture {:08X}: {}x{}{} guest format {} -> DXGI {}", page << 12, width, height, slices > 1 ? fmt::format("x{}", slices) : "", guest_format, uint32_t(format));
    ResolvedTexture *raw = t.get();
    resolved_[page] = std::move(t);
    return raw;
  }

  void Renderer::Resolve(const uint8_t *base, const ResolveCall &c)
  {
    ++stats_.resolves;
    const uint32_t dev = c.device;

    if (!PlausibleAddress(dev))
      return;

    // Destination texture (its fetch constant at +0x1C) and the copy rectangle
    uint32_t fetch[6] = {};

    if (PlausibleAddress(c.dst_texture))
    {
      for (uint32_t k = 0; k < 6; ++k)
        fetch[k] = be32(base, c.dst_texture + 0x1C + 4 * k);
    }

    int32_t rect[4] = {-1, -1, -1, -1}, point[2] = {-1, -1};

    if (PlausibleAddress(c.src_rect))
    {
      for (int i = 0; i < 4; ++i)
        rect[i] = int32_t(be32(base, c.src_rect + 4 * i));
    }

    if (PlausibleAddress(c.dst_point))
    {
      for (int i = 0; i < 2; ++i)
        point[i] = int32_t(be32(base, c.dst_point + 4 * i));
    }

    const uint32_t which = c.flags & 7; // 0-3 colour target, 4 depth
    const uint32_t surface = which == 4 ? be32(base, dev + kDevDepth) : be32(base, dev + kDevRT0 + 4 * (which & 3));
    std::string colour = "none";

    if (PlausibleAddress(c.clear_color))
      colour = fmt::format("{} {} {} {}", BeFloat(base, c.clear_color), BeFloat(base, c.clear_color + 4), BeFloat(base, c.clear_color + 8), BeFloat(base, c.clear_color + 12));

    const std::string line = fmt::format("resolve flags {:X} from {} rect {} {} {} {} -> texture {:08X} {} at {} {} level {} slice {} clear colour {} z {}",
                                         c.flags, which == 4 ? "depth" : fmt::format("RT{}", which & 3), rect[0], rect[1], rect[2], rect[3], c.dst_texture, fetch[0] ? DescribeTexture(fetch) : std::string("(none)"), point[0], point[1], c.level, c.slice, colour, c.clear_z);

    if (rt_trace_.active)
    {
      TraceState(base, dev);
      rt_trace_.lines.push_back("  " + line);
      rt_trace_.lines.push_back("     source surface " + DescribeSurface(base, surface));
    }

    if (capture_active_)
      REXLOG_INFO("[sst-native] capture (before draw {}): {} source {:08X}{}", draw_seq_ + 1, line, surface, scene_surfaces_.count(surface) ? " (scene)" : "");
    else if (LogOnce(0xA700000000000000ull ^ (uint64_t(c.flags) << 40) ^ (fetch[1] & ~0xFFFu) ^ (uint64_t(surface) << 8)) && stats_.resolves_logged++ < 40)
      REXLOG_INFO("[sst-native] {}", line);

    // the copy
    if (!RenderTargetsEnabled())
      return;

    if (!recording_)
      BeginFrame();

    const bool depth = which == 4;
    auto &map = depth ? depth_rts_ : color_rts_;
    auto src_it = map.find(surface);

    if (src_it == map.end() || !src_it->second.srv || (fetch[0] & 3) != 2 || c.level != 0)
    {
      ++stats_.resolves_skipped;

      if (LogOnce(0xAD00000000000000ull ^ (uint64_t(c.flags) << 32) ^ surface))
      {
        REXLOG_INFO("[sst-native] resolve skipped ({}): {}", src_it == map.end() ? "nothing drawn to the source yet" : c.level != 0 ? "mip level"
                                                                                                                                    : "destination",
                    line);
      }

      return;
    }

    HostRT &src = src_it->second;
    const bool stacked = (fetch[1] >> 10) & 1;
    const uint32_t dw = (fetch[2] & 0x1FFF) + 1, dh = ((fetch[2] >> 13) & 0x1FFF) + 1;
    const uint32_t slices = stacked ? (fetch[2] >> 26) + 1 : 1;
    const uint32_t page = (fetch[1] >> 12) & 0x1FFFF;

    // The shadow map: its depth is resolved into a stacked (3D) texture. Its targets get the shadow map scale from the next frame on
    if (stacked && depth && shadow_surfaces_.insert(surface).second)
    {
      shadow_surfaces_.insert(be32(base, dev + kDevRT0));
      REXLOG_INFO("[sst-native] shadow map surfaces: depth {:08X}, colour {:08X}", surface, be32(base, dev + kDevRT0));
    }

    // final smoothed bloom image
    const bool bloom_src = !depth && !stacked && BloomSurface(src, surface);
    bool bloom_final = false;

    if (bloom_src)
    {
      if (!bloom_small_seen_ && bloom_early_pages_.insert(page).second && bloom_final_pages_.erase(page))
        REXLOG_INFO("[sst-native] bloom: {:08X} is also an input of the chain, not its final image", page << 12);

      if (dw * 16 <= width_)
        bloom_small_seen_ = true;
      else if (bloom_small_seen_ && dw * 4 >= width_ && !bloom_early_pages_.count(page) && bloom_final_pages_.insert(page).second)
        REXLOG_INFO("[sst-native] bloom: final image at {:08X} ({}x{}), stored at the frame's size with a smooth upscale", page << 12, dw, dh);

      bloom_final = REXCVAR_GET(sst_native_bloom_smooth) && bloom_final_pages_.count(page) && dw && dh;
    }

    const Scale2 dst_scale = bloom_final ? Scale2{scale_.x * float(width_) / float(dw), scale_.y * float(height_) / float(dh)} : src.scale;
    ResolvedTexture *dst = GetResolvedTexture(page, fetch[1] & 0x3F, dw, dh, slices, stacked, dst_scale, !depth && src.format == DXGI_FORMAT_R16G16B16A16_UNORM);

    // Full-screen images (the 3D scene, the glow chain, frame copies): draws that sample them are composites, not UI (aspect ratio fit)
    {
      const int32_t rw = (rect[0] >= 0 ? rect[2] - rect[0] : int32_t(dw));
      const bool full = !depth && !stacked && (scene_surfaces_.count(surface) || BloomSurface(src, surface) || rw * 10 >= int32_t(width_) * 9);

      if (full)
        composite_pages_.insert(page);
      else
        composite_pages_.erase(page);
    }

    if (!dst)
    {
      ++stats_.resolves_skipped;
      return;
    }

    // Source rectangle (default: the destination's size) and destination point
    const int32_t sx0 = rect[0] >= 0 ? rect[0] : 0, sy0 = rect[1] >= 0 ? rect[1] : 0;
    const int32_t sx1 = rect[0] >= 0 ? rect[2] : int32_t(dw), sy1 = rect[1] >= 0 ? rect[3] : int32_t(dh);
    const int32_t px = point[0] >= 0 ? point[0] : 0, py = point[1] >= 0 ? point[1] : 0;
    D3D12_RECT dr = {std::max<LONG>(px, 0), std::max<LONG>(py, 0), std::min<LONG>(px + (sx1 - sx0), LONG(dw)), std::min<LONG>(py + (sy1 - sy0), LONG(dh))};
    const uint32_t slice = std::min(c.slice, slices - 1);
    SetRTState(src, kShaderRead);

    if (dst->state != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
      list_->Barrier(*dst->resource, dst->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      dst->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    // Colour resolves into textures whose fetch swizzle reads blue from data component x (ARGB-style formats: swizzle zyxw) are stored red/blue swapped, as the Xbox's resolve does for them; the sampling swizzle then swaps them back
    const uint32_t dst_swizzle = (fetch[3] >> 1) & 0xFFF;
    const bool swap_rb = !depth && (dst_swizzle & 7) == 2 && ((dst_swizzle >> 6) & 7) == 0;
    dst->rb_swapped = swap_rb;

    if (bloom_final)
    {
      const Scale2 k = src.scale;
      const D3D12_RECT src_host = {Px(sx0 + (dr.left - px), k.x), Px(sy0 + (dr.top - py), k.y), Px(sx0 + (dr.right - px), k.x), Px(sy0 + (dr.bottom - py), k.y)};
      BlitSmooth(src.srv, src_host, RtRtv(dst->rtvs[slice]), dst->format, Scaled(dr, dst_scale), swap_rb);
    }
    else
    {
      const Scale2 k = src.scale;
      const D3D12_RECT host = Scaled(dr, k);

      // FXAA (VIDEO > ANTI-ALIASING): applied to the 3D scene's colour copies, before the UI is drawn over the final image
      const bool fxaa = fxaa_ && !depth && !stacked && src.samples == 1 && scene_surfaces_.count(surface);
      Blit(src.srv, Px(sx0 + (dr.left - px), k.x), Px(sy0 + (dr.top - py), k.y), RtRtv(dst->rtvs[slice]), dst->format, host, swap_rb, 0.0f, src.samples | (depth ? 0x100u : 0u), fxaa);
    }

    {
      list_->Barrier(*dst->resource, D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderRead);
      dst->state = kShaderRead;
    }

    SetRTState(src, depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
    ++stats_.resolves_done;

    // Clears after the copy: 0x100 the colour target, 0x200 depth / stencil
    const D3D12_RECT sr = {std::max(sx0, 0), std::max(sy0, 0), std::min<LONG>(sx1, src.width), std::min<LONG>(sy1, src.height)};

    if (sr.right > sr.left && sr.bottom > sr.top)
    {
      if ((c.flags & 0x100) && !depth)
      {
        float col[4] = {0, 0, 0, 0};

        if (PlausibleAddress(c.clear_color))
        {
          for (int i = 0; i < 4; ++i)
            col[i] = BeFloat(base, c.clear_color + 4 * i);
        }

        const D3D12_RECT host = Scaled(sr, src.scale);
        list_->ClearRtv(RtRtv(src.view), col, &host, 1);
      }

      if (c.flags & 0x200)
      {
        const uint32_t ds = be32(base, dev + kDevDepth);
        auto dit = depth_rts_.find(ds);

        if (dit != depth_rts_.end())
        {
          HostRT &d = dit->second;
          SetRTState(d, D3D12_RESOURCE_STATE_DEPTH_WRITE);
          const D3D12_RECT dsr = {sr.left, sr.top, std::min<LONG>(sr.right, d.width), std::min<LONG>(sr.bottom, d.height)};
          const D3D12_RECT host = Scaled(dsr, d.scale);

          if (dsr.right > dsr.left && dsr.bottom > dsr.top)
            list_->ClearDsv(RtDsv(d.view), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, std::clamp(c.clear_z, 0.0f, 1.0f), 0, &host, 1);
        }
      }
    }

    // The game's front buffer copy: the last full-size colour resolve
    if (!depth && !stacked && dw == width_ && dh == height_)
      present_page_ = page;
  }

  // frame

  uint32_t Renderer::UsableSamples(DXGI_FORMAT format, uint32_t samples)
  {
    while (samples > 1)
    {
      const uint32_t key = (uint32_t(format) << 8) | samples;
      auto it = msaa_support_.find(key);

      if (it == msaa_support_.end())
      {
        const bool ok = gfx_->SupportsSamples(format, samples);
        it = msaa_support_.emplace(key, ok ? 1u : 0u).first;

        if (!ok)
          REXLOG_WARN("[sst-native] {}x MSAA not supported for DXGI format {}", samples, uint32_t(format));
      }

      if (it->second)
        return samples;

      samples /= 2;
    }

    return 1;
  }

  bool Renderer::Frame16Supported()
  {
    if (frame16_supported_ < 0)
    {
      frame16_supported_ = gfx_->SupportsSamples(DXGI_FORMAT_R16G16B16A16_UNORM, 1) ? 1 : 0;

      if (!frame16_supported_)
        REXLOG_INFO("[sst-native] the GPU can't render to 16-bit UNORM targets: the final image stays 8-bit");
    }

    return frame16_supported_ == 1;
  }

  // The quarter-size glow / bloom chain (320x180 down to 20x11, 16:9 targets that are neither the 3D scene nor the shadow map)
  bool Renderer::BloomSurface(const HostRT &rt, uint32_t surface) const
  {
    if (rt.depth || scene_surfaces_.count(surface) || shadow_surfaces_.count(surface))
      return false;

    return BloomShape(rt.width, rt.height);
  }

  // A guest size in the bloom's range, shaped like the frame
  bool Renderer::BloomShape(uint32_t width, uint32_t height) const
  {
    if (width > 320 || width < 16)
      return false;

    const int64_t aspect = int64_t(width) * height_ - int64_t(height) * width_;
    return std::abs(aspect) <= int64_t(width_) * 2;
  }

  // Which scale a guest surface (and the depth buffer drawn with it) renders at: the shadow map's, the 3D scene's (surfaces drawn with MSAA, RB_SURFACE_INFO bits 16-17), or the UI / final image's. Remembered per surface so a target keeps its size between passes
  Scale2 Renderer::SurfaceScale(const uint8_t *base, uint32_t dev, uint32_t color, uint32_t depth)
  {
    if ((color && shadow_surfaces_.count(color)) || (depth && shadow_surfaces_.count(depth)))
      return shadow_scale_;

    const bool msaa = (be32(base, dev + 0x2880) >> 16) & 3;

    if (msaa)
    {
      if (color && scene_surfaces_.insert(color).second)
        REXLOG_INFO("[sst-native] 3D scene surface {:08X}", color);

      if (depth)
        scene_surfaces_.insert(depth);
    }

    if ((color && scene_surfaces_.count(color)) || (depth && scene_surfaces_.count(depth)))
      return scene_scale_;

    if (color && !msaa && PlausibleAddress(color))
    {
      const uint32_t size = be32(base, color + 0x24);

      if (BloomShape((size >> 18) + 1, ((size >> 3) & 0x7FFF) + 1))
        return bloom_scale_;
    }

    return scale_;
  }

  // The output this frame renders into (and the output depth buffer) at the current scale; the old textures are released once the GPU is done with them. Called with slot_mutex_ held (Acquire reads output_scale_)
  bool Renderer::ApplyScale()
  {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = PxU(width_, scale_.x);
    desc.Height = PxU(height_, scale_.y);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;

    if (output_scale_[target_] != scale_)
    {
      desc.Format = kOutputFormat;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      gfx::TextureRef output = gfx_->CreateTexture(desc, D3D12_RESOURCE_STATE_COPY_SOURCE);

      if (!output)
      {
        REXLOG_ERROR("[sst-native] could not create a {}x{} output", desc.Width, desc.Height);
        scale_ = output_scale_[target_];
        return false;
      }

      retired_rts_.push_back({gfx_->LastSubmitted() + 1, outputs_[target_]});
      outputs_[target_] = output;
      gfx_->WriteRtv(Rtv(target_), *output, nullptr);
      output_scale_[target_] = scale_;
    }

    if (depth_scale_ != scale_)
    {
      desc.Format = kDepthFormat;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
      D3D12_CLEAR_VALUE clear = {};
      clear.Format = kDepthFormat;
      clear.DepthStencil.Depth = 1.0f;

      if (gfx::TextureRef depth = gfx_->CreateTexture(desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear))
      {
        retired_rts_.push_back({gfx_->LastSubmitted() + 1, depth_});
        depth_ = depth;
        gfx_->WriteDsv(kOutputDsv, *depth_, nullptr);
        depth_scale_ = scale_;
      }
    }

    return true;
  }

  // clear

  void Renderer::Clear(const uint8_t *base, uint32_t device, uint32_t flags, uint32_t color, float z, uint32_t stencil)
  {
    PhaseClock clk(prof_, kPhClear);

    if (rt_trace_.active)
    {
      TraceState(base, device);
      rt_trace_.lines.push_back(fmt::format("  clear flags {:X} colour {:08X} z {} stencil {}", flags, color, z, stencil));
    }

    ++stats_.clears;
    ++stats_.clear_flags[flags];

    if (!recording_)
      BeginFrame();

    const uint32_t rt0 = be32(base, device + kDevRT0);

    if (RenderTargetsEnabled())
    {
      // Xbox D3DCLEAR_TARGET0-3 = 0x1-0x8, ZBUFFER 0x10, STENCIL 0x20 (Clear drops 0xF0 when no depth surface is set). The area is viewport (+ scissor), which PA_SC_WINDOW_SCISSOR holds
      const uint32_t tl = be32(base, device + kDevScissorTL), br = be32(base, device + kDevScissorBR);
      const LONG x0 = LONG(tl & 0x7FFF), y0 = LONG((tl >> 16) & 0x7FFF);
      const LONG x1 = LONG(br & 0x7FFF), y1 = LONG((br >> 16) & 0x7FFF);
      uint32_t vw = 0, vh = 0;

      if (Reg(base, device, PA_CL_VTE_CNTL) & 1)
      {
        const float xs = std::fabs(RegF(base, device, PA_CL_VPORT_XSCALE));
        const float ys = std::fabs(RegF(base, device, PA_CL_VPORT_YSCALE));
        const float xo = RegF(base, device, PA_CL_VPORT_XOFFSET);
        const float yo = RegF(base, device, PA_CL_VPORT_YOFFSET);

        if (std::isfinite(xs + ys + xo + yo) && xs < 8192 && ys < 8192)
        {
          vw = uint32_t(std::max(0.0f, std::ceil(xo + xs)));
          vh = uint32_t(std::max(0.0f, std::ceil(yo + ys)));
        }
      }

      const uint32_t clear_ds = be32(base, device + kDevDepth);
      const Scale2 rs = SurfaceScale(base, device, rt0, clear_ds);
      const uint32_t samples = (scene_surfaces_.count(rt0) || scene_surfaces_.count(clear_ds)) ? msaa_ : 1;

      auto area = [&](const HostRT &rt)
      {
        D3D12_RECT r = {0, 0, LONG(rt.width), LONG(rt.height)};

        if (x1 > x0 && y1 > y0)
          r = {std::min(x0, r.right), std::min(y0, r.bottom), std::min(x1, r.right), std::min(y1, r.bottom)};

        return Scaled(r, rt.scale);
      };

      if ((flags & 0xF) && rt0)
      {
        if (HostRT *c = GetHostRT(base, rt0, false, rs, samples, vw, vh))
        {
          SetRTState(*c, D3D12_RESOURCE_STATE_RENDER_TARGET);
          float col[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f, float(color & 0xFF) / 255.0f, float((color >> 24) & 0xFF) / 255.0f};

          if (bloom_off_ && BloomSurface(*c, rt0))
            col[0] = col[1] = col[2] = col[3] = 0;

          const D3D12_RECT r = area(*c);

          if (r.right > r.left && r.bottom > r.top)
            list_->ClearRtv(RtRtv(c->view), col, &r, 1);
        }
      }

      const uint32_t ds = be32(base, device + kDevDepth);

      if ((flags & 0x30) && ds)
      {
        if (HostRT *d = GetHostRT(base, ds, true, rs, samples, vw, vh))
        {
          SetRTState(*d, D3D12_RESOURCE_STATE_DEPTH_WRITE);
          D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(0);

          if (flags & 0x10)
            cf |= D3D12_CLEAR_FLAG_DEPTH;

          if (flags & 0x20)
            cf |= D3D12_CLEAR_FLAG_STENCIL;

          const D3D12_RECT r = area(*d);

          if (r.right > r.left && r.bottom > r.top)
            list_->ClearDsv(RtDsv(d->view), cf, std::clamp(z, 0.0f, 1.0f), uint8_t(stencil), &r, 1);
        }
      }

      return;
    }

    if (stats_.clear_logged++ < 12)
      REXLOG_INFO("[sst-native] clear flags {:X} colour {:08X} z {} stencil {} RT0 {:08X} [{:08X}]", flags, color, z, stencil, rt0, rt0 ? be32(base, rt0 + 0x24) : 0);

    if (rt0)
    {
      // In auto mode the viewport isn't reliable at clear time (it is often still the previous target's), so use what draws taught us about this render target, and the viewport only for targets not drawn to yet
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

    // Xbox D3DCLEAR_TARGET0..3 = 0x10..0x80 (the D3D clear helper masks 0xF0 out before handling depth / stencil); the low bits are depth / stencil
    if ((flags & 0xF0) && rt0)
    {
      const float c[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f, float(color & 0xFF) / 255.0f, float((color >> 24) & 0xFF) / 255.0f};
      list_->ClearRtv(Rtv(target_), c);
    }

    D3D12_CLEAR_FLAGS ds = D3D12_CLEAR_FLAGS(0);

    if (flags & 0x3)
      ds |= D3D12_CLEAR_FLAG_DEPTH;

    if (flags & 0x4)
      ds |= D3D12_CLEAR_FLAG_STENCIL;

    if (ds & D3D12_CLEAR_FLAG_DEPTH)
      last_depth_clear_ = std::clamp(z, 0.0f, 1.0f);

    if (ds)
      list_->ClearDsv(kOutputDsv, ds, std::clamp(z, 0.0f, 1.0f), uint8_t(stencil));
  }
}