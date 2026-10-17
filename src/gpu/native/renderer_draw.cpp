// Native renderer: Renderer::Draw and its steps. The vertex input steps are renderer_vertices.cpp; shaders, constants, textures and the geometry cache have files of their own (renderer_shaders, renderer_constants, renderer_textures, renderer_geometry)

#include "gpu/native/draw_context.h"
#include "gpu/native/guest_watch.h"
#include "gpu/native/renderer_debug.h"

namespace sst::native
{
  using namespace renderer_detail;

  // Draw: one guest draw call, recorded into the frame's command list. Each step works out one part of the draw into a DrawContext and returns false when the draw can't be drawn (the reason is counted in stats_ / logged once)
  void Renderer::Draw(const uint8_t *base, const DrawCall &call)
  {
    ++stats_.draws;
    ++draw_seq_;
    ++stats_.prims[call.prim];

    if (!call.count)
      return;

    DrawContext d(base, call, prof_);

    if (RunMemexportDraw(d))
      return; // a morph pass: no rendering

    if (!SelectRenderTarget(d) || !ChooseTopology(d) || !ResolvePixelShader(d) || !DebugFilterDraw(d))
      return;

    if (!BuildVertexLayout(d) || !BindDrawTargets(d) || !SelectShaderVariants(d) || !BuildPipelineKey(d))
      return;

    if (!ReadIndices(d) || !GetDrawPipeline(d) || !ResolveVertexStreams(d) || !UploadVertices(d) || !UploadIndices(d))
      return;

    if (!SetConstantsAndViewport(d))
      return;

    RecordDraw(d);
    DebugAfterDraw(d);
  }

  // The vertex shader, and memory-export (morph) passes: no rendering, they write vertices a later draw reads. Run them whatever the render target
  bool Renderer::RunMemexportDraw(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const DrawCall &call = d.call;
    const uint32_t dev = d.dev;
    PhaseClock &clk = d.clk;
    const uint32_t vs_obj = be32(base, dev + kDevVS), ps_obj = be32(base, dev + kDevPS);
    clk.Switch(kPhShaders);
    ShaderEntry *vs_entry = ResolveShader(base, vs_obj, true);
    clk.Switch(kPhOther);

    if (rt_trace_.active)
      TraceDraw(base, dev, call, vs_entry && vs_entry->memexport);

    if (vs_entry && vs_entry->memexport)
    {
      PublishConstantUse(guest_watch::ShaderKey(vs_obj, be32(base, vs_obj + 0x20)), 0xFFFF); // the stream constants are found by scanning c0-c255
      clk.Switch(kPhMemexport);
      RunMemexport(base, call, vs_obj, vs_entry);
      return true;
    }

    d.vs_obj = vs_obj;
    d.ps_obj = ps_obj;
    d.vs_entry = vs_entry;
    return false;
  }

  // The colour target: draws into surfaces the renderer doesn't emulate (legacy mode) are skipped
  bool Renderer::SelectRenderTarget(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const uint32_t dev = d.dev;

    // Render target
    const uint32_t rt0 = be32(base, dev + kDevRT0);

    if (!rt0)
    {
      ++stats_.skip_rt;
      return false;
    }

    const uint32_t rt_size = be32(base, rt0 + 0x24);
    ++stats_.render_targets[(uint64_t(rt0) << 32) | rt_size];

    if (LogOnce(0x9000000000000000ull | rt0))
    {
      REXLOG_INFO("[sst-native] render target {:08X}: size word {:08X} (~{}x{}), depth {:08X}, viewport scale {} {} {} offset {} {} {}, VTE {:08X}",
                  rt0, rt_size, (rt_size >> 18) + 1, ((rt_size >> 3) & 0x7FFF) + 1, be32(base, dev + kDevDepth), RegF(base, dev, PA_CL_VPORT_XSCALE), RegF(base, dev, PA_CL_VPORT_YSCALE), RegF(base, dev, PA_CL_VPORT_ZSCALE), RegF(base, dev, PA_CL_VPORT_XOFFSET), RegF(base, dev, PA_CL_VPORT_YOFFSET), RegF(base, dev, PA_CL_VPORT_ZOFFSET), Reg(base, dev, PA_CL_VTE_CNTL));
    }

    const bool rt_mode = RenderTargetsEnabled();
    const bool rt_selected = rt_mode || RenderTargetSelected(base, dev, rt_size);

    if (REXCVAR_GET(sst_native_rt) == "auto")
      (rt_selected ? main_rts_ : other_rts_).insert(rt0);

    if (!rt_selected)
    {
      ++stats_.skip_rt;
      return false;
    }

    d.rt0 = rt0;
    d.rt_mode = rt_mode;
    return true;
  }

  bool Renderer::ChooseTopology(DrawContext &d)
  {
    const DrawCall &call = d.call;
    const bool rt_mode = d.rt_mode;
    D3D_PRIMITIVE_TOPOLOGY &topology = d.topology;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE &topology_type = d.topology_type;
    bool &rect_list = d.rect_list;
    bool &convert = d.convert; // TRIFAN / QUADLIST -> indexed triangle list

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
        return false;
      }

      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      convert = true;
      break;

    case 8: // RECTLIST: 3 corners per rectangle, 4th added on the CPU
      if (!rt_mode)
      {
        ++stats_.skip_prim;
        return false;
      }

      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      rect_list = true;
      break;

    default: // others: later
      ++stats_.skip_prim;
      return false;
    }

    return true;
  }

  // Objects the Create*Shader hooks didn't see are rebuilt from the object itself (container copy + microcode)
  bool Renderer::ResolvePixelShader(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const DrawCall &call = d.call;
    const uint32_t vs_obj = d.vs_obj, ps_obj = d.ps_obj;
    const ShaderEntry *vs_entry = d.vs_entry;
    ShaderEntry *&ps_entry = d.ps_entry;
    PhaseClock &clk = d.clk;
    clk.Switch(kPhShaders);
    ps_entry = ps_obj ? ResolveShader(base, ps_obj, false) : nullptr;
    clk.Switch(kPhOther);

    if (!vs_entry || vs_entry->memexport || (ps_obj && !ps_entry))
    {
      ++stats_.skip_shader;

      if (stats_.skip_shader_logged++ < 8)
      {
        REXLOG_INFO("[sst-native] skipped draw: VS {:08X} ({}) PS {:08X} ({}) prim {} count {}", vs_obj, !vs_entry ? "unknown" : vs_entry->memexport ? "memexport"
                                                                                                                                                     : "ok",
                    ps_obj, !ps_obj ? "none" : ps_entry ? "ok"
                                                        : "unknown",
                    call.prim, call.count);
      }

      return false;
    }

    return true;
  }

  // Render state from the device's register mirror, and the host textures for this draw's colour / depth surfaces
  bool Renderer::BindDrawTargets(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const uint32_t dev = d.dev;
    const uint32_t rt0 = d.rt0;
    const bool rt_mode = d.rt_mode;
    HostRT *&color_rt = d.color_rt;
    HostRT *&depth_rt = d.depth_rt;
    uint32_t &target_w = d.target_w, &target_h = d.target_h;
    d.depthcontrol = Reg(base, dev, RB_DEPTHCONTROL);
    d.colorcontrol = Reg(base, dev, RB_COLORCONTROL);
    d.mode = Reg(base, dev, PA_SU_SC_MODE_CNTL);
    const uint32_t depthcontrol = d.depthcontrol;
    target_w = width_;
    target_h = height_;

    if (rt_mode)
    {
      // What the viewport covers (for sizing targets drawn bigger than the surface)
      uint32_t vw = 0, vh = 0;

      if (Reg(base, dev, PA_CL_VTE_CNTL) & 1)
      {
        const float xs = std::fabs(RegF(base, dev, PA_CL_VPORT_XSCALE));
        const float ys = std::fabs(RegF(base, dev, PA_CL_VPORT_YSCALE));
        const float xo = RegF(base, dev, PA_CL_VPORT_XOFFSET), yo = RegF(base, dev, PA_CL_VPORT_YOFFSET);

        if (std::isfinite(xs + ys + xo + yo) && xs < 8192 && ys < 8192)
        {
          vw = uint32_t(std::max(0.0f, std::ceil(xo + xs)));
          vh = uint32_t(std::max(0.0f, std::ceil(yo + ys)));
        }
      }

      const uint32_t ds_surface = be32(base, dev + kDevDepth);
      const Scale2 rs = SurfaceScale(base, dev, rt0, ds_surface);

      if (shadows_off_ && rs == shadow_scale_ && (shadow_surfaces_.count(rt0) || shadow_surfaces_.count(ds_surface)))
      {
        ++stats_.skip_cull; // VIDEO > SHADOWS OFF: nothing drawn into the shadow map
        return false;
      }

      const uint32_t samples = (scene_surfaces_.count(rt0) || scene_surfaces_.count(ds_surface)) ? msaa_ : 1;
      color_rt = GetHostRT(base, rt0, false, rs, samples, vw, vh);

      if (!color_rt)
      {
        ++stats_.skip_rt;
        return false;
      }

      if (bloom_off_ && BloomSurface(*color_rt, rt0))
      {
        ++stats_.skip_cull; // VIDEO > BLOOM OFF: the glow / bloom targets stay black
        return false;
      }

      const uint32_t ds = be32(base, dev + kDevDepth);

      if (ds && (depthcontrol & 3)) // stencil or depth test on
      {
        depth_rt = GetHostRT(base, ds, true, rs, samples, vw, vh);

        if (depth_rt && (depth_rt->width != color_rt->width || depth_rt->height != color_rt->height || depth_rt->samples != color_rt->samples))
        {
          if (LogOnce(0xAC00000000000000ull ^ (uint64_t(rt0) << 24) ^ ds))
            REXLOG_INFO("[sst-native] colour target {:08X} ({}x{}) and depth {:08X} ({}x{}) differ in size: drawing without depth", rt0, color_rt->width, color_rt->height, ds, depth_rt->width, depth_rt->height);

          depth_rt = nullptr;
        }
      }

      target_w = color_rt->width;
      target_h = color_rt->height;
    }

    d.has_depth = rt_mode ? depth_rt != nullptr : be32(base, dev + kDevDepth) != 0;
    d.draw_scale = color_rt ? color_rt->scale : output_scale_[target_];
    return true;
  }

  // Alpha test and culling that leave nothing to draw; the compiled shader variants (from the exe's shader pack, or compiled on worker threads: skipped until ready. urgent: a draw waits for them, so they jump the compile queue)
  bool Renderer::SelectShaderVariants(DrawContext &d)
  {
    const uint32_t colorcontrol = d.colorcontrol, mode = d.mode;
    const ShaderEntry *ps_entry = d.ps_entry;
    uint32_t ps_spec = 0;
    const bool alpha_test = (colorcontrol >> 3) & 1;
    const uint32_t alpha_func = colorcontrol & 7;

    if (alpha_test && alpha_func == 0) // NEVER
    {
      ++stats_.skip_cull;
      return false;
    }

    if (alpha_test && alpha_func != 7)
      ps_spec |= sst::xenos::kSpecAlphaTest;

    if ((mode & 3) == 3) // both faces culled
    {
      ++stats_.skip_cull;
      return false;
    }

    d.vs = shaders_->GetVariant(d.vs_entry, d.vs_spec, true);
    d.ps = ps_entry ? shaders_->GetVariant(d.ps_entry, ps_spec, true) : nullptr;
    d.debug_shading = REXCVAR_GET(sst_native_debug_shading) && debug_ps_;

    if (!d.vs || (ps_entry && !d.ps))
    {
      ++stats_.skip_pending;
      return false;
    }

    d.bisect_hide = DebugBisectHides(d);
    return true;
  }

  // Everything that selects the pipeline: shaders, layout, blend, depth, cull, target formats, polygon offset
  bool Renderer::BuildPipelineKey(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const uint32_t dev = d.dev;
    const uint32_t depthcontrol = d.depthcontrol, colorcontrol = d.colorcontrol, mode = d.mode;
    const bool rt_mode = d.rt_mode, has_depth = d.has_depth, rect_list = d.rect_list, bisect_hide = d.bisect_hide;
    const HostRT *color_rt = d.color_rt, *depth_rt = d.depth_rt;
    const ShaderVariant *vs = d.vs;
    const ShaderVariant *&ps = d.ps;
    const bool debug_shading = d.debug_shading;
    const D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type = d.topology_type;
    const uint64_t *layout_hash_input = d.layout_hash_input;
    const uint32_t layout_words = d.layout_words;
    PipelineKey &key = d.key;
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
      return false;
    }

    key.color_mask = ps && !bisect_hide ? uint8_t(Reg(base, dev, RB_COLOR_MASK) & 0xF) : 0;

    if (LogOnce(0xD000000000000000ull | (uint64_t(key.color_mask) << 32) | key.blend))
      REXLOG_INFO("[sst-native] new state: colour mask {:X} blend {:08X} depth {:08X} colour control {:08X} mode {:08X}", key.color_mask, key.blend, depthcontrol, colorcontrol, mode);

    key.depth_enable = has_depth && ((depthcontrol >> 1) & 1);
    key.depth_write = key.depth_enable && ((depthcontrol >> 2) & 1);
    key.depth_func = key.depth_enable ? uint8_t((depthcontrol >> 4) & 7) : 7;
    key.cull = rect_list ? 0 : uint8_t(mode & 3);
    key.rtv_format = rt_mode ? uint8_t(color_rt->format) : 0;
    key.dsv = rt_mode ? (depth_rt ? 2 : 1) : 0;

    // MSAA: log2(samples) in bits 4-7 (0 without MSAA, so other keys are unchanged)
    if (rt_mode && color_rt->samples > 1)
      key.dsv |= uint8_t(std::countr_zero(color_rt->samples) << 4);

    key.front_ccw = ((mode >> 2) & 1) ? 0 : 1;

    // Polygon offset (the shadow pass's depth bias; without it surfaces shadow themselves in a dotted pattern). PA_SU_SC_MODE_CNTL bits 11-13 enable it for front / back / both; the device keeps PA_SU_POLY_OFFSET_FRONT_SCALE, _FRONT_OFFSET, _BACK_SCALE, _BACK_OFFSET at dev+0x2A50..+0x2A5C. Units as Xenia: scale in 1/16 subpixels, offset in float24 depth ULPs
    if (key.depth_enable && (mode & (7u << 11)))
    {
      const bool front = (mode >> 11) & 1, para = (mode >> 13) & 1;
      const uint32_t at = dev + ((front || para) ? 0x2A50 : 0x2A58);
      float scale, offset;
      const uint32_t sv = be32(base, at), ov = be32(base, at + 4);
      std::memcpy(&scale, &sv, 4);
      std::memcpy(&offset, &ov, 4);

      if (std::isfinite(scale) && std::isfinite(offset))
      {
        int32_t bias = int32_t(std::ceil(std::fabs(offset) * float(1u << 21))) << 3;
        key.depth_bias = offset < 0 ? -bias : bias;
        key.slope_bias = scale * (1.0f / 16.0f);

        if (LogOnce(0xAE00000000000000ull ^ (uint64_t(sv) << 20) ^ ov))
          REXLOG_INFO("[sst-native] polygon offset: scale {} offset {} (mode {:08X}) -> depth bias {} slope {}", scale, offset, mode, key.depth_bias, key.slope_bias);
      }
    }

    key.topology_type = uint8_t(topology_type);
    return true;
  }

  // The pipeline (skipped while it compiles in the background; UI-like draws get theirs at once)
  bool Renderer::GetDrawPipeline(DrawContext &d)
  {
    d.clk.Switch(kPhPipeline);
    const bool ui_like = (!d.key.depth_enable || d.key.depth_func == 7) && d.vmax >= d.vmin && d.vmax - d.vmin < 256; // no depth test (or ALWAYS)
    d.pso = GetPipeline(d.key, d.layout, d.vs, d.ps, ui_like);

    if (!d.pso)
    {
      ++stats_.skip_pipeline;
      return false;
    }

    if (!recording_)
      BeginFrame();

    return true;
  }

  // Aspect ratio (VIDEO > ASPECT RATIO wider than 16:9): whether this draw is UI to squeeze into a centred 16:9 box, and for layers on one edge of the frame, the clip-space transform that keeps that edge on the screen's edge
  Renderer::UiFit Renderer::FitUi(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const DrawCall &call = d.call;
    const uint32_t dev = d.dev;
    const ShaderEntry *vs_entry = d.vs_entry;
    const uint32_t vs_obj = d.vs_obj, rt0 = d.rt0, depthcontrol = d.depthcontrol;
    const HostRT *color_rt = d.color_rt;
    const uint32_t first_vertex = d.first_vertex, vertex_count = d.vertex_count;

    // Aspect ratio: UI draws (into the full-size UI / final targets) keep their proportions in a centred 16:9 box
    // 2D draws (w = 1, measured on the CPU) into full-size targets (UI and the 2D layers of menus drawn into the 3D scene's target) are fitted, except full-width layers (backgrounds, fades, full-screen copies), which stretch to fill the screen. Draws that cannot be measured: the UI targets'
    // draws that don't sample a full-screen copy:
    //  - a single quad over the whole width (background, fade, full-screen copy) stretches;
    //  - a layer touching one edge of the 16:9 frame (menu backgrounds, bars) keeps that edge on the screen's edge and its other edge fitted;
    //  - in the 3D scene's target (menus drawn with 3D characters) fitted 2D is clipped to the 16:9 box, as on a 16:9 screen (parked elements)
    bool ui_fit = false, box_clip = false;
    float edge_a = 0.0f, edge_b = 0.0f; // edge-touching layers: clip x' = a x + b (else 0)

    if ((ui_fit_x_ != 1.0f || ui_fit_y_ != 1.0f) && REXCVAR_GET(sst_native_ui_fit) && color_rt && !color_rt->depth && color_rt->width * 10 >= width_ * 9 && !shadow_surfaces_.count(rt0))
    {
      // Draws that test and write depth are 3D (never measured: running every 3D draw's vertex shader on the CPU costs a lot of frame time). Small ones (a quad) are measured anyway: 2D sprites drawn with depth (e.g. character select's loading ring) are flat and get fitted
      const bool depth_test = (depthcontrol & 6) == 6; // Z_ENABLE + Z_WRITE_ENABLE
      const bool measure = !depth_test || vertex_count <= 6;
      const DrawExtent ext = measure ? MeasureDraw(base, call, vs_obj, vs_entry, first_vertex, vertex_count) : DrawExtent{};

      if (depth_test && scene_surfaces_.count(rt0) && vertex_count <= 6 && vs_entry && LogOnce(0xD100000000000000ull ^ vs_entry->hash ^ (uint64_t(ext.ok) << 40) ^ (uint64_t(ext.flat) << 41)))
        REXLOG_INFO("[sst-native] aspect: small depth-tested draw in the 3D target (vs {:016X}, {} vertices, {}, {}) - {}", vs_entry->hash, vertex_count, call.up_data ? "user memory" : "vertex buffer", ext.ok ? (ext.flat ? "flat" : "3D") : "not measured", ext.ok && ext.flat ? "fitted" : "not fitted");

      if (depth_test && !(ext.ok && ext.flat))
      {
        ui_fit = false;
      }
      else if (ext.ok)
      {
        const bool touch_l = ext.x0 >= -1.02f && ext.x0 <= -0.98f;
        const bool touch_r = ext.x1 >= 0.98f && ext.x1 <= 1.02f;

        // (A quad reaching well past the frame was laid out for the wider screen - wide layouts, menu/scene_widen.cpp - and is fitted like the rest.)
        const bool within = ext.x0 >= -1.05f && ext.x1 <= 1.05f;
        const bool full = (ext.x0 <= -0.98f && ext.x1 >= 0.98f && vertex_count <= 6 && within) || (touch_l && touch_r);
        ui_fit = ext.flat && !full;

        // (Single quads only - a curved shape stretched this way would no longer meet the fitted pieces next to it.)
        if (ui_fit && ui_fit_x_ < 1.0f && (touch_l || touch_r) && vertex_count <= 6)
        {
          const float k = ui_fit_x_;

          if (touch_l)
          {
            edge_a = (ext.x1 * k + 1.0f) / (ext.x1 + 1.0f);
            edge_b = edge_a - 1.0f;
          }
          else
          {
            edge_a = (1.0f - ext.x0 * k) / (1.0f - ext.x0);
            edge_b = 1.0f - edge_a;
          }
        }
      }
      else
      {
        // Not measurable (e.g. drawn from user memory - the game's animated sprites, like character select's loading ring): UI targets' draws are fitted; in the 3D scene's target only those without depth testing (2D - 3D effects there test depth)
        const bool z_test = (depthcontrol & 2) != 0;
        ui_fit = !draw_composite_ && (!scene_surfaces_.count(rt0) || (call.up_data && !z_test));

        if (scene_surfaces_.count(rt0) && vs_entry && vertex_count <= 64 && LogOnce(0xD000000000000000ull ^ vs_entry->hash ^ (uint64_t(z_test) << 40)))
          REXLOG_INFO("[sst-native] aspect: unmeasured draw in the 3D target (vs {:016X}, prim {}, {} vertices, {}, depth {:08X}) - {}", vs_entry->hash, call.prim, vertex_count, call.up_data ? "user memory" : "vertex buffer", depthcontrol, ui_fit ? "fitted" : "not fitted");
      }

      // F9 capture: every fitted draw (aspect-ratio debugging)
      if (capture_active_ && (ui_fit || (ext.ok && vertex_count <= 64)))
      {
        REXLOG_INFO("[sst-native] fit{}: draw {} rt {:08X}{} prim {} verts {} vs {:016X} depthctl {:08X} measured {} flat {} x {:.3f}..{:.3f} y {:.3f}..{:.3f} edge {:.3f} box {} composite {} viewport x {:.1f}+{:.1f} y {:.1f}+{:.1f} vte {:08X}{}",
                    ui_fit ? "" : " (NOT fitted)", draw_seq_, rt0, scene_surfaces_.count(rt0) ? " (scene)" : "", call.prim, vertex_count, vs_entry ? vs_entry->hash : 0, depthcontrol, ext.ok, ext.flat, ext.x0, ext.x1, ext.y0, ext.y1, edge_a, box_clip, draw_composite_, RegF(base, dev, PA_CL_VPORT_XOFFSET), RegF(base, dev, PA_CL_VPORT_XSCALE), RegF(base, dev, PA_CL_VPORT_YOFFSET), RegF(base, dev, PA_CL_VPORT_YSCALE), Reg(base, dev, PA_CL_VTE_CNTL), draw_textures_);
      }
    }

    if (capture_active_ && color_rt && color_rt->width * 10 < width_ * 9 && vertex_count <= 64)
      REXLOG_INFO("[sst-native] fit (small target {}x{}): draw {} rt {:08X} prim {} verts {} vs {:016X} depthctl {:08X}{}", color_rt->width, color_rt->height, draw_seq_, rt0, call.prim, vertex_count, vs_entry ? vs_entry->hash : 0, depthcontrol, draw_textures_);

    return {ui_fit, edge_a, edge_b};
  }

  // Constants (device mirror: big-endian float4 x 256 per stage, plus the shared constants the translated shaders read), textures, the UI fit for wide screens, the viewport and the scissor
  bool Renderer::SetConstantsAndViewport(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const DrawCall &call = d.call;
    const uint32_t dev = d.dev;
    PhaseClock &clk = d.clk;
    const ShaderEntry *vs_entry = d.vs_entry, *ps_entry = d.ps_entry;
    const ShaderVariant *ps = d.ps;
    const uint32_t target_w = d.target_w, target_h = d.target_h;
    const uint32_t first_vertex = d.first_vertex;
    const Scale2 draw_scale = d.draw_scale;
    float &cap_half_x = d.half_x, &cap_half_y = d.half_y;
    clk.Switch(kPhConstants);

    // Identical constant files (very common between draws) are uploaded once per frame. Only the registers the shaders read (most use a fraction of the 256). The game thread compares only these constant bytes of the device block for this shader object (native.cpp, DeviceDelta).
    // Keyed by object and microcode pointer, both read from the draw's snapshot; only for shaders resolved here.
    if (const uint32_t obj = be32(base, dev + kDevVS); obj && vs_entry)
      PublishConstantUse(guest_watch::ShaderKey(obj, be32(base, obj + 0x20)), ConstantChunkMask(vs_entry));

    if (const uint32_t obj = be32(base, dev + kDevPS); obj && ps_entry)
      PublishConstantUse(guest_watch::ShaderKey(obj, be32(base, obj + 0x18)), ConstantChunkMask(ps_entry));

    d.vsc_gpu = UploadConstants(GuestPtr(base, dev + kDevVSConstants), ConstantBytes(vs_entry));
    d.psc_gpu = UploadConstants(GuestPtr(base, dev + kDevPSConstants), ps ? ConstantBytes(ps_entry) : 16);
    const Upload shared = Allocate(512);

    if (!d.vsc_gpu || !d.psc_gpu || !shared.cpu)
      return false;

    {
      uint32_t sc[80] = {}; // [76] (c19.x): g_SstDepthFlip

      for (int i = 0; i < 16; ++i)
      {
        sc[i] = kPlaceholder2D;
        sc[16 + i] = kPlaceholder3D;
        sc[32 + i] = kPlaceholderCube;
        sc[48 + i] = 0; // sampler
      }

      clk.Switch(kPhTextures);
      draw_composite_ = false;
      draw_textures_.clear();
      BindTextures(base, dev, vs_entry, ps ? ps_entry : nullptr, sc);
      clk.Switch(kPhUiFit);

      const UiFit fit = FitUi(d);
      const bool ui_fit = fit.fit;
      const float edge_a = fit.edge_a, edge_b = fit.edge_b;
      const float fit_cx = float(width_) * 0.5f, fit_cy = float(height_) * 0.5f;
      clk.Switch(kPhConstants);

      const uint32_t vs_bools = be32(base, dev + kDevBools);
      const uint32_t ps_bools = be32(base, dev + kDevBools + 16);
      sc[64] = (vs_bools & 0xFFFF) | (ps_bools << 16);
      sc[65] = 0; // g_SwappedTexcoords: the stream swap already matches Xenos order
      float half_x = 0, half_y = 0;
      float alpha_ref = RegF(base, dev, RB_ALPHA_REF);
      std::memcpy(&sc[68], &alpha_ref, 4);

      // g_VertexIndexBase: SV_VertexID + this = the Xenos vertex index (r0.x)
      sc[69] = first_vertex;

      // Viewport (needed for the half-pixel offset too)
      const uint32_t vte = Reg(base, dev, PA_CL_VTE_CNTL);
      float xs = RegF(base, dev, PA_CL_VPORT_XSCALE), xo = RegF(base, dev, PA_CL_VPORT_XOFFSET);
      float ys = RegF(base, dev, PA_CL_VPORT_YSCALE), yo = RegF(base, dev, PA_CL_VPORT_YOFFSET);
      float zs = RegF(base, dev, PA_CL_VPORT_ZSCALE), zo = RegF(base, dev, PA_CL_VPORT_ZOFFSET);
      D3D12_VIEWPORT vp = {0, 0, float(target_w), float(target_h), 0, 1};

      if ((vte & 1) && xs != 0 && ys != 0 && std::isfinite(xs) && std::isfinite(ys))
      {
        vp.TopLeftX = xo - std::fabs(xs);
        vp.Width = 2 * std::fabs(xs);
        vp.TopLeftY = yo - std::fabs(ys);
        vp.Height = 2 * std::fabs(ys);
        vp.MinDepth = std::clamp(zo, 0.0f, 1.0f);
        vp.MaxDepth = std::clamp(zo + zs, 0.0f, 1.0f);

        // A negative z scale (the game's reversed depth) gives an inverted depth range (MinDepth > MaxDepth), which Intel GPUs evaluate too coarsely: eyes and moustaches drawn just in front of the face fail the depth test. Instead the vertex shader outputs w - z (g_SstDepthFlip, XenosRecomp under SST_RECOMP) and the range is the normal way round: same window depth
        if (zs < 0 && REXCVAR_GET(sst_native_depth_flip_vs))
        {
          std::swap(vp.MinDepth, vp.MaxDepth);
          sc[76] = 1;
        }

        if (ys > 0 && LogOnce(0xC000000000000001ull))
          REXLOG_WARN("[sst-native] positive viewport y scale ({}): image may be flipped", ys);
      }
      else if (LogOnce(0xC000000000000000ull | vte))
      {
        REXLOG_INFO("[sst-native] viewport transform off (PA_CL_VTE_CNTL {:08X}), using full target", vte);
      }

      if (REXCVAR_GET(sst_native_half_pixel) && vp.Width > 0 && vp.Height > 0)
      {
        half_x = 1.0f / vp.Width; // half a guest pixel in clip space
        half_y = -1.0f / vp.Height;
      }

      std::memcpy(&sc[66], &half_x, 4);
      std::memcpy(&sc[67], &half_y, 4);

      // Internal resolution: host pixels = guest pixels x the target's scale. Pixel shaders that read their screen position get it in guest pixels
      const float inv_scale[2] = {1.0f / draw_scale.x, 1.0f / draw_scale.y};
      std::memcpy(&sc[70], inv_scale, 8); // g_InvRenderScale (c17.zw)

      // g_ClipTransform (c18): clip-space scale + offset, applied in the vertex shader so UI moved outside the 16:9 box (ultrawide HUD) is not clipped. UI draws: positions scaled by ui_fit_ about the frame's centre
      {
        float ct[4] = {1.0f, 1.0f, 0.0f, 0.0f};

        if (ui_fit && vp.Width > 0 && vp.Height > 0)
        {
          const float hw = vp.Width * 0.5f, hh = vp.Height * 0.5f;
          const float vcx = vp.TopLeftX + hw, vcy = vp.TopLeftY + hh;
          ct[0] = ui_fit_x_;
          ct[1] = ui_fit_y_;
          ct[2] = (vcx - fit_cx) * (ui_fit_x_ - 1.0f) / hw;
          ct[3] = (vcy - fit_cy) * (1.0f - ui_fit_y_) / hh;

          if (edge_a != 0.0f)
            ct[0] = edge_a, ct[2] = edge_b; // (full-frame viewport)
        }

        std::memcpy(&sc[72], ct, sizeof(ct));
      }

      vp.TopLeftX *= draw_scale.x;
      vp.TopLeftY *= draw_scale.y;
      vp.Width *= draw_scale.x;
      vp.Height *= draw_scale.y;
      cap_half_x = half_x;
      cap_half_y = half_y;
      std::memcpy(shared.cpu, sc, sizeof(sc));
      list_->RSSetViewports(1, &vp);

      // Scissor: SetScissorRect (sub_822FD8D8) keeps PA_SC_WINDOW_SCISSOR_TL/BR at dev+0x28C4/+0x28C8 (x | y << 16): the viewport rectangle, intersected with the scissor rectangle when scissor testing is on (dev+0x2E50)
      {
        D3D12_RECT rect = {0, 0, LONG(target_w), LONG(target_h)};

        if (REXCVAR_GET(sst_native_scissor))
        {
          const uint32_t tl = be32(base, dev + kDevScissorTL), br = be32(base, dev + kDevScissorBR);
          const LONG x0 = LONG(tl & 0x7FFF), y0 = LONG((tl >> 16) & 0x7FFF);
          const LONG x1 = LONG(br & 0x7FFF), y1 = LONG((br >> 16) & 0x7FFF);

          if (x1 > x0 && y1 > y0)
          {
            rect = {std::min(x0, LONG(target_w)), std::min(y0, LONG(target_h)), std::min(x1, LONG(target_w)), std::min(y1, LONG(target_h))};
            const bool partial = x0 > 0 || y0 > 0 || x1 < LONG(target_w) || y1 < LONG(target_h);

            if (partial)
            {
              ++stats_.scissored_draws;

              if (LogOnce(0xE000000000000000ull | (uint64_t(tl) << 32 | br) >> 4) && stats_.scissor_logged < 16)
              {
                ++stats_.scissor_logged;
                REXLOG_INFO("[sst-native] scissor {}x{}-{}x{} (prim {}, {} vertices)", x0, y0, x1, y1, call.prim, call.count);
              }
            }
          }
        }

        // UI scissor rectangles (clipped text boxes) follow the fit. A side on the frame's edge (a scissor around the whole frame - which the 3D scene's 1280x736 target makes smaller than the target - or a tile of it) goes to the target's edge instead: wide layouts and edge-anchored HUD draw beyond the 16:9 box
        if (ui_fit && edge_a == 0.0f)
        {
          const LONG fw = std::min(LONG(target_w), LONG(width_));
          const LONG fh = std::min(LONG(target_h), LONG(height_));
          auto fx = [&](LONG v)
          {
            return fit_cx + (float(v) - fit_cx) * ui_fit_x_;
          };

          auto fy = [&](LONG v)
          {
            return fit_cy + (float(v) - fit_cy) * ui_fit_y_;
          };

          rect = {rect.left <= 0 ? 0 : LONG(std::floor(fx(rect.left))), rect.top <= 0 ? 0 : LONG(std::floor(fy(rect.top))), rect.right >= fw ? LONG(target_w) : LONG(std::ceil(fx(rect.right))), rect.bottom >= fh ? LONG(target_h) : LONG(std::ceil(fy(rect.bottom)))};
        }

        rect = Scaled(rect, draw_scale);
        list_->RSSetScissorRects(1, &rect);
      }
    }

    d.shared_gpu = shared.gpu;
    return true;
  }

  // Targets, pipeline, constants and the draw itself
  void Renderer::RecordDraw(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const uint32_t dev = d.dev;
    const bool rt_mode = d.rt_mode;
    HostRT *color_rt = d.color_rt, *depth_rt = d.depth_rt;
    d.clk.Switch(kPhRecord);
    const float blend_factor[4] = {RegF(base, dev, RB_BLEND_RED), RegF(base, dev, RB_BLEND_RED + 1), RegF(base, dev, RB_BLEND_RED + 2), RegF(base, dev, RB_BLEND_RED + 3)};
    list_->OMSetBlendFactor(blend_factor);

    if (rt_mode)
    {
      SetRTState(*color_rt, D3D12_RESOURCE_STATE_RENDER_TARGET);

      if (depth_rt)
        SetRTState(*depth_rt, D3D12_RESOURCE_STATE_DEPTH_WRITE);

      const uint64_t pair = (uint64_t(color_rt->view + 1) << 32) | (depth_rt ? depth_rt->view + 1 : 0);

      if (pair != pass_pair_)
      {
        const uint32_t rt0 = be32(base, dev + kDevRT0);
        MarkPass(rt0, color_rt->width, color_rt->height, scene_surfaces_.count(rt0) ? 1 : shadow_surfaces_.count(rt0) ? 2
                                                                                                                      : 0);
        pass_pair_ = pair;
      }

      if (pair != bound_targets_)
      {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtRtv(color_rt->view);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth_rt ? RtDsv(depth_rt->view) : rtv;
        list_->OMSetRenderTargets(1, &rtv, FALSE, depth_rt ? &dsv : nullptr);
        bound_targets_ = pair;
      }

      last_color_rt_ = color_rt;
    }

    list_->SetPipelineState(d.pso);
    list_->SetGraphicsRootConstantBufferView(kRootVSConstants, d.vsc_gpu);
    list_->SetGraphicsRootConstantBufferView(kRootVSConstantsRaw, d.vsc_gpu);
    list_->SetGraphicsRootConstantBufferView(kRootPSConstants, d.psc_gpu);
    list_->SetGraphicsRootConstantBufferView(kRootPSConstantsRaw, d.psc_gpu);
    list_->SetGraphicsRootConstantBufferView(kRootShared, d.shared_gpu);
    list_->IASetPrimitiveTopology(d.topology);
    DebugMarkDraw(d);
    NoteDraw(d.vs_entry ? d.vs_entry->hash : 0, d.ps_entry ? d.ps_entry->hash : 0);

    if (d.draw_indexed)
      list_->DrawIndexedInstanced(d.draw_count, 1, 0, d.convert || d.rect_list ? 0 : -int32_t(d.vmin), 0);
    else
      list_->DrawInstanced(d.draw_count, 1, 0, 0);

    ++stats_.drawn;

    if (!d.ps)
      ++stats_.depth_only;
  }
}