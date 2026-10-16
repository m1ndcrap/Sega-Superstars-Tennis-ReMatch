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

#include "gpu/native/shader_pack.h"

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

  // DXIL bytes: owned (compiled or read from disk) or a view into the exe's embedded shader pack
  class DxilBlob
  {
  public:
    const uint8_t *data() const { return view_ ? view_ : owned_.data(); }
    size_t size() const { return view_ ? view_size_ : owned_.size(); }
    bool empty() const { return size() == 0; }

    template <class It>
    void assign(It first, It last)
    {
      view_ = nullptr;
      owned_.assign(first, last);
    }

    void resize(size_t n)
    {
      view_ = nullptr;
      owned_.resize(n);
    }

    uint8_t *writable() { return owned_.data(); } // after resize

    void clear()
    {
      view_ = nullptr;
      view_size_ = 0;
      owned_.clear();
    }

    void set_view(const uint8_t *p, size_t n)
    {
      owned_.clear();
      view_ = p;
      view_size_ = n;
    }

  private:
    std::vector<uint8_t> owned_;
    const uint8_t *view_ = nullptr;
    size_t view_size_ = 0;
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
    DxilBlob dxil;                   // valid once kReady
    std::atomic<bool> claimed{false}; // a worker took its job (a boosted variant has two queued jobs; the second is skipped)
    std::atomic<bool> boosted{false}; // re-queued at the front because a draw needs it
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

    // Variant for `spec` (masked to the bits the shader uses). Comes from the embedded shader pack when the build baked it; otherwise
    // queues it if missing. Returns nullptr while not ready (or failed). `urgent` (a draw is waiting for it): queued work for this
    // variant moves to the front of the compile queue.
    const ShaderVariant *GetVariant(ShaderEntry *entry, uint32_t spec, bool urgent = false);

    // Logs a one-line summary of the work done so far
    void LogStats(const char *when) const;
    bool Idle() const { return pending_.load() == 0; }

  private:
    struct Job
    {
      ShaderEntry *entry;
      uint32_t spec;
      ShaderVariant *variant;
      bool urgent;
    };

    void Enqueue(const Job &job);
    bool FromPack(ShaderEntry *entry, uint32_t spec, ShaderVariant *variant);

    void WorkerMain(unsigned index);
    void Build(Worker &worker, const Job &job);

    std::filesystem::path FilePath(uint64_t hash, uint32_t spec) const;

    const rex::ui::d3d12::D3D12Provider *provider_ = nullptr;
    std::filesystem::path cache_dir_;
    std::filesystem::path containers_dir_; // <exe>/cache/containers: shaders missing from the pack, for the next build to bake

    mutable std::shared_mutex entries_mutex_;
    std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> entries_;
    std::unordered_map<uint32_t, ShaderEntry *> objects_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Job> queue_;  // shaders created by the game, in creation order
    std::deque<Job> urgent_; // variants a draw is waiting for (taken first)
    bool stopping_ = false;
    std::vector<std::thread> workers_;

    pack::View pack_;                   // the exe's embedded shader pack (empty if the build didn't bake one)
    std::vector<uint8_t> pack_storage_; // the pack unpacked (pack_ and the variants point into it)

    // Debug (--sst_shader_override=<folder>): <HASH>.hlsl files there replace the translator's HLSL for that shader (first line may be
    // "// dxc: <extra args>"); all.args adds DXC arguments to every shader. Overridden shaders bypass the pack and the disk cache.
    struct Override
    {
      std::string source;            // replacement HLSL (empty = translate as usual)
      std::vector<std::wstring> args; // extra DXC arguments
    };

    std::filesystem::path override_dir_;
    std::unordered_map<uint64_t, Override> overrides_;
    std::vector<std::wstring> override_all_args_;
    bool override_all_ = false;

    const Override *FindOverride(uint64_t hash) const;

  public:
    struct Stats
    {
      std::atomic<uint32_t> registered{0}, memexport{0};
      std::atomic<uint32_t> from_pack{0}, from_disk{0}, compiled{0}, failed{0}, boosted{0};
      std::atomic<uint64_t> compile_us{0};
      std::atomic<bool> unsigned_warned{false};
    };

  private:
    Stats stats_;
    std::atomic<uint32_t> pending_{0};
  };
}