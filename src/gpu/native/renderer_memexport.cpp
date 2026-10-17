// Native renderer: memory-export (morph) shaders, run on the CPU

#include "gpu/native/renderer_internal.h"

namespace sst::native
{
  using namespace renderer_detail;

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

  // On the console the GPU runs these shaders and writes their eM# exports to memory; the visible mesh draw then fetches those vertices. Here the shader is interpreted on the CPU (memexport.h) with the device's current state, writing guest memory directly before the dependent draw is recorded
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

    // Vertex indices (r0.x of each invocation)
    memexport_indices_.resize(call.count);

    if (call.indexed)
    {
      const uint32_t ib = be32(base, dev + kDevIndices);

      if (!ib || !GuestReadable(ib, 0x1C))
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

      if (!GuestReadable(ib_addr + aligned_begin, aligned_end - aligned_begin))
        return fail("index buffer memory gone");

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

    // Earlier background jobs this one depends on: its output, or inputs that are another morph's output
    if (!pending_memexports_.empty())
    {
      for (uint32_t i = 0; i < stream_count; ++i)
        WaitMemexports(streams[i].begin, streams[i].begin + streams[i].size);

      uint32_t slots[16];
      const uint32_t nslots = VertexFetchSlots(memexport_ucode_.data(), dwords, slots, 16);

      for (uint32_t i = 0; i < nslots; ++i)
      {
        const uint32_t phys = fetch[2 * slots[i]] & 0x1FFFFFFCu;
        const uint32_t size = fetch[2 * slots[i] + 1] & 0x03FFFFFCu;
        WaitMemexports(phys, phys + size);
      }
    }

    // Cache (checked by the job, off the render thread): identical shader, constants, indices, input vertex data and an untouched previous output -> nothing to do
    const bool use_cache = REXCVAR_GET(sst_native_memexport_cache) && stream_count == 1 && streams[0].size && uint64_t(streams[0].begin) + streams[0].size <= 0x20000000u;
    const uint64_t key = use_cache ? vs_entry->hash ^ (uint64_t(streams[0].begin) << 20) : 0;
    const uint64_t input_hash = 0;

    auto job = std::make_shared<MemexportJob>();
    job->ucode = memexport_ucode_;
    job->constants = memexport_constants_;
    std::memcpy(job->bools, bools, sizeof(bools));
    std::memcpy(job->loops, loops, sizeof(loops));
    std::memcpy(job->fetch, fetch, sizeof(fetch));
    job->indices.assign(memexport_indices_.begin(), memexport_indices_.begin() + call.count);

    if (key)
    {
      job->use_cache = true;
      job->hash_begin = streams[0].begin;
      job->hash_size = streams[0].size;

      if (auto it = memexport_cache_.find(key); it != memexport_cache_.end() && it->second.output_begin == streams[0].begin && it->second.output_size == streams[0].size)
      {
        job->has_previous = true;
        job->previous_input_hash = it->second.input_hash;
        job->previous_output_hash = it->second.output_hash;
      }
    }

    PendingMemexport pending;
    pending.job = job;
    pending.cache_key = key;
    pending.input_hash = input_hash;
    pending.vs_hash = vs_entry->hash;
    pending.count = call.count;
    pending.indexed = call.indexed;
    pending.literals = literals;
    pending.stream_count = stream_count;

    for (uint32_t i = 0; i < stream_count; ++i)
    {
      pending.begin[i] = streams[i].begin;
      pending.size[i] = streams[i].size;
      pending.format[i] = streams[i].format;

      // Consumers this frame read these ranges (checks in Draw)
      memexport_written_.push_back({streams[i].begin, streams[i].begin + streams[i].size});
    }

    // --sst_native_memexport_dump: the first 3 runs of each shader, run here (synchronously) so the output can be saved with the inputs
    if (const std::string dir = REXCVAR_GET(sst_native_memexport_dump); !dir.empty() && memexport_dumped_[vs_entry->hash] < 3)
    {
      const uint32_t n = memexport_dumped_[vs_entry->hash]++;
      std::vector<uint8_t> file;

      auto put = [&](const void *data, size_t size)
      {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        file.insert(file.end(), p, p + size);
      };

      auto put32 = [&](uint32_t v)
      { put(&v, 4); };

      // Format (little-endian): "SSTMX1\0\0", vs hash, dwords, ucode, 1024 floats, bools[8], loops[32], fetch[192], index count, indices, input range count, {phys, size, bytes}..., output range count, {phys, size, bytes after the run}...
      put("SSTMX1\0\0", 8);
      put(&vs_entry->hash, 8);
      put32(uint32_t(job->ucode.size()));
      put(job->ucode.data(), job->ucode.size() * 4);
      put(job->constants.data(), 1024 * 4);
      put(job->bools, sizeof(job->bools));
      put(job->loops, sizeof(job->loops));
      put(job->fetch, sizeof(job->fetch));
      put32(uint32_t(job->indices.size()));
      put(job->indices.data(), job->indices.size() * 4);
      uint32_t slots[16];
      const uint32_t nslots = VertexFetchSlots(job->ucode.data(), uint32_t(job->ucode.size()), slots, 16);
      std::vector<std::pair<uint32_t, uint32_t>> inputs;

      for (uint32_t i = 0; i < nslots; ++i)
      {
        const uint32_t phys = job->fetch[2 * slots[i]] & 0x1FFFFFFCu, size = job->fetch[2 * slots[i] + 1] & 0x03FFFFFCu;

        if (size && uint64_t(phys) + size <= 0x20000000u && std::find(inputs.begin(), inputs.end(), std::make_pair(phys, size)) == inputs.end())
          inputs.push_back({phys, size});
      }

      put32(uint32_t(inputs.size()));

      for (const auto &[phys, size] : inputs)
      {
        put32(phys);
        put32(size);
        put(physical_membase_ + phys, size);
      }

      job->use_cache = false; // always run
      memexport_->Execute(*job);
      put32(stream_count);

      for (uint32_t i = 0; i < stream_count; ++i)
      {
        const uint32_t size = uint64_t(streams[i].begin) + streams[i].size <= 0x20000000u ? streams[i].size : 0;
        put32(streams[i].begin);
        put32(size);
        put(physical_membase_ + streams[i].begin, size);
      }

      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      std::ofstream(std::filesystem::path(dir) / fmt::format("{:016X}_{}.mx", vs_entry->hash, n), std::ios::binary).write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
      REXLOG_INFO("[sst-native] memexport dump: VS {:016X} run {} ({} vertices, {} KB) written to {}", vs_entry->hash, n, job->indices.size(), file.size() >> 10, dir);
      FinishMemexport(pending);
      return;
    }

    if (REXCVAR_GET(sst_native_memexport_async))
    {
      // Runs on the background thread; draws reading the output wait for it
      pending.ticket = memexport_->Submit(job);
      pending_memexports_.push_back(std::move(pending));
      ++stats_.memexport_async;
      return;
    }

    memexport_->Execute(*job);
    FinishMemexport(pending);
  }

  // Waits for background memexport jobs whose output overlaps [begin, end)
  void Renderer::WaitMemexports(uint32_t begin, uint32_t end)
  {
    if (pending_memexports_.empty())
      return;

    uint64_t need = 0;

    for (const PendingMemexport &p : pending_memexports_)
    {
      for (uint32_t i = 0; i < p.stream_count; ++i)
      {
        if (begin < p.begin[i] + p.size[i] && end > p.begin[i])
          need = std::max(need, p.ticket);
      }
    }

    if (need)
    {
      if (memexport_->finished() < need)
      {
        const auto t0 = std::chrono::steady_clock::now();
        memexport_->Wait(need);
        ++stats_.memexport_waits;
        stats_.memexport_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      }
    }

    CollectMemexports(false);
  }

  // Finishes the jobs that are done (all of them, waiting, with `all`)
  void Renderer::CollectMemexports(bool all)
  {
    if (pending_memexports_.empty())
      return;

    if (all)
      memexport_->Wait(pending_memexports_.back().ticket);

    const uint64_t done = memexport_->finished();
    size_t n = 0;

    while (n < pending_memexports_.size() && pending_memexports_[n].ticket <= done)
      FinishMemexport(pending_memexports_[n++]);

    pending_memexports_.erase(pending_memexports_.begin(), pending_memexports_.begin() + n);
  }

  void Renderer::FinishMemexport(const PendingMemexport &p)
  {
    const MemexportJob &job = *p.job;
    MemexportStats &m = memexport_stats_;
    m.vertices += job.stats.vertices;
    m.elements += job.stats.elements;
    m.bad_address += job.stats.bad_address;
    m.bad_format += job.stats.bad_format;
    m.out_of_range += job.stats.out_of_range;
    m.fetch_oob += job.stats.fetch_oob;
    m.malformed += job.stats.malformed;
    m.per_vertex_draws += job.stats.per_vertex_draws;

    if (job.stats.elements)
    {
      m.last_format = job.stats.last_format;
      m.last_base = job.stats.last_base;
      m.last_count = job.stats.last_count;
    }

    stats_.memexport_ms += job.ms;

    {
      MemexportShaderCost &c = memexport_costs_[p.vs_hash];
      c.ms += job.ms;
      c.vertices += job.skipped ? 0 : job.indices.size();
      ++(job.skipped ? c.cached : c.runs);
    }

    if (job.skipped)
    {
      ++stats_.memexport_cached;
      return;
    }

    ++stats_.memexport_run;
    const uint64_t written = job.stats.elements;

    if (!job.ok || !written)
    {
      ++stats_.memexport_failed;

      if (LogOnce(0xC400000000000000ull ^ p.vs_hash))
        REXLOG_WARN("[sst-native] memexport VS {:016X}: {}", p.vs_hash, !job.ok ? "interpreter refused the shader" : "shader wrote nothing");

      return;
    }

    if (REXCVAR_GET(sst_native_diagnostics))
    {
      for (uint32_t i = 0; i < p.stream_count; ++i)
      {
        const bool hashable = uint64_t(p.begin[i]) + p.size[i] <= 0x20000000u;
        morph_writes_[p.begin[i]] = {submitted_, draw_seq_, false, p.size[i], hashable ? XXH3_64bits(physical_membase_ + p.begin[i], p.size[i]) : 0};
      }

      if (p.stream_count == 1 && p.format[0] >= 36 && p.format[0] <= 38 && uint64_t(p.begin[0]) + p.size[0] <= 0x20000000u)
      {
        // (Values read as 8in32-swapped floats, the endian this game uses.)
        const uint8_t *out = physical_membase_ + p.begin[0];
        uint32_t bad = 0, first_bad = UINT32_MAX;
        float first_value = 0;

        for (uint32_t i = 0; i < p.size[0] / 4; ++i)
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
              weights += fmt::format(" ({:.3f} {:.3f} {:.3f} {:.3f})", job.constants[4 * c], job.constants[4 * c + 1], job.constants[4 * c + 2], job.constants[4 * c + 3]);

            REXLOG_WARN("[sst-native] memexport VS {:016X} at {:08X}: {} implausible values of {} (first: float {} = {}); vertices {}; c1-c4{}", p.vs_hash, p.begin[0], bad, p.size[0] / 4, first_bad, first_value, p.count, weights);
          }
        }
      }
    }

    if (p.cache_key && job.input_hash)
    {
      MemexportCacheEntry &e = memexport_cache_[p.cache_key];
      e.input_hash = job.input_hash;
      e.output_begin = p.begin[0];
      e.output_size = p.size[0];
      e.output_hash = job.output_hash;
    }

    if (LogOnce(0xC500000000000000ull ^ p.vs_hash) || LogOnce(0xC600000000000000ull ^ (uint64_t(p.begin[0]) << 8)))
    {
      REXLOG_INFO("[sst-native] memexport VS {:016X}: {} vertices ({}) -> {} elements, format {} at {:08X} ({} streams, {} literal constants, c255 {} {} {} {}); {:.3f} ms;{}",
                  p.vs_hash, p.count, p.indexed ? "indexed" : "auto", written, job.stats.last_format, job.stats.last_base, p.stream_count, p.literals, job.constants[1020], job.constants[1021], job.constants[1022], job.constants[1023], job.ms, DescribeMicrocode(job.ucode.data(), uint32_t(job.ucode.size())));
    }
  }
}