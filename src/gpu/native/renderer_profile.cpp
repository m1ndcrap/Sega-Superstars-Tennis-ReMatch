// Native renderer: GPU timing and the perf log lines

#include "gpu/native/renderer_internal.h"

namespace sst::native
{
  using namespace renderer_detail;

  void Renderer::ReadGpuTime(uint32_t slot)
  {
    if (!timestamps_ || !timestamp_pending_[slot])
      return;

    // Per pass: the time from each mark to the next goes to the mark's target
    if (pass_ts_ && pass_marks_[slot].size() >= 2)
    {
      const std::vector<PassMark> &marks = pass_marks_[slot];
      const size_t begin = 8 * size_t(slot) * kPassMarks, end = 8 * (size_t(slot) * kPassMarks + marks.size());

      if (const uint8_t *pm = pass_readback_->Map(begin, end))
      {
        const uint64_t *t = reinterpret_cast<const uint64_t *>(pm + begin);

        for (size_t i = 0; i + 1 < marks.size(); ++i)
        {
          if (t[i + 1] < t[i] || !timestamp_frequency_)
            continue;

          const PassMark &m = marks[i];
          const uint64_t key = (uint64_t(m.rt) << 32) | (uint64_t(m.w) << 18) | (uint64_t(m.h) << 4) | m.kind;
          PassCost &c = pass_costs_[key];
          c.ms += double(t[i + 1] - t[i]) * 1000.0 / double(timestamp_frequency_);
          ++c.passes;
        }

        ++pass_frames_;
        pass_readback_->Unmap();
      }
    }

    pass_marks_[slot].clear();

    timestamp_pending_[slot] = false;
    const uint8_t *mapped = timestamp_readback_->Map(16 * size_t(slot), 16 * size_t(slot) + 16);

    if (!mapped)
      return;

    uint64_t t[2];
    std::memcpy(t, mapped + 16 * slot, 16);
    timestamp_readback_->Unmap();

    if (t[1] <= t[0] || !timestamp_frequency_)
      return;

    const double ms = double(t[1] - t[0]) * 1000.0 / double(timestamp_frequency_);
    prof_.gpu_ms_sum += ms;
    prof_.gpu_ms_max = std::max(prof_.gpu_ms_max, ms);
    ++prof_.gpu_frames;
  }

  void Renderer::CreateTimestampQueries()
  {
    timestamp_frequency_ = gfx_->TimestampFrequency();

    if (!timestamp_frequency_)
      return;

    timestamps_ = gfx_->CreateTimestampPool(2 * kSlots);

    if (!timestamps_)
      return;

    timestamp_readback_ = gfx_->CreateBuffer(16 * kSlots, gfx::HeapType::kReadback, D3D12_RESOURCE_STATE_COPY_DEST);

    if (!timestamp_readback_)
      timestamps_.reset();

    // Per-pass timestamps
    pass_ts_ = gfx_->CreateTimestampPool(kPassMarks * kSlots);
    pass_readback_ = pass_ts_ ? gfx_->CreateBuffer(8 * uint64_t(kPassMarks) * kSlots, gfx::HeapType::kReadback, D3D12_RESOURCE_STATE_COPY_DEST) : nullptr;

    if (!pass_ts_ || !pass_readback_)
    {
      pass_ts_.reset();
      pass_readback_.reset();
    }
  }

  void Renderer::MarkPass(uint32_t rt, uint32_t w, uint32_t h, uint8_t kind)
  {
    std::vector<PassMark> &marks = pass_marks_[frame_res_];

    if (!pass_ts_ || marks.size() >= kPassMarks - 1)
      return;

    list_->WriteTimestamp(*pass_ts_, frame_res_ * kPassMarks + uint32_t(marks.size()));
    marks.push_back({rt, uint16_t(std::min(w, 65535u)), uint16_t(std::min(h, 65535u)), kind});
  }

  Renderer::FrameSnapshot Renderer::TakeFrameSnapshot() const
  {
    FrameSnapshot f;
    std::copy(std::begin(prof_.ticks), std::end(prof_.ticks), f.ticks);
    f.draws = stats_.draws;
    f.memexport_run = stats_.memexport_run;
    f.memexport_waits = stats_.memexport_waits;
    f.memexport_wait_ms = stats_.memexport_wait_ms;
    f.pipelines_created = stats_.pipelines_created;
    f.pipeline_ms = stats_.pipeline_ms;

    if (textures_)
    {
      const TextureCache::Counters t = textures_->TotalCounters();
      f.tex_created = t.created;
      f.tex_uploads = t.uploads;
      f.tex_upload_bytes = t.upload_bytes;
      f.tex_hashed_bytes = t.hashed_bytes;
      f.tex_upload_ms = t.upload_ms;
      f.tex_hash_ms = t.hash_ms;
    }

    return f;
  }

  // At every frame end, a frame whose renderer work took longer than --sst_native_slow_frame_ms is logged with its own breakdown (the 10 s profile averages hide one-off frames like a match's first). Running totals can be reset by the periodic log; a total smaller than its snapshot counts from 0.
  void Renderer::CheckSlowFrame(uint64_t frame)
  {
    const FrameSnapshot now = TakeFrameSnapshot();
    const FrameSnapshot &was = frame_prev_;
    const int32_t threshold = REXCVAR_GET(sst_native_slow_frame_ms);

    auto d = [](auto a, auto b)
    { return a >= b ? a - b : a; };

    if (threshold > 0 && ms_per_tick_ > 0)
    {
      double phase[kPhCount], renderer = 0;

      for (uint32_t i = 0; i < kPhCount; ++i)
      {
        phase[i] = double(d(now.ticks[i], was.ticks[i])) * ms_per_tick_;

        if (i != kPhFenceWait)
          renderer += phase[i];
      }

      if (renderer >= threshold && (slow_frames_logged_ < 60 || (slow_frames_logged_ % 20) == 0))
      {
        static constexpr const char *kNames[kPhCount] = {"other", "memexport", "shaders", "inputs", "indices", "pipelines", "vertices", "constants", "textures", "ui fit", "recording", "clears", "submit", "GPU wait"};
        uint32_t order[kPhCount];

        for (uint32_t i = 0; i < kPhCount; ++i)
          order[i] = i;

        std::sort(std::begin(order), std::end(order), [&](uint32_t a, uint32_t b)
                  { return phase[a] > phase[b]; });

        std::string top;

        for (uint32_t i = 0; i < 5 && phase[order[i]] >= 0.5; ++i)
          top += fmt::format("{}{} {:.1f}", i ? ", " : "", kNames[order[i]], phase[order[i]]);

        REXLOG_INFO("[sst-native] slow render frame {}: {:.1f} ms in the renderer ({}); {} draws; textures: {} created, {} uploads ({:.1f} MB, {:.1f} ms), {:.1f} MB hashed ({:.1f} ms); memexport: {} run, waited {} times ({:.1f} ms); pipelines: {} ready ({:.1f} ms)", frame, renderer, top,
                    d(now.draws, was.draws), d(now.tex_created, was.tex_created), d(now.tex_uploads, was.tex_uploads), double(d(now.tex_upload_bytes, was.tex_upload_bytes)) / (1 << 20), d(now.tex_upload_ms, was.tex_upload_ms), double(d(now.tex_hashed_bytes, was.tex_hashed_bytes)) / (1 << 20),
                    d(now.tex_hash_ms, was.tex_hash_ms), d(now.memexport_run, was.memexport_run), d(now.memexport_waits, was.memexport_waits), d(now.memexport_wait_ms, was.memexport_wait_ms), d(now.pipelines_created, was.pipelines_created), d(now.pipeline_ms, was.pipeline_ms));
      }

      if (renderer >= threshold)
        ++slow_frames_logged_;
    }

    frame_prev_ = now;
  }

  // Every sst_native_log_every frames: where the frame time went.
  void Renderer::LogProfile()
  {
    Profile &p = prof_;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t tsc = Tick();

    if (p.period_start.time_since_epoch().count() && p.frames)
    {
      const double wall_ms = std::chrono::duration<double, std::milli>(now - p.period_start).count();
      const double ms_per_tick = wall_ms / double(std::max<uint64_t>(1, tsc - p.period_tsc));
      ms_per_tick_ = ms_per_tick;
      const double frames = double(p.frames);

      auto ms = [&](Phase ph)
      {
        return double(p.ticks[ph]) * ms_per_tick / frames;
      };

      double renderer = 0;

      for (uint32_t i = 0; i < kPhCount; ++i)
      {
        if (i != kPhFenceWait)
          renderer += ms(Phase(i));
      }

      const double frame = p.frame_ms_sum / frames;
      REXLOG_INFO("[sst-native] perf: {:.1f} fps, frame {:.2f} ms avg / {:.1f} max ({:.0f}% over 1/60 s, {:.0f}% over 1/30 s); GPU {:.2f} ms avg / {:.1f} max", 1000.0 / frame, frame, p.frame_ms_max, 100.0 * p.over_60 / frames, 100.0 * p.over_30 / frames, p.gpu_frames ? p.gpu_ms_sum / p.gpu_frames : 0.0, p.gpu_ms_max);

      if (pass_frames_ && !pass_costs_.empty())
      {
        std::vector<std::pair<uint64_t, PassCost>> v(pass_costs_.begin(), pass_costs_.end());

        std::sort(v.begin(), v.end(), [](const auto &x, const auto &y)
                  { return x.second.ms > y.second.ms; });

        std::string text;
        static constexpr const char *kKinds[5] = {"", " 3D scene", " shadow map", " frame start", " present / UI blits"};

        for (size_t i = 0; i < v.size() && i < 10; ++i)
        {
          const uint64_t k = v[i].first;
          const uint32_t kind = uint32_t(k & 15), w = uint32_t((k >> 18) & 0x3FFF), h = uint32_t((k >> 4) & 0x3FFF);
          text += fmt::format(" {}{:.2f} ms ({:.1f}/frame);", kind >= 3 ? std::string(kKinds[kind] + 1) + " " : fmt::format("{:08X} {}x{}{} ", uint32_t(k >> 32), w, h, kKinds[kind]), v[i].second.ms / double(pass_frames_), double(v[i].second.passes) / double(pass_frames_));
        }

        REXLOG_INFO("[sst-native] perf: GPU by pass (per frame):{}", text);
        pass_costs_.clear();
        pass_frames_ = 0;
      }

      REXLOG_INFO("[sst-native] perf: {} per frame: {:.2f} ms in the renderer, {:.2f} ms {}; renderer: memexport {:.2f}, shaders {:.2f}, inputs {:.2f}, indices {:.2f}, pipelines {:.2f}, vertices {:.2f}, constants {:.2f}, textures {:.2f}, ui fit {:.2f}, recording {:.2f}, other {:.2f}, clears {:.2f}, "
                  "submit {:.2f}; waits: GPU {:.2f}, morphs {:.2f}",
                  threaded_ ? "render thread" : "game thread", renderer, std::max(0.0, frame - renderer), threaded_ ? "idle (waiting for the game's next frame)" : "in the game (and its own D3D)", ms(kPhMemexport), ms(kPhShaders), ms(kPhInputs), ms(kPhIndices), ms(kPhPipeline), ms(kPhVertices), ms(kPhConstants), ms(kPhTextures), ms(kPhUiFit), ms(kPhRecord), ms(kPhOther), ms(kPhClear), ms(kPhSubmit), ms(kPhFenceWait), stats_.memexport_wait_ms / frames);

      uint64_t acquires, repeats, skips;
      {
        std::lock_guard lock(slot_mutex_);
        acquires = acquires_;
        repeats = acquire_repeats_;
        skips = acquire_skips_;
        acquires_ = acquire_repeats_ = acquire_skips_ = 0;
      }

      REXLOG_INFO("[sst-native] perf: frames handed to the display: {} ({} shown twice, {} never shown); frame intervals over 20 ms: {}", acquires, repeats, skips, p.over_20);
    }

    const Profile fresh;
    const auto last = p.last_frame;
    p = fresh;
    p.last_frame = last;
    p.period_start = now;
    p.period_tsc = tsc;
  }
}