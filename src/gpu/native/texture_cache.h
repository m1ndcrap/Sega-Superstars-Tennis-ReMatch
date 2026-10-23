// Ntive renderer: textures and samplers

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/native/gfx/gfx.h"
#include "gpu/native/ucode_analysis.h" // TextureFetchUse

namespace sst::native
{
  class TextureCache
  {
  public:
    struct UploadSpace
    {
      uint8_t *cpu = nullptr;
      gfx::Buffer *buffer = nullptr;
      uint64_t offset = 0;
    };

    using AllocateFn = std::function<UploadSpace(size_t size)>;

    static constexpr uint32_t kNone = UINT32_MAX;

    // SRV table slots [srv_first, srv_first + srv_count) and sampler slots [sampler_first, ...) belong to the cache
    bool Initialize(gfx::Device &device, const uint8_t *physical_membase, uint32_t srv_first, uint32_t srv_count, uint32_t sampler_first, uint32_t sampler_count);
    void Shutdown();

    // frame: frame number; completed_fence: the renderer's completed fence value; frame_fence: the value signalled when this frame finishes
    void BeginFrame(uint64_t frame, uint64_t completed_fence, uint64_t frame_fence);

    // fetch: the 6 fetch constant dwords (host endian). fetch_dimension: the xenos::FetchOpDimension the shader samples with. Records uploads on `list`. Returns the SRV heap index, or kNone (bind a placeholder)
    uint32_t GetTexture(gfx::CommandList &list, const AllocateFn &allocate, const uint32_t fetch[6], uint32_t fetch_dimension);

    // Render-target resolve destinations: textures the renderer draws itself. A fetch constant whose base address is a registered page samples this resource (when the size matches) instead of guest memory
    struct External
    {
      gfx::Texture *resource = nullptr;
      uint32_t dxgi_format = 0;
      uint32_t width = 0, height = 0, slices = 1;
      bool volume = false; // Texture3D (stacked textures sampled with 3D fetches)
    };

    void SetExternal(uint32_t base_page, const External &e);
    void RemoveExternal(uint32_t base_page);

    // Sampler heap index for the fetch constant + the shader's overrides
    uint32_t GetSampler(const uint32_t fetch[6], const TextureFetchUse &use);

    std::string StatusLine() const;

    // Copies of guest texture data the game thread made when it captured this frame's draws (physical [begin, end)); valid until ClearSnapshots at the end of the frame
    void AddSnapshot(uint32_t begin, uint32_t end, const uint8_t *data);

    // The game thread hashed this texture (base page) at draw time instead of copying it: unchanged since its last copy
    void AddUnchanged(uint32_t base_page, uint64_t hash) { unchanged_.push_back({base_page, hash}); }

    void ClearSnapshots()
    {
      snapshots_.clear();
      unchanged_.clear();
    }

    // Running totals for the renderer's slow-frame report (renderer_profile.cpp)
    struct Counters
    {
      uint64_t created = 0, uploads = 0, upload_bytes = 0, hashed_bytes = 0;
      double upload_ms = 0, hash_ms = 0;
    };

    Counters TotalCounters() const { return {stats_.created, stats_.uploads, stats_.upload_bytes, stats_.hashed_bytes, stats_.upload_ms, stats_.hash_ms}; }

  private:
    struct Snapshot
    {
      uint32_t begin, end;
      const uint8_t *data;
    };

    std::vector<Snapshot> snapshots_;
    std::vector<std::pair<uint32_t, uint64_t>> unchanged_; // (base page, content hash) this frame

    // Where to read guest physical [addr, addr + size): `result + addr` is a frame snapshot covering it, else guest memory
    const uint8_t *Source(uint32_t addr, uint32_t size) const;

    struct Key
    {
      uint32_t base_page, mip_page;
      uint32_t width, height, depth_or_array;
      uint32_t format, endian, dimension, tiled, packed, pitch;
      uint32_t mip_min, mip_max, signed_mode;
    };

    struct Texture
    {
      Key key{};
      gfx::TextureRef resource;
      uint32_t dxgi_format = 0;
      uint32_t host_width = 0, host_height = 0, levels = 0, array_size = 1, depth = 1;
      uint64_t content_hash = 0;
      uint64_t checked_frame = UINT64_MAX, used_frame = 0;
      uint32_t stable_checks = 0; // consecutive checks with unchanged data
      uint64_t bytes = 0;
      uint32_t base_size = 0, mip_size = 0;         // guest extents (bytes) from base / mip address
      bool resident = false;                        // uploaded at least once
      int prompt_page = -1;                         // a button-icon atlas page (prompts.h), else -1
      uint32_t prompt_generation = 0;               // prompts::Generation() it was uploaded with
      std::unordered_map<uint32_t, uint32_t> views; // swizzle | view kind << 12 -> SRV slot
    };

    struct Deferred
    {
      uint64_t fence;
      gfx::TextureRef resource;
      std::vector<uint32_t> slots;
    };

    Texture *Create(const Key &key, uint64_t hash);
    bool Upload(gfx::CommandList &list, const AllocateFn &allocate, Texture &t);
    uint64_t HashGuest(const Texture &t) const;
    uint32_t AllocateSrvSlot();
    void Evict();

    gfx::Device *device_ = nullptr;
    const uint8_t *membase_ = nullptr;
    uint32_t srv_next_ = 0, srv_end_ = 0;
    std::vector<uint32_t> srv_free_;
    uint32_t sampler_first_ = 0, sampler_next_ = 0, sampler_end_ = 0;
    std::unordered_map<uint64_t, uint32_t> samplers_;

    std::unordered_map<uint64_t, std::unique_ptr<Texture>> textures_;

    struct ExternalEntry
    {
      External info;
      std::unordered_map<uint32_t, uint32_t> views; // swizzle | kind << 12 -> SRV slot
    };

    std::unordered_map<uint32_t, ExternalEntry> externals_;
    uint32_t GetExternalView(ExternalEntry &e, uint32_t guest_format, uint32_t swizzle, uint32_t fetch_dimension, uint32_t width, uint32_t height);
    std::vector<Deferred> deferred_;
    uint64_t frame_ = 0, frame_fence_ = 0;
    uint64_t total_bytes_ = 0;
    uint64_t frame_upload_bytes_ = 0;

    struct Stats
    {
      uint64_t created = 0, uploads = 0, reuploads = 0, upload_bytes = 0, hashed_bytes = 0;
      double upload_ms = 0, hash_ms = 0;
      uint64_t unsupported = 0, bad = 0, dimension_mismatch = 0, evicted = 0, srv_full = 0, sampler_full = 0, failed = 0;
      uint64_t lookups = 0, deferred_loads = 0, checks_skipped = 0, external_hits = 0, external_mismatch = 0;
      uint64_t from_snapshots = 0;
      uint64_t unchanged_hits = 0, unchanged_misses = 0; // game-thread "unchanged" hashes matching / not matching the resident texture
      uint32_t logged = 0;
    } stats_;

    std::unordered_map<uint64_t, uint64_t> logged_once_;
    bool LogOnce(uint64_t key) { return logged_once_.emplace(key, 0).second; }
  };
}