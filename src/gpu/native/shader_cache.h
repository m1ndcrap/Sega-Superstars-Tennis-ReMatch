// sst_rematch - native renderer: shader cache

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sst_translate.h>

namespace rex::ui::d3d12
{
  class D3D12Provider;
}

namespace sst::native
{

  struct Worker; // per compile thread (shader_cache.cpp)

  enum class ShaderStage : uint8_t
  {
    kVertex,
    kPixel
  };

  struct ShaderVariant
  {
    enum class State : uint8_t
    {
      kQueued,
      kReady,
      kFailed
    };
    std::atomic<State> state{State::kQueued};
    std::vector<uint8_t> dxil; // valid once kReady
  };

  struct ShaderEntry
  {
    uint64_t hash = 0;
    ShaderStage stage = ShaderStage::kVertex;
    bool memexport = false;                             // not translatable yet (custom path later)
    std::vector<uint8_t> container;                     // big-endian, as the game passed it
    std::vector<sst::xenos::VertexInput> vertex_inputs; // VS only: semantics it reads
    uint32_t ucode_offset = 0;                          // VS only: microcode offset in the physical part
    std::atomic<uint32_t> spec_mask{0};                 // spec-constant bits the shader reads (after variant 0)
    std::atomic<bool> translated{false};                // spec_mask valid

    std::mutex variants_mutex;
    std::unordered_map<uint32_t, std::unique_ptr<ShaderVariant>> variants; // by spec value
  };

  class ShaderCache
  {
  public:
    bool Initialize(const rex::ui::d3d12::D3D12Provider &provider,
                    const std::filesystem::path &cache_dir);
    void Shutdown();

    // Game thread
    ShaderEntry *Register(const uint8_t *container, uint32_t size);

    // Associates a guest shader object (Create*Shader's return value) with its entry
    void BindObject(uint32_t guest_object, ShaderEntry *entry);
    ShaderEntry *FindObject(uint32_t guest_object) const;

    // Variant for `spec` (masked to the bits the shader uses). Queues it if missing. Returns nullptr while not ready (or failed).
    const ShaderVariant *GetVariant(ShaderEntry *entry, uint32_t spec);

    // Logs a one-line summary of the work done so far
    void LogStats(const char *when) const;
    bool Idle() const { return pending_.load() == 0; }

  private:
    struct Job
    {
      ShaderEntry *entry;
      uint32_t spec;
      ShaderVariant *variant;
    };

    void WorkerMain(unsigned index);
    void Build(Worker &worker, const Job &job);

    std::filesystem::path FilePath(uint64_t hash, uint32_t spec) const;

    const rex::ui::d3d12::D3D12Provider *provider_ = nullptr;
    std::filesystem::path cache_dir_;

    mutable std::shared_mutex entries_mutex_;
    std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> entries_;
    std::unordered_map<uint32_t, ShaderEntry *> objects_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Job> queue_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;

  public:
    struct Stats
    {
      std::atomic<uint32_t> registered{0}, memexport{0};
      std::atomic<uint32_t> from_disk{0}, compiled{0}, failed{0};
      std::atomic<uint64_t> compile_us{0};
      std::atomic<bool> unsigned_warned{false};
    };

  private:
    Stats stats_;
    std::atomic<uint32_t> pending_{0};
  };
}