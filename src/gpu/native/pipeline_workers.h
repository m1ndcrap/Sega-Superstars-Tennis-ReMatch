// Native renderer: background pipeline compiles and the on-disk pipeline library

#pragma once

#include "gpu/native/renderer_internal.h"

namespace sst::native
{
  using namespace renderer_detail;

  struct Renderer::PipelineWorkers
  {
    using Job = PipelineJob;

    PipelineWorkers(ID3D12Device *device, uint32_t threads, bool use_library, bool fast_shaders) : device(device)
    {
      if (use_library)
        OpenLibrary(fast_shaders);

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

    void OpenLibrary(bool fast_shaders)
    {
      ComPtr<ID3D12Device1> device1;

      if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
        return;

      // SHADER PRECISION: FAST pipelines hold other DXIL under the same names: a file of their own
      path = rex::filesystem::GetExecutableFolder() / "cache" / (fast_shaders ? "pipelines_fast.bin" : "pipelines.bin");

      {
        std::ifstream in(path, std::ios::binary);

        if (in)
          blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      }

      HRESULT hr = E_FAIL;

      if (!blob.empty())
      {
        hr = device1->CreatePipelineLibrary(blob.data(), blob.size(), IID_PPV_ARGS(&library));

        if (FAILED(hr))
          REXLOG_INFO("[sst-native] pipeline cache {} not usable ({:08X}: new driver?); starting a new one", path.string(), uint32_t(hr));
      }

      if (!library)
      {
        blob.clear();
        hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library));

        if (FAILED(hr))
        {
          library.Reset();
          REXLOG_WARN("[sst-native] pipeline cache unavailable ({:08X})", uint32_t(hr));
          return;
        }
      }

      REXLOG_INFO("[sst-native] pipeline cache: {} ({} KB)", path.string(), blob.size() >> 10);
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
      if (!library)
        return false;

      const auto t0 = std::chrono::steady_clock::now();
      job.desc.InputLayout = {job.layout.data(), UINT(job.layout.size())};

      {
        std::lock_guard lock(library_mutex);

        if (FAILED(library->LoadGraphicsPipeline(job.name.c_str(), &job.desc, IID_PPV_ARGS(&job.pso))))
          return false;
      }

      job.from_library = true;
      job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      job.state.store(1, std::memory_order_release);
      return true;
    }

    void Build(Job &job)
    {
      const auto t0 = std::chrono::steady_clock::now();
      job.desc.InputLayout = {job.layout.data(), UINT(job.layout.size())};

      if (library)
      {
        std::lock_guard lock(library_mutex);

        if (SUCCEEDED(library->LoadGraphicsPipeline(job.name.c_str(), &job.desc, IID_PPV_ARGS(&job.pso))))
          job.from_library = true;
      }

      if (!job.pso)
      {
        job.hr = device->CreateGraphicsPipelineState(&job.desc, IID_PPV_ARGS(&job.pso));

        if (SUCCEEDED(job.hr) && library)
        {
          std::lock_guard lock(library_mutex);

          if (SUCCEEDED(library->StorePipeline(job.name.c_str(), job.pso.Get())))
            dirty = true;
        }
      }

      job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      job.state.store(job.pso ? 1 : 2, std::memory_order_release);
    }

    // Writes the library if it gained pipelines (on a worker when `background`).
    void Save()
    {
      std::vector<uint8_t> data;

      {
        std::lock_guard lock(library_mutex);

        if (!library || !dirty)
          return;

        dirty = false;
        data.resize(library->GetSerializedSize());

        if (data.empty() || FAILED(library->Serialize(data.data(), data.size())))
          return;
      }

      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      const auto tmp = std::filesystem::path(path).concat(".tmp");

      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);

        if (!out)
          return;

        out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));

        if (!out)
          return;
      }

      std::filesystem::rename(tmp, path, ec);

      if (!ec)
        saved_bytes = data.size();
    }

    void SaveInBackground()
    {
      if (!library || !dirty || saving.exchange(true))
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
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // background

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

    ID3D12Device *device;
    ComPtr<ID3D12PipelineLibrary> library;
    std::vector<uint8_t> blob; // must outlive the library
    std::filesystem::path path;
    std::mutex mutex, library_mutex;
    std::condition_variable wake;
    std::deque<std::shared_ptr<Job>> queue;
    std::vector<std::thread> workers;
    bool quit = false, dirty = false;
    std::atomic<bool> saving{false};
    size_t saved_bytes = 0;
  };
}