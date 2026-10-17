// Running memory-export vertex shaders on the CPU: splitting big draws across worker threads, the background queue, and skipping runs whose inputs and output are unchanged. The interpreter itself is cpu_shader.cpp.
// Derived from Xenia's ShaderInterpreter and memexport translators (copyright 2022 Ben Vanik, all rights reserved, released under the BSD license (ReXGlue SDK adaptation by Tom Clay, 2026)).

#include "gpu/native/memexport.h"

#include "gpu/native/cpu_shader.h"
#include "gpu/native/cpu_shader_values.h"
#include "gpu/native/ucode_analysis.h"

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/xenos.h>

#include <fmt/format.h>
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sst::native
{
  // Background helper threads yield to the game's own threads when cores are busy
  inline void LowerThreadPriority()
  {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  }

  using cpu_shader::Interpreter;
  using cpu_shader::kGuestPhysicalSize;
  using cpu_shader::kLanes;
  using cpu_shader::NeedsPerVertexFlow;

  // Persistent worker threads: a job is split into parts that the workers and the calling thread take in turn.
  struct MemexportRunner::Pool
  {
    explicit Pool(uint32_t n)
    {
      for (uint32_t i = 0; i < n; ++i)
        threads.emplace_back([this]
                             { Worker(); });
    }

    ~Pool()
    {
      {
        std::lock_guard lock(mutex);
        quit = true;
      }

      wake.notify_all();

      for (auto &t : threads)
        t.join();
    }

    // Runs fn(0..parts-1) on the workers and the caller; returns when all are done.
    void Run(uint32_t parts, const std::function<void(uint32_t)> &fn)
    {
      {
        std::lock_guard lock(mutex);
        job = &fn;
        job_parts = parts;
        next.store(0);
        remaining = parts;
        ++generation;
      }

      wake.notify_all();
      Work(fn, parts);
      std::unique_lock lock(mutex);

      // Also wait for workers to leave the job, so none can pick up a part index of the next job with this one's function.
      done.wait(lock, [&]
                { return remaining == 0 && active == 0; });
      job = nullptr;
    }

  private:
    void Work(const std::function<void(uint32_t)> &fn, uint32_t parts)
    {
      for (uint32_t part; (part = next.fetch_add(1)) < parts;)
      {
        fn(part);
        std::lock_guard lock(mutex);

        if (--remaining == 0)
          done.notify_all();
      }
    }

    void Worker()
    {
      LowerThreadPriority();
      uint64_t seen = 0;

      for (;;)
      {
        const std::function<void(uint32_t)> *fn;
        uint32_t parts;

        {
          std::unique_lock lock(mutex);
          wake.wait(lock, [&]
                    { return quit || (generation != seen && job); });

          if (quit)
            return;

          seen = generation;
          fn = job;
          parts = job_parts;
          ++active;
        }

        Work(*fn, parts);
        std::lock_guard lock(mutex);

        if (--active == 0)
          done.notify_all();
      }
    }

  public:
    std::vector<std::thread> threads;

  private:
    std::mutex mutex;
    std::condition_variable wake, done;
    const std::function<void(uint32_t)> *job = nullptr;
    uint32_t job_parts = 0, remaining = 0, active = 0;
    std::atomic<uint32_t> next{0};
    uint64_t generation = 0;
    bool quit = false;
  };

  // Background dispatcher: runs submitted jobs in order (each one split across the pool as usual).
  struct MemexportRunner::Queue
  {
    explicit Queue(MemexportRunner *runner) : runner(runner), thread([this]
                                                                     { Main(); }) {}

    ~Queue()
    {
      {
        std::lock_guard lock(mutex);
        quit = true;
      }

      wake.notify_all();
      thread.join();
    }

    uint64_t Submit(std::shared_ptr<MemexportJob> job)
    {
      uint64_t ticket;

      {
        std::lock_guard lock(mutex);
        ticket = ++submitted;
        jobs.emplace_back(ticket, std::move(job));
      }

      wake.notify_one();
      return ticket;
    }

    void Wait(uint64_t ticket)
    {
      if (finished.load(std::memory_order_acquire) >= ticket)
        return;

      std::unique_lock lock(mutex);
      done.wait(lock, [&]
                { return finished.load(std::memory_order_acquire) >= ticket; });
    }

    void Main()
    {
      LowerThreadPriority();

      for (;;)
      {
        std::pair<uint64_t, std::shared_ptr<MemexportJob>> next;

        {
          std::unique_lock lock(mutex);
          wake.wait(lock, [&]
                    { return quit || !jobs.empty(); });

          if (jobs.empty())
            return; // quit with nothing left

          next = std::move(jobs.front());
          jobs.pop_front();
        }

        runner->Execute(*next.second);

        {
          std::lock_guard lock(mutex);
          finished.store(next.first, std::memory_order_release);
        }

        done.notify_all();
      }
    }

    MemexportRunner *runner;
    std::mutex mutex;
    std::condition_variable wake, done;
    std::deque<std::pair<uint64_t, std::shared_ptr<MemexportJob>>> jobs;
    uint64_t submitted = 0;
    std::atomic<uint64_t> finished{0};
    bool quit = false;
    std::thread thread; // last: starts once the rest is constructed
  };

  MemexportRunner::MemexportRunner(uint8_t *physical_membase, uint32_t worker_threads) : membase_(physical_membase)
  {
    if (worker_threads)
      pool_ = std::make_unique<Pool>(worker_threads);
  }

  MemexportRunner::~MemexportRunner()
  {
    queue_.reset(); // finishes queued jobs (they use the pool)
  }

  void MemexportRunner::Execute(MemexportJob &job)
  {
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t dwords = uint32_t(job.ucode.size());
    const bool output_ok = job.hash_size && uint64_t(job.hash_begin) + job.hash_size <= kGuestPhysicalSize;

    if (job.use_cache && output_ok)
    {
      XXH3_state_t *st = XXH3_createState();
      XXH3_64bits_reset(st);
      XXH3_64bits_update(st, job.ucode.data(), job.ucode.size() * 4);

      // Only the constants the shader reads (the rest of the file holds whatever other shaders left there, which changes between passes).
      uint64_t used[4];

      if (job.constants.size() >= 1024 && FloatConstantsRead(job.ucode.data(), dwords, used))
      {
        for (uint32_t c = 0; c < 256; ++c)
          if ((used[c >> 6] >> (c & 63)) & 1)
            XXH3_64bits_update(st, &job.constants[4 * c], 16);
      }
      else
      {
        XXH3_64bits_update(st, job.constants.data(), job.constants.size() * 4);
      }

      XXH3_64bits_update(st, job.bools, sizeof(job.bools));
      XXH3_64bits_update(st, job.loops, sizeof(job.loops));
      XXH3_64bits_update(st, job.indices.data(), job.indices.size() * 4);
      uint32_t slots[16];
      const uint32_t nslots = VertexFetchSlots(job.ucode.data(), dwords, slots, 16);
      bool hashed = true;

      for (uint32_t i = 0; i < nslots; ++i)
      {
        XXH3_64bits_update(st, &job.fetch[2 * slots[i]], 8);
        const uint32_t phys = job.fetch[2 * slots[i]] & 0x1FFFFFFCu;
        const uint32_t size = job.fetch[2 * slots[i] + 1] & 0x03FFFFFCu;

        if (size > (8u << 20) || uint64_t(phys) + size > kGuestPhysicalSize)
        {
          hashed = false;
          break;
        }

        XXH3_64bits_update(st, membase_ + phys, size);
      }

      job.input_hash = hashed ? XXH3_64bits_digest(st) : 0;
      XXH3_freeState(st);

      if (hashed && job.has_previous && job.input_hash == job.previous_input_hash && XXH3_64bits(membase_ + job.hash_begin, job.hash_size) == job.previous_output_hash)
      {
        job.ok = job.skipped = true;
        job.output_hash = job.previous_output_hash;
        job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return;
      }
    }

    VsConstants vc;
    vc.float_constants = job.constants.data();
    vc.bool_constants = job.bools;
    vc.loop_constants = job.loops;
    vc.fetch_constants = job.fetch;
    job.ok = Run(job.ucode.data(), dwords, vc, job.indices.data(), uint32_t(job.indices.size()), &job.stats);

    if (job.ok && output_ok)
      job.output_hash = XXH3_64bits(membase_ + job.hash_begin, job.hash_size);

    job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }

  uint64_t MemexportRunner::Submit(std::shared_ptr<MemexportJob> job)
  {
    if (!queue_)
      queue_ = std::make_unique<Queue>(this);

    return queue_->Submit(std::move(job));
  }

  void MemexportRunner::Wait(uint64_t ticket)
  {
    if (queue_)
      queue_->Wait(ticket);
  }

  uint64_t MemexportRunner::finished() const
  {
    return queue_ ? queue_->finished.load(std::memory_order_acquire) : 0;
  }

  uint32_t MemexportRunner::worker_threads() const
  {
    return pool_ ? uint32_t(pool_->threads.size()) : 0;
  }

  bool MemexportRunner::Run(const uint32_t *ucode, uint32_t dwords, const VsConstants &constants, const uint32_t *vertex_indices, uint32_t count, MemexportStats *stats, bool force_per_vertex)
  {
    if (!ucode || dwords < 3 || !constants.float_constants || !constants.bool_constants || !constants.fetch_constants || !membase_)
      return false;

    uint32_t temp_count;
    const bool per_vertex = NeedsPerVertexFlow(ucode, dwords, &temp_count) || force_per_vertex;
    const uint32_t lanes = per_vertex ? 1 : kLanes;

    if (per_vertex)
      ++stats->per_vertex_draws;

    // Vertices are independent and write disjoint elements: large draws are split across the worker threads (parts of 8 batches).
    constexpr uint32_t kPartVertices = 8 * kLanes;

    if (pool_ && !per_vertex && count >= 2 * kPartVertices)
    {
      const uint32_t parts = (count + kPartVertices - 1) / kPartVertices;
      std::mutex stats_mutex;

      const std::function<void(uint32_t)> fn = [&](uint32_t part)
      {
        MemexportStats local;
        Interpreter interpreter(ucode, dwords, constants, membase_, &local, temp_count);
        const uint32_t begin = part * kPartVertices;
        const uint32_t end = std::min(count, begin + kPartVertices);

        for (uint32_t i = begin; i < end; i += lanes)
          interpreter.Run(vertex_indices + i, std::min(lanes, end - i));

        std::lock_guard lock(stats_mutex);
        stats->elements += local.elements;
        stats->bad_address += local.bad_address;
        stats->bad_format += local.bad_format;
        stats->out_of_range += local.out_of_range;
        stats->fetch_oob += local.fetch_oob;
        stats->malformed += local.malformed;

        if (local.elements)
        {
          stats->last_format = local.last_format;
          stats->last_base = local.last_base;
          stats->last_count = local.last_count;
        }
      };

      pool_->Run(parts, fn);
    }
    else
    {
      Interpreter interpreter(ucode, dwords, constants, membase_, stats, temp_count);

      for (uint32_t i = 0; i < count; i += lanes)
        interpreter.Run(vertex_indices + i, std::min(lanes, count - i));
    }

    stats->vertices += count;
    return true;
  }

  bool MemexportRunner::RunFetchIndices(const uint32_t *ucode, uint32_t dwords, const VsConstants &constants, const uint32_t *vertex_indices, uint32_t count, std::unordered_map<uint32_t, std::vector<int32_t>> *records)
  {
    if (!ucode || dwords < 3 || !constants.float_constants || !constants.bool_constants || !constants.fetch_constants || !membase_ || !records)
      return false;

    records->clear();
    uint32_t temp_count;
    const bool per_vertex = NeedsPerVertexFlow(ucode, dwords, &temp_count);
    const uint32_t lanes = per_vertex ? 1 : kLanes;
    MemexportStats ignored;
    Interpreter interpreter(ucode, dwords, constants, membase_, &ignored, temp_count);
    interpreter.fetch_records_ = records;

    for (uint32_t i = 0; i < count; i += lanes)
    {
      interpreter.fetch_lane_base_ = i;
      interpreter.Run(vertex_indices + i, std::min(lanes, count - i));
    }

    return true;
  }

  bool MemexportRunner::RunPositions(const uint32_t *ucode, uint32_t dwords, const VsConstants &constants, const uint32_t *vertex_indices, uint32_t count, float *positions, MemexportStats *stats)
  {
    if (!ucode || dwords < 3 || !constants.float_constants || !constants.bool_constants || !constants.fetch_constants || !membase_)
      return false;

    uint32_t temp_count;
    const bool per_vertex = NeedsPerVertexFlow(ucode, dwords, &temp_count);
    const uint32_t lanes = per_vertex ? 1 : kLanes;
    Interpreter interpreter(ucode, dwords, constants, membase_, stats, temp_count);

    for (uint32_t i = 0; i < count; i += lanes)
      interpreter.Run(vertex_indices + i, std::min(lanes, count - i), positions + 4 * size_t(i));

    return true;
  }

  uint32_t MemexportElementSize(uint32_t format)
  {
    const float zero[4] = {};
    uint32_t packed[4];
    return cpu_shader::PackElement(zero, format << 8, packed);
  }
}