// sst_rematch - native renderer: textures and samplers

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/native/memexport.h" // TextureFetchUse

#include <d3d12.h>
#include <wrl/client.h>

namespace sst::native
{
  class TextureCache
  {
  public:
    struct UploadSpace
    {
      uint8_t *cpu = nullptr;
      ID3D12Resource *resource = nullptr;
      uint64_t offset = 0;
    };

    using AllocateFn = std::function<UploadSpace(size_t size)>;

    static constexpr uint32_t kNone = UINT32_MAX;

    bool Initialize(ID3D12Device *device, const uint8_t *physical_membase, ID3D12DescriptorHeap *srv_heap, uint32_t srv_first, uint32_t srv_count, ID3D12DescriptorHeap *sampler_heap, uint32_t sampler_first, uint32_t sampler_count);
    void Shutdown();

    // frame: frame number; completed_fence: the renderer's completed fence value; frame_fence: the value signalled when this frame finishes
    void BeginFrame(uint64_t frame, uint64_t completed_fence, uint64_t frame_fence);

    // fetch: the 6 fetch constant dwords (host endian). fetch_dimension: the xenos::FetchOpDimension the shader samples with. Records uploads on `list`. Returns the SRV heap index, or kNone (bind a placeholder)
    uint32_t GetTexture(ID3D12GraphicsCommandList *list, const AllocateFn &allocate, const uint32_t fetch[6], uint32_t fetch_dimension);

    // Sampler heap index for the fetch constant + the shader's overrides
    uint32_t GetSampler(const uint32_t fetch[6], const TextureFetchUse &use);

    std::string StatusLine() const;

  private:
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
      Microsoft::WRL::ComPtr<ID3D12Resource> resource;
      uint32_t dxgi_format = 0;
      uint32_t host_width = 0, host_height = 0, levels = 0, array_size = 1, depth = 1;
      uint64_t content_hash = 0;
      uint64_t checked_frame = UINT64_MAX, used_frame = 0;
      uint32_t stable_checks = 0; // consecutive checks with unchanged data
      uint64_t bytes = 0;
      uint32_t base_size = 0, mip_size = 0;         // guest extents (bytes) from base / mip address
      bool resident = false;                        // uploaded at least once
      std::unordered_map<uint32_t, uint32_t> views; // swizzle | view kind << 12 -> SRV slot
    };

    struct Deferred
    {
      uint64_t fence;
      Microsoft::WRL::ComPtr<ID3D12Resource> resource;
      std::vector<uint32_t> slots;
    };

    Texture *Create(const Key &key, uint64_t hash);
    bool Upload(ID3D12GraphicsCommandList *list, const AllocateFn &allocate, Texture &t);
    uint64_t HashGuest(const Texture &t) const;
    uint32_t AllocateSrvSlot();
    void Evict();

    ID3D12Device *device_ = nullptr;
    const uint8_t *membase_ = nullptr;
    ID3D12DescriptorHeap *srv_heap_ = nullptr;
    ID3D12DescriptorHeap *sampler_heap_ = nullptr;
    uint32_t srv_size_ = 0, sampler_size_ = 0;
    uint32_t srv_next_ = 0, srv_end_ = 0;
    std::vector<uint32_t> srv_free_;
    uint32_t sampler_first_ = 0, sampler_next_ = 0, sampler_end_ = 0;
    std::unordered_map<uint64_t, uint32_t> samplers_;

    std::unordered_map<uint64_t, std::unique_ptr<Texture>> textures_;
    std::vector<Deferred> deferred_;
    uint64_t frame_ = 0, frame_fence_ = 0;
    uint64_t total_bytes_ = 0;
    uint64_t frame_upload_bytes_ = 0;

    struct Stats
    {
      uint64_t created = 0, uploads = 0, reuploads = 0, upload_bytes = 0, hashed_bytes = 0;
      double upload_ms = 0, hash_ms = 0;
      uint64_t unsupported = 0, bad = 0, dimension_mismatch = 0, evicted = 0, srv_full = 0, sampler_full = 0, failed = 0;
      uint64_t lookups = 0, deferred_loads = 0, checks_skipped = 0;
      uint32_t logged = 0;
    } stats_;

    std::unordered_map<uint64_t, uint64_t> logged_once_;
    bool LogOnce(uint64_t key) { return logged_once_.emplace(key, 0).second; }
  };
}