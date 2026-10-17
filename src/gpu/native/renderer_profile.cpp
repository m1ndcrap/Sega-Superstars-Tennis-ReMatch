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
      const D3D12_RANGE prange = {8 * size_t(slot) * kPassMarks, 8 * (size_t(slot) * kPassMarks + marks.size())};
      void *pm = nullptr;

      if (SUCCEEDED(pass_readback_->Map(0, &prange, &pm)) && pm)
      {
        const uint64_t *t = reinterpret_cast<const uint64_t *>(static_cast<const uint8_t *>(pm) + prange.Begin);

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
        const D3D12_RANGE none = {0, 0};
        pass_readback_->Unmap(0, &none);
      }
    }

    pass_marks_[slot].clear();

    timestamp_pending_[slot] = false;
    const D3D12_RANGE range = {16 * size_t(slot), 16 * size_t(slot) + 16};
    void *mapped = nullptr;

    if (FAILED(timestamp_readback_->Map(0, &range, &mapped)) || !mapped)
      return;

    uint64_t t[2];
    std::memcpy(t, static_cast<const uint8_t *>(mapped) + 16 * slot, 16);
    const D3D12_RANGE none = {0, 0};
    timestamp_readback_->Unmap(0, &none);

    if (t[1] <= t[0] || !timestamp_frequency_)
      return;

    const double ms = double(t[1] - t[0]) * 1000.0 / double(timestamp_frequency_);
    prof_.gpu_ms_sum += ms;
    prof_.gpu_ms_max = std::max(prof_.gpu_ms_max, ms);
    ++prof_.gpu_frames;
  }

  void Renderer::CreateTimestampQueries()
  {
    if (FAILED(provider_->GetDirectQueue()->GetTimestampFrequency(&timestamp_frequency_)))
      return;

    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2 * kSlots;

    if (FAILED(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&timestamps_))))
      return;

    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 16 * kSlots;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timestamp_readback_))))
      timestamps_.Reset();

    // Per-pass timestamps
    qd.Count = kPassMarks * kSlots;
    bd.Width = 8 * uint64_t(kPassMarks) * kSlots;

    if (FAILED(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&pass_ts_))) || FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pass_readback_))))
    {
      pass_ts_.Reset();
      pass_readback_.Reset();
    }
  }

  void Renderer::MarkPass(uint32_t rt, uint32_t w, uint32_t h, uint8_t kind)
  {
    std::vector<PassMark> &marks = pass_marks_[frame_res_];

    if (!pass_ts_ || marks.size() >= kPassMarks - 1)
      return;

    list_->EndQuery(pass_ts_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame_res_ * kPassMarks + uint32_t(marks.size()));
    marks.push_back({rt, uint16_t(std::min(w, 65535u)), uint16_t(std::min(h, 65535u)), kind});
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