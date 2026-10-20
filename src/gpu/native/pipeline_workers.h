// Native renderer: background pipeline compiles and the on-disk pipeline cache (gfx::PipelineCache)

#pragma once

#include "gpu/native/renderer_internal.h"

namespace sst::native
{
  using namespace renderer_detail;

  struct Renderer::PipelineWorkers
  {
    using Job = PipelineJob;

    PipelineWorkers(gfx::Device &device, uint32_t threads, bool use_library, bool fast_shaders) : device(device)
    {
      // SHADER PRECISION: FAST pipelines hold other shader code under the same names: a file of their own
      if (use_library)
        cache = device.OpenPipelineCache(rex::filesystem::GetExecutableFolder() / "cache" / (fast_shaders ? "pipelines_fast.bin" : "pipelines.bin"));

      for (uint32_t i = 0; i < threads; ++i)
        workers.emplace_back([this]
                             { Main(); });
    }

    ~PipelineWorkers()
    {
      {
        std::lock_guard lock(mutex);
        quit = true;
      }

      wake.notify_all();

      for (auto &t : workers)
        t.join();

      while (saving.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

      Save();
    }

    void Submit(std::shared_ptr<Job> job)
    {
      {
        std::lock_guard lock(mutex);
        queue.push_back(std::move(job));
      }

      wake.notify_one();
    }

    // Pipelines already in the cache file load in well under a millisecond: done right away by the render thread, so the draw isn't skipped
    bool TryLoad(Job &job)
    {
      if (!cache)
        return false;

      const auto t0 = std::chrono::steady_clock::now();
      job.pso = cache->Load(job.name, job.desc);

      if (!job.pso)
        return false;

      job.from_library = true;
      job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      job.state.store(1, std::memory_order_release);
      return true;
    }

    void Build(Job &job)
    {
      const auto t0 = std::chrono::steady_clock::now();

      if (cache)
      {
        job.pso = cache->Load(job.name, job.desc);

        if (job.pso)
          job.from_library = true;
        else
          job.pso = cache->Create(job.name, job.desc, &job.error);
      }
      else
      {
        job.pso = device.CreatePipeline(job.desc, &job.error);
      }

      job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      job.state.store(job.pso ? 1 : 2, std::memory_order_release);
    }

    // Writes the cache file if it gained pipelines
    void Save()
    {
      if (!cache)
        return;

      if (const size_t bytes = cache->Save())
        saved_bytes = bytes;
    }

    void SaveInBackground()
    {
      if (!cache || !cache->Dirty() || saving.exchange(true))
        return;

      std::thread(
          [this]
          {
            Save();
            saving = false;
          })
          .detach();
    }

    void Main()
    {
#if defined(_WIN32)
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // background
#endif

      for (;;)
      {
        std::shared_ptr<Job> job;

        {
          std::unique_lock lock(mutex);
          wake.wait(lock, [&]
                    { return quit || !queue.empty(); });

          if (quit)
            return;

          job = std::move(queue.front());
          queue.pop_front();
        }

        Build(*job);
      }
    }

    gfx::Device &device;
    std::unique_ptr<gfx::PipelineCache> cache;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::shared_ptr<Job>> queue;
    std::vector<std::thread> workers;
    bool quit = false;
    std::atomic<bool> saving{false};
    size_t saved_bytes = 0;
  };
}