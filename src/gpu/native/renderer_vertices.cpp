// Native renderer: a draw's vertex input: the layout from the vertex shader's fetches, the indices, the vertex streams and their uploads

#include "gpu/native/draw_context.h"
#include "gpu/native/guest_watch.h"
#include "gpu/native/renderer_debug.h"
#include "gpu/native/write_watch.h"

namespace sst::native
{
  using namespace renderer_detail;

  // Vertex inputs, from the vertex shader's *live* fetch instructions: the Xbox D3D patches format / offset / stride / fetch constant / swizzle into the microcode when it binds the declaration (draw-time flush), so this is exactly what the GPU would fetch. Each input is decoded on the CPU (DecodeElement) into 4 x 32 bits; swizzle changes the patch made relative to the original instruction become the component order
  bool Renderer::BuildVertexLayout(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const uint32_t dev = d.dev;
    const uint32_t vs_obj = d.vs_obj;
    const ShaderEntry *vs_entry = d.vs_entry;
    using FetchStream = DrawContext::FetchStream;
    using HostElement = DrawContext::HostElement;
    FetchStream *fetch_streams = d.fetch_streams;
    uint32_t &fetch_stream_count = d.fetch_stream_count;
    std::vector<HostElement> &host_elements = d.host_elements;
    std::vector<gfx::InputElement> &layout = d.layout;
    bool &computed_fetch = d.computed_fetch;
    int &position_element = d.position_element;
    std::string &capture_detail = d.capture_detail;
    uint64_t *layout_hash_input = d.layout_hash_input;
    uint32_t &layout_words = d.layout_words;
    constexpr size_t kLayoutHashWords = DrawContext::kLayoutHashWords;
    d.clk.Switch(kPhInputs);
    const uint32_t live_ucode = ToVirtual(be32(base, vs_obj + 0x20));

    if (!PlausibleAddress(live_ucode) || vs_entry->container.size() < 16)
    {
      ++stats_.skip_decl;
      return false;
    }

    const uint32_t virtual_size = (uint32_t(vs_entry->container[4]) << 24) | (uint32_t(vs_entry->container[5]) << 16) | (uint32_t(vs_entry->container[6]) << 8) | vs_entry->container[7];

    auto stream_index = [&](uint32_t slot, uint32_t stride) -> int
    {
      for (uint32_t i = 0; i < fetch_stream_count; ++i)
      {
        if (fetch_streams[i].slot == slot && fetch_streams[i].stride == stride)
          return int(i);
      }

      if (fetch_stream_count == kMaxStreams)
        return -1;

      fetch_streams[fetch_stream_count] = {slot, stride};
      return int(fetch_stream_count++);
    };

    std::vector<sst::xenos::VertexInput> inputs = vs_entry->vertex_inputs;

    std::sort(inputs.begin(), inputs.end(), [](const auto &x, const auto &y)
              { return x.address < y.address; });

    // Shaders whose full vfetches don't fetch with the plain vertex index (r0.x) - e.g. particle quads reading one record per 4 vertices - get their element indices from running the shader on the CPU
    uint32_t full_fetch_addr = 0;
    layout.reserve(inputs.size());
    host_elements.reserve(inputs.size());
    int full_stream = -1; // the last full vfetch (mini fetches share its constant)

    // The live fetch instructions. D3D's bind-time patch rewrites whole vfetch instructions - destination register included - and may assign the declaration's elements to the fetch slots in a different order than the container lists them. So each input is matched to the live instruction that writes the same register as the original instruction did, not to the one at the same address
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
      {
        if (((swizzle_word >> (3 * i)) & 7) != 7)
          m |= 1u << i;
      }

      return m;
    };

    bool remapped = false;

    for (const auto &in : inputs)
    {
      D3D12_INPUT_ELEMENT_DESC element = {};
      element.SemanticName = UsageSemantic(in.usage);
      element.SemanticIndex = in.usage_index;
      element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

      if (!element.SemanticName)
        continue;

      const LiveFetch *self = nullptr;

      for (const LiveFetch &lf : live_fetches)
      {
        if (lf.addr == in.address)
          self = &lf;
      }

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

      // Left unpatched by D3D (format 0, stride 0): laid out from the declaration below
      const bool unpatched = (l0 & 0x1F) == 0 && ((l1 >> 16) & 0x3F) == 0 && (l2 & 0xFF) == 0;

      // Its full fetch (a mini fetch uses the last full fetch before it)
      const LiveFetch *full = nullptr;

      if ((l0 & 0x1F) == 0 && !((l1 >> 30) & 1))
      {
        full = match;
      }
      else
      {
        for (const LiveFetch &lf : live_fetches)
        {
          if (lf.addr < match->addr && (lf.l0 & 0x1F) == 0 && !((lf.l1 >> 30) & 1) && (!full || lf.addr > full->addr))
            full = &lf;
        }
      }

      if (full && !unpatched)
      {
        const uint32_t slot = ((full->l0 >> 20) & 0x1F) * 3 + ((full->l0 >> 25) & 3);
        full_stream = stream_index(slot, (full->l2 & 0xFF) * 4);
        full_fetch_addr = full->addr;

        // Source register bits 5-10 (+ relative bit 11), source swizzle 30-31
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

      // A vertex fetch D3D left unpatched (format 0, stride 0 - as compiled): seen on the movie quad (CriMovieControl, DrawVerticesUP). Its layout comes from the bound vertex declaration instead
      bool from_decl = false;

      if (unpatched)
      {
        DeclInput di;

        if (FindDeclInput(base, dev, in.usage, in.usage_index, di))
        {
          FetchElement &f = h.fetch;
          f.format = di.format;
          f.offset = di.offset;
          f.out = !UsageIsUint(in.usage) ? FetchElement::Out::kFloat : in.usage == 2 ? FetchElement::Out::kUint
                                                                                     : FetchElement::Out::kFloatBits;

          // Input component c reads element component c; missing ones read 0 (w: 1), as D3D's own patch fills them
          for (int c = 0; c < 4; ++c)
          {
            f.sel[c] = uint8_t(c < int(di.components) ? c : c == 3 ? 5
                                                                   : 4);
          }

          h.stream = stream_index(95 - di.stream, di.stride);
          h.fetch_addr = in.address;
          from_decl = h.stream >= 0;

          if (from_decl && LogOnce(0xCD00000000000000ull ^ vs_entry->hash ^ in.address))
            REXLOG_INFO("[sst-native] VS {:016X} {}{}: unpatched vertex fetch, layout from the vertex declaration (format {}, offset {}, stride {}, stream {})", vs_entry->hash, element.SemanticName, element.SemanticIndex, di.format, di.offset, di.stride, di.stream);
        }
      }

      if (!from_decl && (l0 & 0x1F) == 0) // vertex fetch opcode
      {
        const int32_t offset = int32_t(l2 << 1) >> 9; // bits 8-30, signed, in dwords
        FetchElement &f = h.fetch;
        f.format = (l1 >> 16) & 0x3F;
        f.is_signed = (l1 >> 12) & 1;
        f.normalized = !((l1 >> 13) & 1);
        f.no_zero = (l1 >> 14) & 1;
        f.exp_adjust = int32_t(l1 << 2) >> 26;

        // The shader was translated from the original instruction: destination i reads input component o_i there, and hardware component l_i after the patch, so input component o_i must hold hardware component l_i
        bool conflict = false;
        uint8_t set = 0;

        for (int i = 0; i < 4; ++i)
        {
          const uint32_t o = (o1 >> (3 * i)) & 7, l = (l1 >> (3 * i)) & 7;

          if (o >= 4)
            continue; // the shader doesn't read the input for this destination

          // Patched destination 6 (invalid) / 7 (keep: the register's previous value, zero for a fetch into a fresh register) read as 0
          const uint8_t hw = uint8_t(l >= 6 ? 4 : l);

          if ((set >> o) & 1)
            conflict |= f.sel[o] != hw;

          f.sel[o] = hw;
          set |= uint8_t(1u << o);
        }

        if (conflict && LogOnce(0xE100000000000000ull | (uint64_t(o1) << 24) | (l1 & 0xFFF)))
          REXLOG_WARN("[sst-native] {}{}: conflicting swizzles (original {:03X} patched {:03X})", element.SemanticName, element.SemanticIndex, o1 & 0xFFF, l1 & 0xFFF);

        f.out = !UsageIsUint(in.usage) ? FetchElement::Out::kFloat : in.usage == 2 ? FetchElement::Out::kUint
                                                                                   : FetchElement::Out::kFloatBits;

        if (DeclFormatSize(f.format) && offset >= 0 && full_stream >= 0)
        {
          f.offset = uint32_t(offset) * 4;
          h.stream = full_stream;
          h.fetch_addr = full_fetch_addr;
        }

        if (capture_active_)
          capture_detail += fmt::format(" {}{}[live {:08X} {:08X} {:08X} orig {:08X} fmt {} sel {}{}{}{} off {}]", element.SemanticName, element.SemanticIndex, l0, l1, l2, o1, f.format, f.sel[0], f.sel[1], f.sel[2], f.sel[3], offset * 4);

        if (LogOnce(0xE200000000000000ull | (uint64_t(in.usage) << 40) | (uint64_t(l1 & 0x3FFFFFFF))))
          REXLOG_INFO("[sst-native] vertex input {}{}: format {} {} {} exp {} swizzle {:03X}->{:03X} (sel {}{}{}{}) offset {} stride {}", element.SemanticName, element.SemanticIndex, f.format, f.is_signed ? "signed" : "unsigned", f.normalized ? "norm" : "int", f.exp_adjust, o1 & 0xFFF, l1 & 0xFFF, f.sel[0], f.sel[1], f.sel[2], f.sel[3], offset * 4, (l2 & 0xFF) * 4);
      }

      if (h.stream < 0 && LogOnce(0xE000000000000000ull | (uint64_t(vs_entry->hash & 0xFFFFFFFF) << 16) | in.address))
        REXLOG_WARN("[sst-native] VS {:016X} input {}{} at {}: unusable fetch {:08X} {:08X} {:08X}", vs_entry->hash, element.SemanticName, element.SemanticIndex, in.address, l0, l1, l2);

      element.Format = UsageIsUint(in.usage) ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;

      if (h.stream >= 0)
      {
        element.InputSlot = 0;
        element.AlignedByteOffset = UINT(16 * host_elements.size());

        if (in.usage == 0 && position_element < 0)
          position_element = int(host_elements.size());

        host_elements.push_back(h);
      }
      else
      {
        element.InputSlot = kZeroInputSlot;
        element.AlignedByteOffset = 0;
      }

      // Its shader input location: the input's index in the shader's own list (the translator numbers them so, for Vulkan)
      uint32_t location = 0;

      for (size_t i = 0; i < vs_entry->vertex_inputs.size(); ++i)
      {
        const auto &v = vs_entry->vertex_inputs[i];

        if (v.address == in.address && v.usage == in.usage && v.usage_index == in.usage_index)
        {
          location = uint32_t(i);
          break;
        }
      }

      layout.push_back({element, location});

      if (layout_words + 1 < kLayoutHashWords)
        layout_hash_input[layout_words++] = (uint64_t(in.usage) << 56) | (uint64_t(in.usage_index) << 48) | (uint64_t(element.Format) << 40) | (uint64_t(element.InputSlot) << 32) | element.AlignedByteOffset;
    }

    if (remapped)
    {
      ++stats_.fetch_remapped;

      if (LogOnce(0xCC00000000000000ull ^ vs_entry->hash))
        REXLOG_INFO("[sst-native] VS {:016X}: the bound declaration moved inputs to other fetch slots; inputs matched to the live fetches by destination register", vs_entry->hash);
    }

    d.clk.Switch(kPhOther);
    return true;
  }

  // Indices, decoded with the index buffer's own swap mode: DrawIndexedVertices puts (Common >> 29) & 3 into the index DMA's swap field (0 none, 1 8in16, 2 8in32, 3 16in32), so 16-bit indices are not always 8in16. Also the vertex range they reference
  bool Renderer::ReadIndices(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const DrawCall &call = d.call;
    const uint32_t dev = d.dev;
    const bool convert = d.convert;
    PipelineKey &key = d.key;
    std::vector<uint32_t> &indices = d.indices;
    const uint8_t *&ib_src = d.ib_src;
    size_t &ib_src_bytes = d.ib_src_bytes;
    bool &index32 = d.index32;
    uint32_t &vmin = d.vmin, &vmax = d.vmax;
    GeoEntry *&ientry = d.ientry;
    uint64_t &ihash = d.ihash;
    bool &index_hit = d.index_hit;
    d.clk.Switch(kPhIndices);

    // (the geometry cache records copies)
    if (!recording_)
      BeginFrame();

    if (call.indexed)
    {
      const uint32_t ib = be32(base, dev + kDevIndices);

      if (!ib || call.count > kMaxDrawIndices || !GuestReadable(ib, 0x1C))
      {
        ++stats_.skip_data;
        return false;
      }

      const uint32_t common = be32(base, ib);
      index32 = (common >> 31) & 1;
      const uint32_t index_endian = (common >> 29) & 3;
      const uint32_t ib_addr = ToVirtual(be32(base, ib + 0x18) & ~3u);

      if (!PlausibleAddress(ib_addr))
      {
        ++stats_.skip_data;
        return false;
      }

      if (LogOnce(0xC100000000000000ull | (index32 ? 4 : 0) | index_endian))
        REXLOG_INFO("[sst-native] index buffer: {}-bit, swap mode {} (Common {:08X})", index32 ? 32 : 16, index_endian, common);

      // Swap whole dwords around the referenced range, then read little-endian
      const uint32_t isz = index32 ? 4 : 2;
      const uint32_t first_byte = call.start * isz;
      const uint32_t aligned_begin = first_byte & ~3u;
      const uint32_t aligned_end = (first_byte + call.count * isz + 3) & ~3u;

      // Freed by the game while this draw waited (a model loading): skip it
      if (!GuestReadable(ib_addr + aligned_begin, aligned_end - aligned_begin))
      {
        ++stats_.skip_data;

        if (LogOnce(0xC200000000000000ull))
          REXLOG_WARN("[sst-native] index data at {:08X} is no longer mapped (freed while the draw was queued): draws like this are skipped", ib_addr + aligned_begin);

        return false;
      }

      ib_src = GuestPtr(base, ib_addr + aligned_begin);
      ib_src_bytes = aligned_end - aligned_begin;
      const uint32_t restart = index32 ? 0xFFFFFFFFu : 0xFFFFu;
      const bool strip = call.prim == 6 || call.prim == 3;

      // Geometry cache (see the vertex data step): converted indices and their range stay in video memory while the guest index data is unchanged
      if (!convert && GeoCacheUsable())
      {
        const uint64_t words[5] = {ib_addr, call.start, call.count, (uint64_t(index32) << 8) | (uint64_t(index_endian) << 4) | uint64_t(strip), 0x1D1D1D1Dull};
        ientry = &geo_entries_[XXH3_64bits(words, sizeof(words))];

        if (ientry->dynamic)
        {
          ientry = nullptr;
        }
        else if (GeoTrusted(*ientry))
        {
          ihash = ientry->hash;
          index_hit = true;
          vmin = ientry->vmin;
          vmax = ientry->vmax;
          ++stats_.geo_trusted;
        }
        else
        {
          // Write watch (physical views only): stamp, arm, then hash, a write after the stamp is either in the data hashed or changes the stamp
          const uint32_t ib_phys = ib_addr & 0x1FFFFFFFu;
          const bool watch = ib_addr >= 0xA0000000u && ib_addr < 0xE0000000u && !ientry->no_watch && GeoWatchUsable();
          const uint32_t stamp = watch ? write_watch::Stamp(ib_phys + aligned_begin, aligned_end - aligned_begin) : 0;

          if (watch && GeoWatchTrusted(*ientry, stamp))
          {
            ihash = ientry->hash;
            index_hit = true;
            vmin = ientry->vmin;
            vmax = ientry->vmax;
            ientry->checked_frame = submitted_;
            ++stats_.geo_trusted;
            ++stats_.geo_watch_trusted;
          }
          else
          {
            if (watch)
            {
              write_watch::Arm(ib_phys + aligned_begin, aligned_end - aligned_begin);
              ++stats_.geo_watch_arms;
            }

            ihash = XXH3_64bits(ib_src, ib_src_bytes);

            if (ientry->hash == ihash && ientry->generation == geo_generation_ && ientry->size)
            {
              index_hit = true;
              vmin = ientry->vmin;
              vmax = ientry->vmax;
              GeoChecked(*ientry, true);
              GeoWatchVerified(*ientry, stamp, watch);
            }
            else
            {
              const bool changed = ientry->hash && ientry->hash != ihash;

              if (changed)
                GeoWatchChanged(*ientry, stamp, watch);
              else
                ientry->armed = false;

              if (changed && ++ientry->changes >= 2)
              {
                ientry->dynamic = true;
                ientry = nullptr;
              }
              else
              {
                GeoChecked(*ientry, false);
              }
            }
          }
        }
      }

      if (!index_hit)
      {
        std::vector<uint8_t> raw(aligned_end - aligned_begin);
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

        vmin = UINT32_MAX;
        vmax = 0;

        for (uint32_t v : indices)
        {
          if (strip && v == restart)
            continue;

          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
        }
      }

      if (vmin > vmax || vmax - vmin >= kMaxDrawVertices)
      {
        ++stats_.skip_data;
        return false;
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
        return false;
      }
    }

    return true;
  }

  // Vertex data: resolve each fetch constant to its guest memory (or the game thread's copy of it)
  bool Renderer::ResolveVertexStreams(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const DrawCall &call = d.call;
    const uint32_t dev = d.dev;
    using FetchStream = DrawContext::FetchStream;
    const FetchStream *fetch_streams = d.fetch_streams;
    const uint32_t fetch_stream_count = d.fetch_stream_count;
    bool &computed_fetch = d.computed_fetch;
    const auto &host_elements = d.host_elements;
    DrawContext::ResolvedStream *resolved = d.resolved;
    bool &reads_morph = d.reads_morph;
    d.clk.Switch(kPhVertices);

    // Immediate-mode data (BeginVertices/EndVertices): the game's particle helper writes one record per sprite but draws 4 quad-list vertices per record, so its shaders must derive the record index from the vertex index in a way the instruction scan above doesn't see (most likely by rewriting r0 before the vfetch). These draws are few and small: always take the element indices from running the shader on the CPU
    if (call.immediate && !host_elements.empty())
      computed_fetch = true;

    d.first_vertex = call.indexed ? call.base_vertex + d.vmin : call.start;
    d.vertex_count = d.vmax - d.vmin + 1;
    const uint32_t first_vertex = d.first_vertex, vertex_count = d.vertex_count;

    for (uint32_t i = 0; i < fetch_stream_count; ++i)
    {
      const FetchStream &fs = fetch_streams[i];
      uint32_t size;

      if (call.up_data && fs.slot == 95)
      {
        // DrawVerticesUP: stream 0 data comes from the caller's buffer
        if (!PlausibleAddress(call.up_data) || !GuestReadable(call.up_data, call.up_stride * (call.start + call.count)))
        {
          ++stats_.skip_data;
          return false;
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

          return false;
        }

        if (!GuestPhysicalReadable(phys, size))
        {
          ++stats_.skip_data;

          if (LogOnce(0xC300000000000000ull))
            REXLOG_WARN("[sst-native] vertex data at {:08X} is no longer mapped (freed while the draw was queued): draws like this are skipped", phys);

          return false;
        }

        WaitMemexports(phys, phys + size); // background morph output this draw reads

        for (const auto &[mb, me] : memexport_ranges_)
        {
          if (phys < me && phys + size > mb)
          {
            ++stats_.skip_memexport;
            return false;
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
                REXLOG_WARN("[sst-native] draw reads past morph output: fetch {:08X} stride {} vertices {}+{} (bytes {:08X}-{:08X})", phys, fs.stride, first_vertex, vertex_count, uint32_t(read_begin), uint32_t(read_end));
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
                  REXLOG_WARN("[sst-native] draw reads morph output written last frame, not this one (yet): fetch {:08X} stride {} vertices {}+{}", phys, fs.stride, first_vertex, vertex_count);

                break;
              }
            }
          }
        }

        resolved[i] = {physical_membase_ + phys, dw1 & 3, 0, phys};

        if (LogOnce(0xC200000000000000ull | (dw1 & 3)))
          REXLOG_INFO("[sst-native] vertex buffer swap mode {} (fetch {:08X} {:08X})", dw1 & 3, dw0, dw1);
      }

      resolved[i].size = size;
      const uint64_t end = (uint64_t(first_vertex) + vertex_count) * fs.stride;

      if (!fs.stride || (end > size && !computed_fetch))
      {
        ++stats_.skip_data;

        if (LogOnce(0xB000000000000000ull | fs.slot))
          REXLOG_WARN("[sst-native] draw reads past vertex buffer (fetch {} size {}, vertices {}+{} stride {})", fs.slot, size, first_vertex, vertex_count, fs.stride);

        return false;
      }

      // The game thread's copy of the start of this buffer from when the draw was issued (UI sprites: the game rewrites their vertices between draws, and read live a frame later they hold another sprite's). Used when it covers every vertex this draw reads (not for morph output: the renderer
      // writes that itself, after the game thread copied the buffer)
      if (resolved[i].phys && !computed_fetch && !reads_morph)
      {
        for (uint32_t c = 0; c < draw_vertex_copy_count_; ++c)
        {
          const GuestCopy &g = draw_vertex_copies_[c];

          if (g.begin == resolved[i].phys && uint64_t(g.end) - g.begin >= end)
          {
            resolved[i].src = g.data;
            resolved[i].size = std::min(size, g.end - g.begin);
            ++stats_.vertex_copies_used;
            break;
          }
        }
      }
    }

    return true;
  }

  // Decodes the referenced vertex range into one host stream (16 bytes per input) and binds it
  bool Renderer::UploadVertices(DrawContext &d)
  {
    const uint8_t *base = d.base;
    const DrawCall &call = d.call;
    using FetchStream = DrawContext::FetchStream;
    using HostElement = DrawContext::HostElement;
    using ResolvedStream = DrawContext::ResolvedStream;
    const FetchStream *fetch_streams = d.fetch_streams;
    const uint32_t fetch_stream_count = d.fetch_stream_count;
    const std::vector<HostElement> &host_elements = d.host_elements;
    const ResolvedStream *resolved = d.resolved;
    const uint32_t first_vertex = d.first_vertex, vertex_count = d.vertex_count;
    const bool computed_fetch = d.computed_fetch, reads_morph = d.reads_morph, rect_list = d.rect_list;
    const int position_element = d.position_element;
    const uint32_t vs_obj = d.vs_obj;
    const ShaderEntry *vs_entry = d.vs_entry;
    std::vector<uint32_t> &rect_indices = d.rect_indices;

    auto mark_vertices_changed = [&]
    {
      for (uint32_t i = 0; i < fetch_stream_count; ++i)
      {
        if (resolved[i].phys)
          guest_watch::VerticesChanged(resolved[i].phys, uint32_t(std::min<uint64_t>((uint64_t(first_vertex) + vertex_count) * fetch_streams[i].stride, 1u << 20)), submitted_ + 1);
      }
    };

    if (!host_elements.empty())
    {
      const uint32_t host_stride = uint32_t(16 * host_elements.size());

      // Geometry cache: decoded vertices stay in video memory while the guest data they came from is unchanged (checked by hash on every use). Data that keeps changing (morph output, dynamic buffers) is marked dynamic and decoded every time as before
      GeoEntry *ventry = nullptr;
      uint64_t vhash = 0;
      bool vhit = false;

      if (GeoCacheUsable() && !computed_fetch && !call.up_data && !reads_morph && !rect_list)
      {
        uint64_t words[4 * kMaxStreams + 8 * 16 + 4];
        uint32_t n = 0;
        words[n++] = first_vertex;
        words[n++] = vertex_count;
        words[n++] = host_stride;

        for (const HostElement &h : host_elements)
        {
          if (n + 3 > std::size(words))
            break;

          const FetchElement &f = h.fetch;
          words[n++] = (uint64_t(f.format) << 32) | (uint64_t(f.is_signed) << 24) | (uint64_t(f.normalized) << 16) | (uint64_t(f.no_zero) << 8) | uint64_t(uint8_t(f.out));
          words[n++] = (uint64_t(uint32_t(f.exp_adjust)) << 32) | f.offset;
          words[n++] = (uint64_t(f.sel[0]) << 56) | (uint64_t(f.sel[1]) << 48) | (uint64_t(f.sel[2]) << 40) | (uint64_t(f.sel[3]) << 32) | uint32_t(h.stream);
        }

        for (uint32_t i = 0; i < fetch_stream_count; ++i)
        {
          const uint64_t phys = resolved[i].phys;

          if (n + 2 <= std::size(words))
          {
            words[n++] = (phys << 8) | resolved[i].endian;
            words[n++] = fetch_streams[i].stride;
          }
        }

        const uint64_t key = XXH3_64bits(words, n * sizeof(uint64_t));
        ventry = &geo_entries_[key];

        if (ventry->dynamic)
        {
          ventry = nullptr;
          ++stats_.geo_dynamic;
          mark_vertices_changed();
        }
        else if (GeoTrusted(*ventry))
        {
          vhash = ventry->hash;
          vhit = true;
          ++stats_.geo_trusted;
        }
        else
        {
          // Write watch: the streams' referenced ranges (physical), stamped, armed, then hashed, a write after the stamp is either in the data hashed or changes the stamp
          bool watch = !ventry->no_watch && GeoWatchUsable();
          uint32_t stamp = 0;

          for (uint32_t i = 0; watch && i < fetch_stream_count; ++i)
          {
            if (!resolved[i].phys || resolved[i].src != physical_membase_ + resolved[i].phys) // (not the game thread's draw-time copy: it can be older than the last check)
              watch = false;
            else
              stamp += write_watch::Stamp(uint32_t(resolved[i].phys + uint64_t(first_vertex) * fetch_streams[i].stride), vertex_count * fetch_streams[i].stride) * (2 * i + 1);
          }

          if (watch && GeoWatchTrusted(*ventry, stamp))
          {
            vhash = ventry->hash;
            vhit = true;
            ventry->checked_frame = submitted_;
            ++stats_.geo_trusted;
            ++stats_.geo_watch_trusted;
          }
          else
          {
            if (watch)
            {
              for (uint32_t i = 0; i < fetch_stream_count; ++i)
                write_watch::Arm(uint32_t(resolved[i].phys + uint64_t(first_vertex) * fetch_streams[i].stride), vertex_count * fetch_streams[i].stride);

              ++stats_.geo_watch_arms;
            }

            // One hashing state, reused (creating one per draw allocated)
            static thread_local XXH3_state_t *st = XXH3_createState();
            XXH3_64bits_reset(st);

            for (uint32_t i = 0; i < fetch_stream_count; ++i)
              XXH3_64bits_update(st, resolved[i].src + size_t(first_vertex) * fetch_streams[i].stride, size_t(vertex_count) * fetch_streams[i].stride);

            vhash = XXH3_64bits_digest(st);

            if (ventry->hash == vhash && ventry->generation == geo_generation_ && ventry->size)
            {
              vhit = true;
              GeoChecked(*ventry, true);
              GeoWatchVerified(*ventry, stamp, watch);
            }
            else
            {
              const bool changed = ventry->hash && ventry->hash != vhash;

              if (changed)
              {
                GeoWatchChanged(*ventry, stamp, watch);
                mark_vertices_changed();
              }
              else
              {
                ventry->armed = false;
              }

              if (changed && ++ventry->changes >= 2)
              {
                ventry->dynamic = true; // keeps changing: stop caching it
                ventry = nullptr;
              }
              else
              {
                GeoChecked(*ventry, false);
              }
            }
          }
        }
      }

      if (vhit)
      {
        ++stats_.geo_vertex_hits;
        capture_host_ = nullptr;
        list_->SetVertexBuffer(0, GeoAddress(*ventry), vertex_count * host_stride, host_stride);
      }
      else
      {
        const Upload up = Allocate(size_t(vertex_count) * host_stride, 16);

        if (!up.cpu)
        {
          ++stats_.skip_data;
          return false;
        }

        uint32_t *out = reinterpret_cast<uint32_t *>(up.cpu);

        if (computed_fetch && ComputeFetchRecords(base, call, vs_obj, vs_entry, first_vertex, vertex_count))
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
            REXLOG_INFO("[sst-native] VS {:016X} fetches with a computed index (e.g. one record per particle quad); element indices taken from running it on the CPU", vs_entry->hash);
        }
        else
        {
          if (computed_fetch && LogOnce(0xCC00000000000000ull ^ vs_entry->hash))
            REXLOG_WARN("[sst-native] VS {:016X}: running it on the CPU for its fetch indices failed; vertices fetched in order instead", vs_entry->hash);

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

        DebugAfterVertexDecode(d, up.cpu);
        capture_host_ = capture_active_ ? reinterpret_cast<const float *>(up.cpu) : nullptr;
        gfx::GpuAddress vb_address = up.gpu;
        uint32_t vb_size = vertex_count * host_stride;

        if (rect_list)
        {
          // RECTLIST: each 3 vertices are corners of a rectangle; the GPU makes the 4th. Find the right-angle corner c (by the positions) and add the opposite corner a + b - c (all float inputs, which the full-screen passes' vertex shaders pass through or transform linearly)
          const uint32_t rects = vertex_count / 3;
          const uint32_t floats = host_stride / 4;
          const Upload ex = Allocate(size_t(std::max(rects, 1u)) * 4 * host_stride, 16);

          if (!ex.cpu || !rects)
          {
            ++stats_.skip_data;
            return false;
          }

          const float *src = reinterpret_cast<const float *>(up.cpu);
          float *dst = reinterpret_cast<float *>(ex.cpu);
          rect_indices.clear();

          for (uint32_t r = 0; r < rects; ++r)
          {
            const float *v[3] = {src + size_t(3 * r) * floats, src + size_t(3 * r + 1) * floats, src + size_t(3 * r + 2) * floats};
            uint32_t corner = 0;

            if (position_element >= 0)
            {
              float best = FLT_MAX;

              for (uint32_t i = 0; i < 3; ++i)
              {
                const float *p = v[i] + 4 * position_element;
                const float *q1 = v[(i + 1) % 3] + 4 * position_element;
                const float *q2 = v[(i + 2) % 3] + 4 * position_element;
                const float dot = std::fabs((q1[0] - p[0]) * (q2[0] - p[0]) + (q1[1] - p[1]) * (q2[1] - p[1]));

                if (dot < best)
                  best = dot, corner = i;
              }
            }

            const uint32_t a = (corner + 1) % 3, b = (corner + 2) % 3;
            float *quad = dst + size_t(4 * r) * floats;
            std::memcpy(quad, v[0], 3 * size_t(floats) * 4);
            float *v3 = quad + 3 * size_t(floats);

            for (size_t e = 0; e < host_elements.size(); ++e)
            {
              for (uint32_t j = 0; j < 4; ++j)
              {
                const size_t k = 4 * e + j;
                v3[k] = host_elements[e].fetch.out == FetchElement::Out::kUint ? v[corner][k] : v[a][k] + v[b][k] - v[corner][k];
              }
            }

            rect_indices.insert(rect_indices.end(), {4 * r, 4 * r + 1, 4 * r + 2, 4 * r + a, 4 * r + b, 4 * r + 3});
          }

          ++stats_.rects_converted;
          vb_address = ex.gpu;
          vb_size = rects * 4 * host_stride;
        }

        list_->SetVertexBuffer(0, vb_address, vb_size, host_stride);

        if (ventry)
        {
          ventry->hash = vhash;
          GeoStore(*ventry, up, vertex_count * host_stride);
        }
      }
    }

    list_->SetVertexBuffer(kZeroInputSlot, zero_buffer_->gpu, 256, 0);
    return true;
  }

  // The index buffer: RECTLIST / TRIFAN / QUADLIST converted to triangle lists, cached, or swapped and uploaded
  bool Renderer::UploadIndices(DrawContext &d)
  {
    const DrawCall &call = d.call;
    const std::vector<uint32_t> &rect_indices = d.rect_indices;
    const std::vector<uint32_t> &indices = d.indices;
    const bool rect_list = d.rect_list, convert = d.convert, index_hit = d.index_hit, index32 = d.index32;
    const uint32_t vmin = d.vmin, vmax = d.vmax;
    GeoEntry *ientry = d.ientry;
    const uint64_t ihash = d.ihash;
    uint32_t &draw_count = d.draw_count;
    bool &draw_indexed = d.draw_indexed;
    std::vector<uint32_t> converted; // TRIFAN / QUADLIST
    draw_count = call.count;
    draw_indexed = call.indexed;

    if (rect_list)
    {
      if (rect_indices.empty() || call.indexed)
      {
        ++stats_.skip_prim;
        return false;
      }

      const Upload up = Allocate(rect_indices.size() * 4, 16);

      if (!up.cpu)
        return false;

      std::memcpy(up.cpu, rect_indices.data(), rect_indices.size() * 4);
      list_->SetIndexBuffer(up.gpu, uint32_t(rect_indices.size() * 4), DXGI_FORMAT_R32_UINT);
      draw_count = uint32_t(rect_indices.size());
      draw_indexed = true;
    }
    else if (convert)
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
        return false;

      const Upload up = Allocate(converted.size() * 4, 16);

      if (!up.cpu)
        return false;

      std::memcpy(up.cpu, converted.data(), converted.size() * 4);
      list_->SetIndexBuffer(up.gpu, uint32_t(converted.size() * 4), DXGI_FORMAT_R32_UINT);
      draw_count = uint32_t(converted.size());
      draw_indexed = true;
    }
    else if (call.indexed && index_hit)
    {
      ++stats_.geo_index_hits;
      list_->SetIndexBuffer(GeoAddress(*ientry), uint32_t(size_t(call.count) * (index32 ? 4 : 2)), index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT);
    }
    else if (call.indexed)
    {
      const size_t bytes = size_t(call.count) * (index32 ? 4 : 2);
      const Upload up = Allocate(bytes, 16);

      if (!up.cpu)
        return false;

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

      list_->SetIndexBuffer(up.gpu, uint32_t(bytes), index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT);

      if (ientry)
      {
        ientry->hash = ihash;
        ientry->vmin = vmin;
        ientry->vmax = vmax;
        GeoStore(*ientry, up, uint32_t(bytes));
      }
    }

    return true;
  }
}