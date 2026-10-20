// Native renderer's debug tools: draw tracing, F9 capture, VS position / morph checks, draw labels

#include "gpu/native/draw_context.h"
#include "gpu/native/renderer_debug.h"

#if defined(_WIN32)
#include "gpu/native/gfx/d3d12/d3d12_gfx.h"
#endif

namespace sst::native
{
  using namespace renderer_detail;

  ShaderBisect &renderer_detail::Bisect()
  {
    static ShaderBisect b;
    return b;
  }

  void Renderer::CreateDebugPixelShader()
  {
    // Colour by view distance (SV_Position.w) in bands, slightly darker on steep depth slopes so shapes read without lighting or textures
    static const char kSource[] = "float4 main(float4 p : SV_Position) : SV_Target0 {\n  float d = p.w;\n  float band = frac(d * 0.1);\n  float edge = saturate(1.0 - 40.0 * (abs(ddx(p.z)) + abs(ddy(p.z))));\n  float3 c = float3(0.35 + 0.65 * band, 0.5 + 0.5 * sin(d * 0.37), 1.0 - 0.6 * band);\n  return float4(c * (0.4 + 0.6 * edge), 1.0);\n}\n";
    std::vector<uint8_t> code;

    if (!CompileRendererShader(kSource, L"ps_6_0", code))
      return;

    debug_ps_ = std::make_unique<ShaderVariant>();
    debug_ps_->dxil.assign(code.begin(), code.end());
    debug_ps_->state.store(ShaderVariant::State::kReady);
  }

  std::string Renderer::DescribeTexture(const uint32_t fetch[6])
  {
    const uint32_t type = fetch[0] & 3, format = fetch[1] & 0x3F;
    const uint32_t dim = (fetch[5] >> 9) & 3, stacked = (fetch[1] >> 10) & 1;
    uint32_t w, h, d;

    if (dim == 2) // 3D
    {
      w = (fetch[2] & 0x7FF) + 1, h = ((fetch[2] >> 11) & 0x7FF) + 1, d = (fetch[2] >> 22) + 1;
    }
    else
    {
      w = (fetch[2] & 0x1FFF) + 1, h = ((fetch[2] >> 13) & 0x1FFF) + 1, d = (fetch[2] >> 26) + 1;
    }

    return fmt::format("{:08X} fmt {} {}x{}{} dim {}{}{} pitch {} endian {} swz {:03X} mips {:08X} (type {})",
                       fetch[1] & ~0xFFFu, format, w, h, d > 1 ? fmt::format("x{}", d) : "", dim, stacked ? " stacked" : "", (fetch[0] >> 31) ? " tiled" : " linear", ((fetch[0] >> 22) & 0x1FF) * 32, (fetch[1] >> 6) & 3, (fetch[3] >> 1) & 0xFFF, fetch[5] & ~0xFFFu, type);
  }

  std::string Renderer::DescribeSurface(const uint8_t *base, uint32_t surface)
  {
    if (!surface)
      return "none";

    if (!PlausibleAddress(surface))
      return fmt::format("{:08X}?", surface);

    const uint32_t info = be32(base, surface + 0x1C), size = be32(base, surface + 0x24);
    std::string s = fmt::format("{:08X} [edram base {} fmt {} size word {:08X}]", surface, info & 0xFFF, (info >> 16) & 0xF, size);

    if (rt_trace_.surfaces_logged.insert(surface).second)
    {
      std::string words;

      for (uint32_t i = 0; i < 16; ++i)
        words += fmt::format(" {:08X}", be32(base, surface + 4 * i));

      rt_trace_.lines.push_back(fmt::format("  surface {:08X}:{}", surface, words));
    }

    return s;
  }

  void Renderer::TraceClosePass()
  {
    RtTrace &t = rt_trace_;

    if (!t.has_pass)
      return;

    std::string prims;

    for (auto &[p, n] : t.pass_prims)
      prims += fmt::format(" {:X}:{}", p, n);

    t.lines.push_back(fmt::format("  -> {} draws ({} morph) prims{}; {} pixel shaders", t.pass_draws, t.pass_morph, prims, t.pass_ps.size()));

    for (const std::string &tex : t.pass_textures)
      t.lines.push_back("     samples " + tex);

    t.has_pass = false;
  }

  void Renderer::TraceState(const uint8_t *base, uint32_t dev)
  {
    RtTrace &t = rt_trace_;
    uint32_t words[20];
    uint32_t n = 0;

    for (uint32_t i = 0; i < 5; ++i)
      words[n++] = be32(base, dev + kDevRT0 + 4 * i); // RT0-3, depth

    words[n++] = be32(base, dev + 0x2880); // RB_SURFACE_INFO
    words[n++] = be32(base, dev + 0x2884); // RB_COLOR_INFO
    words[n++] = be32(base, dev + 0x2888); // RB_DEPTH_INFO

    for (uint32_t r = PA_CL_VPORT_XSCALE; r <= PA_CL_VPORT_ZOFFSET; ++r)
      words[n++] = Reg(base, dev, r);

    words[n++] = be32(base, dev + kDevScissorTL);
    words[n++] = be32(base, dev + kDevScissorBR);
    const uint64_t key = XXH3_64bits(words, n * 4);

    if (t.has_pass && key == t.pass_key)
      return;

    TraceClosePass();
    t.has_pass = true;
    t.pass_key = key;
    t.pass_draws = t.pass_morph = 0;
    t.pass_prims.clear();
    t.pass_textures.clear();
    t.pass_texture_keys.clear();
    t.pass_ps.clear();
    const float xs = RegF(base, dev, PA_CL_VPORT_XSCALE), ys = RegF(base, dev, PA_CL_VPORT_YSCALE);
    const float xo = RegF(base, dev, PA_CL_VPORT_XOFFSET), yo = RegF(base, dev, PA_CL_VPORT_YOFFSET);
    const uint32_t tl = words[n - 2], br = words[n - 1];
    t.lines.push_back(fmt::format("pass {}: RT0 {} | RT1 {:08X} | depth {} | surface info {:08X} colour info {:08X} depth info {:08X} | viewport scale {} {} offset {} {} | scissor {}x{}-{}x{}",
                                  ++t.pass_index, DescribeSurface(base, words[0]), words[1], DescribeSurface(base, words[4]), words[5], words[6], words[7], xs, ys, xo, yo, tl & 0x7FFF, (tl >> 16) & 0x7FFF, br & 0x7FFF, (br >> 16) & 0x7FFF));
  }

  void Renderer::TraceDraw(const uint8_t *base, uint32_t dev, const DrawCall &call, bool morph)
  {
    RtTrace &t = rt_trace_;
    TraceState(base, dev);
    ++t.pass_draws;

    if (morph)
      ++t.pass_morph;

    ++t.pass_prims[call.prim];
    const uint32_t ps_obj = be32(base, dev + kDevPS);
    ShaderEntry *ps = ps_obj ? ResolveShader(base, ps_obj, false) : nullptr;

    if (call.up_data && PlausibleAddress(call.up_data))
    {
      // Vertex-data draws (DrawVerticesUP / immediate mode): the raw vertices
      const uint32_t vs_obj = be32(base, dev + kDevVS);
      ShaderEntry *vs = vs_obj ? ResolveShader(base, vs_obj, true) : nullptr;

      std::string line = fmt::format("  {} draw: prim {} vertices {} stride {} vs {:016X} ps {:016X} depthcontrol {:08X} blendcontrol {:08X} colorcontrol {:08X} mode {:08X}",
                                     call.immediate ? "immediate-mode" : "DrawVerticesUP", call.prim, call.count, call.up_stride, vs ? vs->hash : 0, ps ? ps->hash : 0, Reg(base, dev, RB_DEPTHCONTROL), Reg(base, dev, RB_BLENDCONTROL0), Reg(base, dev, RB_COLORCONTROL), Reg(base, dev, PA_SU_SC_MODE_CNTL));

      const uint32_t words = std::min(call.up_stride / 4, 8u);
      const uint32_t shown = std::min(call.count, 6u);

      for (uint32_t v = 0; v < call.count && words; ++v)
      {
        if (v == shown && v + 1 < call.count)
          v = call.count - 1; // ... and the last one

        t.lines.push_back(std::move(line));
        line = fmt::format("      v{}:", v);

        for (uint32_t w = 0; w < words; ++w)
        {
          const uint32_t x = be32(base, call.up_data + v * call.up_stride + 4 * w);
          float f;
          std::memcpy(&f, &x, 4);
          line += (std::fabs(f) > 1e-6f && std::fabs(f) < 1e6f) ? fmt::format(" {:.3f}", f) : fmt::format(" {:08X}", x);
        }
      }

      t.lines.push_back(line);
    }

    if (!ps)
      return;

    t.pass_ps.insert(ps->hash);
    const TextureUsage &u = GetTextureUsage(ps);

    for (uint32_t i = 0; i < 16; ++i)
    {
      if (!((u.mask >> i) & 1))
        continue;

      uint32_t fetch[6];

      for (uint32_t k = 0; k < 6; ++k)
        fetch[k] = be32(base, dev + kDevFetchConstants + 24 * i + 4 * k);

      const uint64_t key = (uint64_t(fetch[1]) << 32) ^ fetch[2] ^ (uint64_t(i) << 58);

      if (t.pass_textures.size() < 16 && t.pass_texture_keys.insert(key).second)
        t.pass_textures.push_back(fmt::format("s{} (tfetch dim {}): {}", i, u.uses[i].dimension - 1, DescribeTexture(fetch)));
    }
  }

  void Renderer::TraceEvent(const uint8_t *base, uint32_t device, const std::string &what)
  {
    if (!rt_trace_.active)
      return;

    TraceState(base, device);
    rt_trace_.lines.push_back("  " + what);
  }

  void Renderer::NoteDraw(uint64_t vs, uint64_t ps)
  {
    draw_labels_[(submitted_ + 1) % std::size(draw_labels_)].draws.emplace_back(vs, ps);
  }

  std::string Renderer::DrawLabel(uint64_t frame, uint32_t draw) const
  {
    for (const DrawLabels &l : draw_labels_)
    {
      if (l.frame != frame)
        continue;

      if (draw >= l.draws.size())
        return fmt::format("draw {} of frame {} (not recorded)", draw, frame);

      const auto &[vs, ps] = l.draws[draw];
      return vs ? fmt::format("draw {}: vs {:016X} ps {:016X}", draw, vs, ps) : fmt::format("draw {}: renderer's own pass {}", draw, ps);
    }

    return fmt::format("draw {} of frame {} (labels gone)", draw, frame);
  }

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

    // On-screen jumps: compare with this mesh's previous frame. A vertex that moves far on screen while the rest of the mesh barely moves is a spike (camera cuts move everything, so they don't count)
    {
      // (The same mesh can be drawn several times a frame - shadow, main pass: compare like with like through the occurrence number.)
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
            {
              if (!jumps++)
                first_jump = v;
            }
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

              REXLOG_WARN("[sst-native] vertex jump (CPU-run shader): VS {:016X} buffer {:08X}{} draw {} vertices from {}: {} vertices jumped (median move {:.4f}); vertex {} screen ({:.3f} {:.3f}) -> ({:.3f} {:.3f}), clip ({} {} {} {});{}",
                          vs_entry->hash, stream0, reads_morph ? " (face)" : "", vertex_count, first_vertex, jumps, median, first_vertex + first_jump, h.ndc[2 * first_jump], h.ndc[2 * first_jump + 1], ndc[2 * first_jump], ndc[2 * first_jump + 1], p[0], p[1], p[2], p[3], inputs);
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

    // Detail: only once a key has also been seen sane (i.e. intermittent)
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

      REXLOG_WARN("[sst-native] spike? VS {:016X} prim {} {} draw {} vertices from {} (buffer {:08X}{}): {} of {} vertices far outside the view, first vertex {} at clip ({} {} {} {});{} [key seen {} times, {} flagged]",
                  vs_entry->hash, call.prim, call.indexed ? "indexed" : "auto", vertex_count, first_vertex, stream0, reads_morph ? ", morph output" : "", extreme, exported, first_vertex + first, p[0], p[1], p[2], p[3], inputs, e.checked, e.flagged);
    }
  }

  // Morph consumers (faces): compare each vertex position with the same draw's previous frame. Faces move a little per frame; a vertex that suddenly jumps far compared with the mesh size is a spike. Logs what it jumped to and when the morph output it reads was last written
  void Renderer::CheckMorphJumps(const ShaderEntry *vs_entry, const DrawCall &call, const std::vector<gfx::InputElement> &layout, const uint8_t *host, uint32_t host_inputs, uint32_t first_vertex, uint32_t vertex_count, uint32_t stream0)
  {
    // Was the morph output changed between the morph and this draw?
    if (auto it = morph_writes_.find(stream0); it != morph_writes_.end() && it->second.hash && it->second.frame == submitted_)
    {
      const uint64_t now = XXH3_64bits(physical_membase_ + stream0, it->second.size);

      if (now != it->second.hash)
      {
        ++stats_.morph_modified;

        if (stats_.morph_modified_logged++ < 10)
          REXLOG_WARN("[sst-native] morph output {:08X} ({} bytes) changed between the morph ({} draws ago) and the draw reading it (VS {:016X})", stream0, it->second.size, draw_seq_ - it->second.draw, vs_entry->hash);
      }
    }

    int pos = -1;

    for (const auto &e : layout)
    {
      const D3D12_INPUT_ELEMENT_DESC &d = e.d3d;

      if (d.InputSlot == 0 && d.SemanticName && std::strcmp(d.SemanticName, "POSITION") == 0 && d.SemanticIndex == 0)
        pos = int(d.AlignedByteOffset / 16);
    }

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

          REXLOG_WARN("[sst-native] morph jump: VS {:016X} buffer {:08X} draw {} vertices from {}: {} vertices moved > 25% of the mesh size {:.2f} (worst {:.2f}); vertex {} ({:.3f} {:.3f} {:.3f}) -> ({:.3f} {:.3f} {:.3f}); previous draw of this mesh {} frames / {} draws ago; {}",
                      vs_entry->hash, stream0, vertex_count, first_vertex, jumps, h.extent, worst, first_vertex + first, a[0], a[1], a[2], b[0], b[1], b[2], submitted_ - h.frame, draw_seq_ - h.draw, writes);
        }
      }
    }

    h.positions.swap(cur);
    h.extent = extent;
    h.frame = submitted_;
    h.draw = draw_seq_;
    (void)call;
  }

  // F9 capture: GPU vertex positions (stream output) vs the CPU
  // For one frame, every drawn draw is drawn a second time as a point list into a stream-output buffer with a pipeline that has no rasterizer, capturing SV_Position exactly as the GPU computed it from the translated shader. The original microcode is run on the CPU over the same vertices; at the
  // end of the frame both are compared. Disagreeing draws are translation (or GPU input) bugs; agreeing draws with vertices far outside the view are data
  bool Renderer::EnsureCaptureResources()
  {
    // Stream output: D3D12 only
    if (gfx_->backend() != gfx::Backend::kD3D12)
      return false;

    if (so_buffer_)
      return true;

    const size_t total = kSoDataBytes + kSoCounterBytes;
    so_buffer_ = gfx_->CreateBuffer(total, gfx::HeapType::kDefault, D3D12_RESOURCE_STATE_COPY_DEST);
    so_readback_ = so_buffer_ ? gfx_->CreateBuffer(total, gfx::HeapType::kReadback, D3D12_RESOURCE_STATE_COPY_DEST) : nullptr;
    so_zero_ = so_readback_ ? gfx_->CreateBuffer(kSoCounterBytes, gfx::HeapType::kUpload, D3D12_RESOURCE_STATE_GENERIC_READ) : nullptr;

    if (!so_zero_)
    {
      so_buffer_.reset();
      so_readback_.reset();
      return false;
    }

    std::memset(so_zero_->cpu, 0, kSoCounterBytes);
    return true;
  }

  void *Renderer::GetSoPipeline(const ShaderVariant *vs, uint64_t layout_hash, const std::vector<gfx::InputElement> &layout)
  {
#if defined(_WIN32)
    if (gfx_->backend() != gfx::Backend::kD3D12)
      return nullptr; // (stream output: D3D12 only)

    const uint64_t key = XXH3_64bits(&vs, sizeof(vs)) ^ layout_hash;

    if (auto it = so_pipelines_.find(key); it != so_pipelines_.end())
      return it->second.get();

    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;

    for (const gfx::InputElement &e : layout)
      elements.push_back(e.d3d);

    static const D3D12_SO_DECLARATION_ENTRY kEntry = {0, "SV_Position", 0, 0, 4, 0};
    static const UINT kStride = 16;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = gfx::d3d12::NativeRootSignature(*gfx_, gfx::Layout::kDraw);
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
    d.InputLayout = {elements.data(), UINT(elements.size())};
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = kOutputFormat;
    d.DSVFormat = kDepthFormat;
    d.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    const HRESULT hr = gfx::d3d12::NativeDevice(*gfx_)->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));

    if (FAILED(hr))
    {
      if (LogOnce(0xCA00000000000000ull ^ key))
        REXLOG_WARN("[sst-native] capture: stream-output pipeline failed ({:08X})", uint32_t(hr));

      so_pipelines_.emplace(key, nullptr);
      return nullptr;
    }

    std::shared_ptr<void> owned(pso, [](void *p)
                                { static_cast<ID3D12PipelineState *>(p)->Release(); });
    return so_pipelines_.emplace(key, std::move(owned)).first->second.get();
#else
    (void)vs;
    (void)layout_hash;
    (void)layout;
    return nullptr;
#endif
  }

  void Renderer::CaptureDraw(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, const ShaderVariant *vs, const std::vector<gfx::InputElement> &layout, uint64_t layout_hash, uint32_t first_vertex, uint32_t vertex_count, uint32_t stream0, bool reads_morph, float half_x, float half_y)
  {
    if (captured_.size() >= kSoMaxDraws || so_used_ + size_t(vertex_count) * 16 > kSoDataBytes)
      return;

    void *pso = GetSoPipeline(vs, layout_hash, layout);

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
#if defined(_WIN32)
    ID3D12GraphicsCommandList *list = gfx::d3d12::NativeList(*list_);
    const D3D12_GPU_VIRTUAL_ADDRESS so = so_buffer_->gpu;
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW view = {so + c.so_offset, UINT64(vertex_count) * 16, so + kSoDataBytes + captured_.size() * 16};
    list->SOSetTargets(0, 1, &view);
    list->SetPipelineState(static_cast<ID3D12PipelineState *>(pso));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    NoteDraw(0, 2);
    list->DrawInstanced(vertex_count, 1, 0, 0);
    list->SOSetTargets(0, 0, nullptr);
#endif
    so_used_ += size_t(vertex_count) * 16;
    captured_.push_back(std::move(c));
  }

  void Renderer::FinishCapture()
  {
    const uint8_t *data = so_readback_->Map(0, kSoDataBytes + kSoCounterBytes);

    if (!data)
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
          diff = std::max({std::fabs(gp[0] - cx), std::fabs(gp[1] - cy), std::fabs(gp[2] - cp[2]), std::fabs(gp[3] - cp[3])}) / scale;
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
          REXLOG_WARN("[sst-native] capture MISMATCH: draw #{} VS {:016X} prim {} {} {} vertices from {} (buffer {:08X}{}): {} of {} vertices differ (worst {:.4g}); vertex {}: GPU ({} {} {} {}) CPU ({} {} {} {});{}",
                      c.seq, c.vs_hash, c.prim, c.indexed ? "indexed" : "auto", c.vertex_count, c.first_vertex, c.stream0, c.morph ? ", face" : "", bad, n, worst, c.first_vertex + first_bad, gp[0], gp[1], gp[2], gp[3], cp[0] + c.half_x * cp[3], cp[1] + c.half_y * cp[3], cp[2], cp[3], c.detail);
        }
      }
    }

    if (capture_quiet_)
    {
      if (mismatched && capture_frames_logged_++ < 300)
        REXLOG_WARN("[sst-native] capture: frame {}: {} of {} draws differ between GPU and CPU", submitted_, mismatched, compared);

      so_readback_->Unmap();
      return;
    }

    std::sort(reach.begin(), reach.end(), [](const Reach &a, const Reach &b)
              { return a.extent > b.extent; });

    REXLOG_INFO("[sst-native] capture: {} draws, {} vertices from the GPU; {} compared with the CPU, {} differ; {} draws with non-finite GPU positions. Draws reaching farthest outside the view (|x/w| or |y/w|):", captured_.size(), vertices, compared, mismatched, gpu_nan);

    for (size_t i = 0; i < reach.size() && i < 25; ++i)
    {
      const CapturedDraw &c = captured_[reach[i].index];
      REXLOG_INFO("[sst-native]   {:.1f}: draw #{} VS {:016X} prim {} {} vertices from {} (buffer {:08X}{})", reach[i].extent, c.seq, c.vs_hash, c.prim, c.vertex_count, c.first_vertex, c.stream0, c.morph ? ", face" : "");
    }

    so_readback_->Unmap();
  }

  // --sst_native_trace_vs / --sst_native_skip_vs: log the draws of one vertex shader, or skip them. False: skip this draw
  bool Renderer::DebugFilterDraw(DrawContext &d)
  {
    const ShaderEntry *vs_entry = d.vs_entry;

    {
      auto parse = [](const std::string &text) -> uint64_t
      {
        return text.empty() ? 0 : std::strtoull(text.c_str(), nullptr, 16);
      };

      static std::string trace_text, skip_text;
      static uint64_t trace_hash = 0, skip_hash = 0;

      if (const std::string t = REXCVAR_GET(sst_native_trace_vs); t != trace_text)
        trace_text = t, trace_hash = parse(t);

      if (const std::string t = REXCVAR_GET(sst_native_skip_vs); t != skip_text)
        skip_text = t, skip_hash = parse(t);

      if (skip_hash && vs_entry->hash == skip_hash)
      {
        ++stats_.skip_cull;
        return false;
      }

      trace_draw_ = trace_hash && vs_entry->hash == trace_hash && traced_draws_ < 40;

      if (trace_draw_)
      {
        ++traced_draws_;
        draw_textures_.clear();
      }
    }

    return true;
  }

  // --sst_native_debug_bisect: a hidden draw still runs in full, it just writes no colour (so nothing else changes)
  bool Renderer::DebugBisectHides(DrawContext &d)
  {
    const ShaderEntry *ps_entry = d.ps_entry;
    bool bisect_hide = false;

    if (ps_entry && REXCVAR_GET(sst_native_debug_bisect))
    {
      ShaderBisect &bisect = Bisect();

      if (!bisect.active && bisect.recent.size() < 4096)
        bisect.recent.insert(ps_entry->hash);

      bisect_hide = bisect.Hidden(ps_entry->hash);
    }

    return bisect_hide;
  }

  // Checks on the decoded vertices of draws reading morph output (--sst_native_diagnostics, --sst_native_check_vs), late-write tracking (--sst_native_check_late_writes), a sample of the first indexed draws, and --sst_native_trace_vs's report. `host`: the decoded vertices
  void Renderer::DebugAfterVertexDecode(DrawContext &d, const uint8_t *host)
  {
    const uint8_t *base = d.base;
    const DrawCall &call = d.call;
    const uint32_t dev = d.dev;
    const uint32_t vs_obj = d.vs_obj;
    const ShaderEntry *vs_entry = d.vs_entry, *ps_entry = d.ps_entry;
    const auto &layout = d.layout;
    const auto &host_elements = d.host_elements;
    const auto *fetch_streams = d.fetch_streams;
    const uint32_t fetch_stream_count = d.fetch_stream_count;
    const auto *resolved = d.resolved;
    const uint32_t first_vertex = d.first_vertex, vertex_count = d.vertex_count, vmin = d.vmin, vmax = d.vmax;
    const bool reads_morph = d.reads_morph;
    const uint8_t *ib_src = d.ib_src;
    const size_t ib_src_bytes = d.ib_src_bytes;
    const auto &indices = d.indices;

    if (reads_morph && REXCVAR_GET(sst_native_diagnostics))
      CheckMorphJumps(vs_entry, call, layout, host, uint32_t(host_elements.size()), first_vertex, vertex_count, fetch_stream_count ? uint32_t(resolved[0].src - physical_membase_) : 0);

    if (reads_morph && REXCVAR_GET(sst_native_diagnostics))
    {
      const float *f = reinterpret_cast<const float *>(host);
      const size_t n = size_t(vertex_count) * host_elements.size();
      uint32_t bad = 0;
      size_t first = 0;

      for (size_t i = 0; i < n; ++i)
      {
        if (host_elements[i % host_elements.size()].fetch.out != FetchElement::Out::kFloat)
          continue;

        for (uint32_t j = 0; j < 4; ++j)
        {
          if (!std::isfinite(f[4 * i + j]) || std::fabs(f[4 * i + j]) > 1.0e5f)
          {
            if (!bad++)
              first = 4 * i + j;
          }
        }
      }

      if (bad)
      {
        ++stats_.morph_consumer_bad;

        if (stats_.morph_consumer_bad_logged++ < 8)
          REXLOG_WARN("[sst-native] morph consumer VS {:016X}: {} implausible input values (first at vertex {} input {} = {}), vertices {}+{}", vs_entry->hash, bad, first / (4 * host_elements.size()), (first / 4) % host_elements.size(), f[first], first_vertex, vertex_count);
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
      CheckVsPositions(base, call, vs_obj, vs_entry, first_vertex, vertex_count, fetch_stream_count ? uint32_t(resolved[0].src - physical_membase_) : 0, reads_morph, reinterpret_cast<const float *>(host), uint32_t(host_elements.size()));

    // Sample of what the first few indexed 3D draws feed the GPU, to check positions / indices / matrices by eye
    if (call.indexed && stats_.sample_logged < 6 && !host_elements.empty() && !indices.empty())
    {
      ++stats_.sample_logged;
      const float *f = reinterpret_cast<const float *>(host);
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

      REXLOG_INFO("[sst-native] sample draw: VS {:016X} prim {} base {} start {} count {} range {}-{}; indices{}; first input of first vertices{}; c0-c3 [{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}]",
                  vs_entry->hash, call.prim, call.base_vertex, call.start, call.count, vmin, vmax, idx, verts, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
    }

    if (trace_draw_)
    {
      const float *f = reinterpret_cast<const float *>(host);
      std::string streams, verts;

      for (uint32_t i = 0; i < fetch_stream_count; ++i)
        streams += fmt::format(" [slot {} phys {:08X} stride {} size {}{}]", fetch_streams[i].slot, resolved[i].phys, fetch_streams[i].stride, resolved[i].size, resolved[i].phys && resolved[i].src != physical_membase_ + resolved[i].phys ? " copy" : "");

      for (uint32_t v = 0; v < std::min<uint32_t>(3, vertex_count); ++v)
      {
        verts += " {";

        for (size_t e = 0; e < std::min<size_t>(host_elements.size(), 8); ++e)
        {
          const float *p = f + (size_t(v) * host_elements.size() + e) * 4;
          verts += fmt::format(" ({:.4g} {:.4g} {:.4g} {:.4g})", p[0], p[1], p[2], p[3]);
        }

        verts += " }";
      }

      // The vertex shader run on the CPU: where the vertices end up (clip space)
      std::string clip = " (CPU run failed)";

      {
        uint32_t bools[8], fetch[192], loops[32];
        check_constants_.resize(1024);

        if (!LoadVsState(base, call.device, vs_obj, vs_entry, check_ucode_, check_constants_.data(), bools, fetch, loops, nullptr) && vertex_count <= 65536)
        {
          check_indices_.resize(vertex_count);

          for (uint32_t v = 0; v < vertex_count; ++v)
            check_indices_[v] = first_vertex + v;

          check_positions_.resize(size_t(vertex_count) * 4);
          VsConstants vc;
          vc.float_constants = check_constants_.data();
          vc.bool_constants = bools;
          vc.loop_constants = loops;
          vc.fetch_constants = fetch;
          MemexportStats ignored;

          if (memexport_->RunPositions(check_ucode_.data(), uint32_t(check_ucode_.size()), vc, check_indices_.data(), vertex_count, check_positions_.data(), &ignored))
          {
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, w0 = 1e30f, w1 = -1e30f;
            uint32_t bad = 0;

            for (uint32_t v = 0; v < vertex_count; ++v)
            {
              const float *p = &check_positions_[4 * size_t(v)];

              if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[3]) || p[3] <= 1e-6f)
              {
                ++bad;
                continue;
              }

              x0 = std::min(x0, p[0] / p[3]), x1 = std::max(x1, p[0] / p[3]);
              y0 = std::min(y0, p[1] / p[3]), y1 = std::max(y1, p[1] / p[3]);
              w0 = std::min(w0, p[3]), w1 = std::max(w1, p[3]);
            }

            const float *p = check_positions_.data();
            clip = fmt::format(" screen x {:.3g}..{:.3g} y {:.3g}..{:.3g} w {:.3g}..{:.3g}, {} vertices non-finite or behind; vertex 0 clip ({:.4g} {:.4g} {:.4g} {:.4g}); bone constants c35 ({:.4g} {:.4g} {:.4g} {:.4g})", x0, x1, y0, y1, w0, w1, bad, p[0], p[1], p[2], p[3], check_constants_[35 * 4], check_constants_[35 * 4 + 1], check_constants_[35 * 4 + 2], check_constants_[35 * 4 + 3]);
          }
        }
      }

      REXLOG_INFO("[sst-native] trace VS {:016X} PS {:016X}: frame {} prim {} indexed {} start {} count {} base {} vertices {}+{} (index range {}-{}); streams{}; textures{}; first vertices{}; CPU-run{}", vs_entry->hash, ps_entry ? ps_entry->hash : 0, submitted_ + 1, call.prim, call.indexed, call.start, call.count, call.base_vertex, first_vertex, vertex_count, vmin, vmax, streams, draw_textures_, verts, clip);
    }
  }

  // --sst_native_debug_markers: name the draw by its shaders and state (RenderDoc / PIX Event Browser)
  void Renderer::DebugMarkDraw(DrawContext &d)
  {
    if (!REXCVAR_GET(sst_native_debug_markers))
      return;

    const std::string label = fmt::format("vs {:016X} ps {:016X} depth {:08X} colour {:08X} blend {:08X} bias {}", d.vs_entry ? d.vs_entry->hash : 0, d.ps_entry ? d.ps_entry->hash : 0, d.depthcontrol, d.colorcontrol, d.key.blend, d.key.depth_bias);
    list_->Marker(label);
  }

  // F9 capture: the draw's inputs and its GPU-transformed vertices (stream output), compared with the CPU's at the frame's end
  void Renderer::DebugAfterDraw(DrawContext &d)
  {
    if (!capture_active_ || d.host_elements.empty() || d.call.up_data)
      return;

    const auto &host_elements = d.host_elements;
    const auto *resolved = d.resolved;
    const auto *fetch_streams = d.fetch_streams;
    const uint32_t fetch_stream_count = d.fetch_stream_count, first_vertex = d.first_vertex;
    std::string &capture_detail = d.capture_detail;

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
    CaptureDraw(d.base, d.call, d.vs_obj, d.vs_entry, d.vs, d.layout, d.key.layout_hash, d.first_vertex, d.vertex_count, d.fetch_stream_count ? uint32_t(d.resolved[0].src - physical_membase_) : 0, d.reads_morph, d.half_x, d.half_y);
  }
}