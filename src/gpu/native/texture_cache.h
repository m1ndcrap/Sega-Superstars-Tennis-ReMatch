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
  // Guest data the game rewrites while the game runs. With the render thread a frame behind, the game may already have written the
  // next frame's data when the renderer reads it, so the game thread copies it when it captures a draw (native.cpp) and the renderer
  // reads the copies. The render thread records what it has seen; the game thread reads a copy of the set when it changes.
  //  - textures: every texture the renderer knows gets flagged `changing` once its data changed (unflagged a minute later).
  //    The game thread copies changing textures for every draw, and all known textures for UI-sized draws (where a one-frame
  //    mismatch is most visible: portraits, court previews, text pages reloaded as screens change).
  //  - vertex buffers: start addresses whose data changed between frames, with the bytes draws read from them.
  namespace guest_watch
  {
    struct TextureRange
    {
      uint32_t base_addr = 0, base_size = 0; // physical
      uint32_t mip_addr = 0, mip_size = 0;
      bool changing = false;
      bool external = false; // a render-target resolve destination: the renderer samples its own copy, never guest memory
    };

    struct Set
    {
      std::unordered_map<uint32_t, TextureRange> textures; // by base page (physical address >> 12)
      std::unordered_map<uint32_t, uint32_t> vertices;     // physical start -> bytes read
    };

    void KnownTexture(uint32_t base_page, const TextureRange &range);    // render thread: texture created
    void TextureChanged(uint32_t base_page, uint64_t frame);             // render thread: its data changed
    void ExternalTexture(uint32_t base_page, bool external);             // render thread: became / stopped being a resolve destination
    void VerticesChanged(uint32_t phys, uint32_t bytes, uint64_t frame); // render thread: vertex data changed between frames
    void Prune(uint64_t frame);
    uint64_t Version(); // changes whenever the set does
    void Copy(Set &out);

    // Float constant chunks a shader's draws read: bit i = c[16i .. 16i+15] (Renderer::ConstantChunkMask; memory-export shaders and
    // shaders with relative constant addressing: all 16), keyed by shader object and microcode pointer (ShaderKey). The game thread
    // compares only those constant chunks of the device block per draw. Only ever grows per key (bits are ORed in).
    inline uint64_t ShaderKey(uint32_t object, uint32_t ucode) { return (uint64_t(object) << 32) | ucode; }
    void ConstantUse(uint64_t key, uint32_t chunk_mask); // render thread
    uint64_t ConstantUseVersion();
    void CopyConstantUse(std::unordered_map<uint64_t, uint32_t> &out);
  }

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
    // Render-target resolve destinations: textures the renderer draws itself. A fetch constant whose base address is a registered page samples this resource (when the size matches) instead of guest memory
    struct External
    {
      ID3D12Resource *resource = nullptr;
      uint32_t dxgi_format = 0;
      uint32_t width = 0, height = 0, slices = 1;
      bool volume = false; // Texture3D (stacked textures sampled with 3D fetches)
    };

    void SetExternal(uint32_t base_page, const External &e);
    void RemoveExternal(uint32_t base_page);

    // Sampler heap index for the fetch constant + the shader's overrides
    uint32_t GetSampler(const uint32_t fetch[6], const TextureFetchUse &use);

    std::string StatusLine() const;

    // Copies of guest texture data the game thread made when it captured this frame's draws (physical [begin, end)); valid until
    // ClearSnapshots at the end of the frame
    void AddSnapshot(uint32_t begin, uint32_t end, const uint8_t *data);
    // The game thread hashed this texture (base page) at draw time instead of copying it: unchanged since its last copy
    void AddUnchanged(uint32_t base_page, uint64_t hash) { unchanged_.push_back({base_page, hash}); }
    void ClearSnapshots()
    {
      snapshots_.clear();
      unchanged_.clear();
    }

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
      Microsoft::WRL::ComPtr<ID3D12Resource> resource;
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