// The game thread's frame pacing and capture cost, logged every 'sst_native_log_every' frames ("[sst-native] perf:" lines). Read-only; nothing here changes what is drawn.

#include "gpu/native/capture_state.h"
#include "gpu/native/write_watch.h"

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <string>

REXCVAR_DECLARE(int32_t, sst_native_log_every);

namespace sst::native::capture
{
  namespace
  {
    using Clock = std::chrono::steady_clock;

    constexpr double kMB = double(1 << 20);

    // The (key, stat) pairs of a per-buffer / per-texture table, biggest byte count first
    template <typename Map>

    auto ByBytes(const Map &map)
    {
      std::vector<std::pair<typename Map::key_type, typename Map::mapped_type>> v(map.begin(), map.end());
      std::sort(v.begin(), v.end(), [](const auto &a, const auto &b)
                { return a.second.bytes > b.second.bytes; });
      return v;
    }

    void LogTotals(State &s, double frames, double tick_ms)
    {
      const CapturePerf &p = s.perf;
      const DeviceCapture &d = s.device;
      const double capture_ms = double(p.capture_ticks) * tick_ms;

      auto percent = [&](uint64_t n)
      { return d.chunks_total ? 100.0 * double(n) / double(d.chunks_total) : 0.0; };

      REXLOG_INFO("[sst-native] perf: game thread per frame: {:.2f} ms capturing {} calls ({:.1f} MB of snapshots), {:.2f} ms waiting for the render thread; game frames over 20 ms: {} (longest {:.1f} ms); longest capture in one frame {:.2f} ms; device block chunks "
                  "copied {:.1f}% (not compared {:.1f}%); textures copied {:.2f} MB ({} known); vertex data copied {:.2f} MB ({} buffers watched){}",
                  capture_ms / frames, uint64_t(double(p.calls) / frames + 0.5), double(p.snapshot_bytes) / frames / kMB, p.wait_ms / frames, p.swaps_over_20, p.swap_max_ms, double(p.capture_max_ticks) * tick_ms, percent(d.chunks_copied), percent(d.chunks_skipped), double(s.textures.bytes) / frames / kMB, s.watched.set.textures.size(), double(s.vertices.bytes) / frames / kMB, s.watched.set.vertices.size(), s.microcode.skipped ? fmt::format("; {} draws without a microcode snapshot", s.microcode.skipped) : std::string());

      double part_ms[CapturePerf::kParts];
      double other_ms = capture_ms;

      for (int i = 0; i < CapturePerf::kParts; ++i)
      {
        part_ms[i] = double(p.part_ticks[i]) * tick_ms;
        other_ms -= part_ms[i];
      }

      REXLOG_INFO("[sst-native] perf: capture split per frame: device block {:.2f} ms, VS microcode {:.2f} ms, index scan {:.2f} ms, texture copies {:.2f} ms, vertex copies {:.2f} ms, other {:.2f} ms",
                  part_ms[CapturePerf::kDevice] / frames, part_ms[CapturePerf::kMicrocode] / frames, part_ms[CapturePerf::kIndexScan] / frames, part_ms[CapturePerf::kTextures] / frames, part_ms[CapturePerf::kVertices] / frames, other_ms / frames);
    }

    void LogVertexCopies(const VertexCapture &v, double frames)
    {
      if (v.stats.empty())
        return;

      const auto by_bytes = ByBytes(v.stats);
      std::string text;

      for (size_t i = 0; i < by_bytes.size() && i < 8; ++i)
      {
        const auto &[buffer, st] = by_bytes[i];
        text += fmt::format(" {:08X}: {:.2f} MB/frame, {:.1f} draws/frame ({:.1f} UI), largest {} KB, watched {} KB;", buffer, double(st.bytes) / frames / kMB, double(st.draws) / frames, double(st.ui_draws) / frames, st.largest >> 10, st.watched >> 10);
      }

      REXLOG_INFO("[sst-native] perf: vertex copies by buffer:{} ({} buffers; {:.1f} copies a frame reused unchanged, {:.1f} of them without comparing (write watch); watch: {:.1f} arms/frame, {:.1f} write faults/frame, {:.1f} false alarms/frame, {} noisy buffers)",
                  text, by_bytes.size(), double(v.reused) / frames, double(v.reused_by_watch) / frames, double(v.watch_arms) / frames, double(write_watch::TakeFaults()) / frames, double(v.watch_false) / frames, v.noisy);
    }

    void LogTextureCopies(const TextureCapture &t, double frames)
    {
      if (!t.stats.empty())
      {
        const auto by_bytes = ByBytes(t.stats);
        std::string text;
        uint64_t changing_bytes = 0;

        for (const auto &[page, st] : by_bytes)
          changing_bytes += st.changing ? st.bytes : 0;

        for (size_t i = 0; i < by_bytes.size() && i < 8; ++i)
        {
          const auto &[page, st] = by_bytes[i];
          text += fmt::format(" {:08X}: {:.2f} MB/frame ({:.1f} copies/frame{});", page << 12, double(st.bytes) / frames / kMB, double(st.copies) / frames, st.changing ? ", seen changing" : "");
        }

        REXLOG_INFO("[sst-native] perf: texture copies by texture:{} ({} textures; seen changing {:.2f} MB/frame)", text, by_bytes.size(), double(changing_bytes) / frames / kMB);
      }

      if (t.checks_hashed)
        REXLOG_INFO("[sst-native] perf: UI textures checked by hash: {:.1f}/frame, {:.1f}/frame unchanged (not copied)", double(t.checks_hashed) / frames, double(t.checks_unchanged) / frames);
    }

    // Starts the next logging period (session-long values - noisy buffers, hash checks' known contents - are kept)
    void ResetPeriod(State &s)
    {
      CapturePerf &p = s.perf;
      std::fill(std::begin(p.part_ticks), std::end(p.part_ticks), uint64_t(0));
      p.capture_ticks = p.capture_max_ticks = 0;
      p.calls = p.snapshot_bytes = p.frames = 0;
      p.wait_ms = p.swap_max_ms = 0;
      p.swaps_over_20 = 0;

      DeviceCapture &d = s.device;
      d.chunks_copied = d.chunks_total = d.chunks_skipped = 0;
      s.microcode.reused_by_watch = s.microcode.skipped = 0;

      TextureCapture &t = s.textures;
      t.stats.clear();
      t.bytes = t.checks_hashed = t.checks_unchanged = 0;

      VertexCapture &v = s.vertices;
      v.stats.clear();
      v.bytes = v.reused = v.reused_by_watch = v.watch_arms = v.watch_false = 0;
    }
  }

  void NoteSwap(State &s)
  {
    CapturePerf &p = s.perf;
    const auto now = Clock::now();

    if (p.last_swap.time_since_epoch().count())
    {
      const double ms = std::chrono::duration<double, std::milli>(now - p.last_swap).count();
      p.swap_max_ms = std::max(p.swap_max_ms, ms);
      p.swaps_over_20 += ms > 20.0;
    }

    p.last_swap = now;
  }

  void LogCapturePerf(State &s)
  {
    CapturePerf &p = s.perf;
    const int every = REXCVAR_GET(sst_native_log_every);

    if (!p.period_ticks0)
    {
      p.period_ticks0 = Ticks();
      p.period_t0 = Clock::now();
    }

    if (every <= 0 || p.frames < uint64_t(every))
      return;

    // Ticks -> ms, from this period's length on both clocks
    const uint64_t now_ticks = Ticks();
    const auto now = Clock::now();
    const double period_ms = std::chrono::duration<double, std::milli>(now - p.period_t0).count();
    const double tick_ms = now_ticks > p.period_ticks0 ? period_ms / double(now_ticks - p.period_ticks0) : 0.0;
    p.period_ticks0 = now_ticks;
    p.period_t0 = now;
    const double frames = double(p.frames);

    LogTotals(s, frames, tick_ms);
    LogVertexCopies(s.vertices, frames);
    LogTextureCopies(s.textures, frames);

    if (s.microcode.reused_by_watch)
      REXLOG_INFO("[sst-native] perf: VS microcode reused without comparing (write watch): {:.1f}/frame", double(s.microcode.reused_by_watch) / frames);

    ResetPeriod(s);
  }
}