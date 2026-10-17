// Native renderer, exe side: the render thread. It runs the game thread's captured commands on the Renderer, a frame behind at most, and gives each frame's arena back once its commands have run.

#include "gpu/native/capture_state.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <bit>
#include <exception>

REXCVAR_DECLARE(int32_t, sst_native_log_every);

namespace sst::native::capture
{
  uint8_t *Arena::Alloc(size_t size)
  {
    size = (size + 63) & ~size_t(63);

    while (current_ < chunks_.size())
    {
      Chunk &c = chunks_[current_];

      if (used_ + size <= c.size)
      {
        uint8_t *p = c.data.get() + used_;
        used_ += size;
        bytes_ += size;
        return p;
      }

      ++current_;
      used_ = 0;
    }

    const size_t chunk = std::max(size, size_t(8) << 20);
    chunks_.push_back({std::make_unique<uint8_t[]>(chunk + 64), chunk});
    current_ = chunks_.size() - 1;
    used_ = size;
    bytes_ += size;
    return chunks_.back().data.get();
  }

  namespace
  {
    // Brings the render thread's copy of the device block up to this command (every command, run or not, so it stays in step with the game thread's)
    void ApplyDevice(State &s, const Command &c)
    {
      if (!c.dev_addr)
        return;

      s.rt_dev_addr = c.dev_addr;
      const uint8_t *src = c.dev_delta;

      for (uint64_t m = c.dev_mask; m; m &= m - 1)
      {
        const uint32_t i = uint32_t(std::countr_zero(m));
        std::memcpy(s.rt_dev.get() + i * kDevChunk, src, ChunkSize(i));
        src += ChunkSize(i);
      }
    }

    void Run(State &s, const Command &c)
    {
      // The guest memory this command reads as it was when the game issued it
      GuestOverlay overlays[9];
      uint32_t count = 0;

      for (uint32_t i = 0; i < c.overlay_count; ++i)
        overlays[count++] = c.overlays[i];

      if (c.dev_addr && c.dev_addr == s.rt_dev_addr)
        overlays[count++] = {c.dev_addr + kDevSnapBegin, c.dev_addr + kDevSnapEnd, s.rt_dev.get()};

      SetGuestOverlays(overlays, count);
      const uint8_t *base = s.base;

      switch (c.kind)
      {
      case Command::kDraw:
        for (uint32_t i = 0; i < c.tex_count; ++i)
        {
          if (c.tex[i].data)
            s.renderer.AddTextureSnapshot(c.tex[i].begin, c.tex[i].end, c.tex[i].data);
          else
            s.renderer.AddTextureUnchanged(c.tex[i].begin >> 12, c.tex[i].hash);
        }

        s.renderer.SetDrawVertexCopies(c.verts, c.vert_count);
        s.renderer.Draw(base, c.draw);
        s.renderer.SetDrawVertexCopies(nullptr, 0);
        break;

      case Command::kClear:
        s.renderer.Clear(base, c.device, c.flags, c.color, c.z, c.stencil);
        break;

      case Command::kResolve:
        s.renderer.Resolve(base, c.resolve);
        break;

      case Command::kEvent:
        s.renderer.TraceEvent(base, c.device, std::string(reinterpret_cast<const char *>(c.data), c.size));
        break;

      case Command::kShader:
        s.shaders.BindObject(c.object, s.shaders.Register(c.data, c.size));
        break;

      case Command::kSwap:
      {
        SetGuestOverlays(nullptr, 0);
        s.renderer.EndFrame();
        const uint64_t frame = s.renderer.frames();
        const int every = REXCVAR_GET(sst_native_log_every);

        if (frame == 1 || (every > 0 && frame % uint64_t(every) == 0))
        {
          s.renderer.LogStats(frame);
          s.shaders.LogStats("so far");
        }

        break;
      }
      }

      SetGuestOverlays(nullptr, 0);
    }

    void RenderThreadMain(State *s)
    {
      CommandQueue &q = s->queue;

      for (;;)
      {
        Batch batch;

        {
          std::unique_lock lock(q.mutex);

          q.work_cv.wait(lock, [&]
                         { return q.stop || !q.batches.empty(); });

          if (q.batches.empty())
            return; // stopping

          batch = std::move(q.batches.front());
          q.batches.pop_front();
        }

        {
          std::lock_guard lock(s->mutex);
          const bool active = g_active.load(std::memory_order_acquire);

          for (const Command &c : batch.commands)
          {
            ApplyDevice(*s, c);

            if (active)
              Execute(*s, c);
          }
        }

        if (batch.arena)
        {
          batch.arena->Reset();
          std::lock_guard lock(q.mutex);
          q.free_arenas.push_back(batch.arena);
          q.frames_done = batch.frame;
          q.done_cv.notify_all();
        }
      }
    }
  }

  // A C++ exception escaping a command (std::bad_alloc / length_error from a draw whose guest data was garbage, e.g. freed while queued) would end the process without a crash report: log it, skip the command and carry on
  void Execute(State &s, const Command &c)
  {
    try
    {
      Run(s, c);
    }
    catch (const std::exception &e)
    {
      SetGuestOverlays(nullptr, 0);
      s.renderer.SetDrawVertexCopies(nullptr, 0);
      static std::atomic<uint32_t> logged{0};

      if (logged.fetch_add(1) < 20)
        REXLOG_ERROR("[sst-native] command {} failed and was skipped: {} (draw: prim {} indexed {} start {} count {} base vertex {} UP {:08X})", int(c.kind), e.what(), c.draw.prim, c.draw.indexed, c.draw.start, c.draw.count, c.draw.base_vertex, c.draw.up_data);
    }
    catch (...)
    {
      SetGuestOverlays(nullptr, 0);
      s.renderer.SetDrawVertexCopies(nullptr, 0);
      REXLOG_ERROR("[sst-native] command {} failed with an unknown exception and was skipped", int(c.kind));
    }
  }

  void StartRenderThread(State &s)
  {
    s.threaded = true;
    s.device.snap_addr = s.rt_dev_addr = 0; // the first capture sends the whole device block
    s.renderer.SetThreaded(true);
    CommandQueue &q = s.queue;

    for (int i = 0; i < 3; ++i)
    {
      q.arenas.push_back(std::make_unique<Arena>());

      // A frame's copies take ~15-20 MB: allocate (and touch) them up front so the game thread doesn't page-fault fresh chunks mid-match
      q.arenas.back()->Alloc(size_t(32) << 20);
      q.arenas.back()->Reset();
      q.free_arenas.push_back(q.arenas.back().get());
    }

    s.arena = q.free_arenas.back();
    q.free_arenas.pop_back();
    s.batch.commands.reserve(kBatchCommands);
    q.thread = std::thread(RenderThreadMain, &s);
  }

  void StopRenderThread(State &s)
  {
    CommandQueue &q = s.queue;

    if (!q.thread.joinable())
      return;

    {
      std::lock_guard lock(q.mutex);
      q.stop = true;
      q.batches.clear();
    }

    q.work_cv.notify_all();
    q.done_cv.notify_all();
    q.thread.join();
  }
}