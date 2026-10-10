// sst_rematch - native renderer: D3D12 frame recording and draw submission

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu/native/memexport.h"

#include <d3d12.h>
#include <wrl/client.h>

namespace rex::ui::d3d12
{
  class D3D12Provider;
}

namespace sst::native
{

  class ShaderCache;
  struct ShaderVariant;
  struct ShaderEntry;

  constexpr uint32_t kMaxStreams = 16;

  struct DrawCall
  {
    uint32_t device = 0;
    uint32_t prim = 0; // xenos::PrimitiveType
    bool indexed = false;
    uint32_t base_vertex = 0; // indexed: added to each index (VGT_INDX_OFFSET)
    uint32_t start = 0;       // first index (indexed) or first vertex
    uint32_t count = 0;       // index or vertex count
    uint32_t up_data = 0;     // DrawVerticesUP: guest address of stream 0 data
    uint32_t up_stride = 0;
  };

  class Renderer
  {
  public:
    static constexpr uint32_t kSlots = 3;

    // physical_membase: host address of guest physical memory (fetch constants hold physical addresses)
    bool Initialize(const rex::ui::d3d12::D3D12Provider &provider, ShaderCache *shaders,
                    const uint8_t *physical_membase, uint32_t width, uint32_t height);
    void Shutdown();

    // render thread
    void Clear(const uint8_t *base, uint32_t device, uint32_t flags, uint32_t color, float z,
               uint32_t stencil);
    void Draw(const uint8_t *base, const DrawCall &call);
    void EndFrame(); // Swap
    void LogStats(uint64_t frame);
    uint64_t frames() const { return submitted_; }

    // plugin (command-processor thread)
    ID3D12Resource *Acquire(uint32_t *slot, uint32_t *width, uint32_t *height);
    void Release(uint32_t slot);

  private:
    struct UploadPage
    {
      Microsoft::WRL::ComPtr<ID3D12Resource> resource;
      uint8_t *cpu = nullptr;
      D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
      size_t size = 0;
    };
    struct FrameResources
    {
      Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
      uint64_t fence_value = 0;
      std::vector<UploadPage> pages;
      size_t page = 0, offset = 0;
    };
    struct Upload
    {
      uint8_t *cpu = nullptr;
      D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    };

    bool CreateRootSignature();
    bool CreateDescriptorsAndPlaceholders();
    void CreateDebugPixelShader();
    Upload Allocate(size_t size, size_t alignment = 256);
    void BeginFrame();
    void WaitFence(uint64_t value);
    ShaderEntry *ResolveShader(const uint8_t *base, uint32_t object, bool vertex);
    // Memory-export (morph) draws: run on the CPU, writing guest memory.
    void RunMemexport(const uint8_t *base, const DrawCall &call, uint32_t vs_obj,
                      const ShaderEntry *vs_entry);
    void RecordMemexportRanges(const uint8_t *base, uint32_t dev);
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t slot) const;

    ID3D12PipelineState *GetPipeline(const struct PipelineKey &key,
                                     const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout,
                                     const ShaderVariant *vs, const ShaderVariant *ps);

    const rex::ui::d3d12::D3D12Provider *provider_ = nullptr;
    ID3D12Device *device_ = nullptr;
    ShaderCache *shaders_ = nullptr;
    const uint8_t *physical_membase_ = nullptr;
    uint32_t width_ = 0, height_ = 0;

    // Output ring + depth
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kSlots> outputs_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_, dsv_heap_;
    uint32_t rtv_size_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_;

    // Bindless heaps + placeholders
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_, sampler_heap_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> placeholder_textures_;
    Microsoft::WRL::ComPtr<ID3D12Resource> zero_buffer_; // for VS inputs the decl lacks

    // Frame recording
    std::array<FrameResources, kSlots> frames_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    uint64_t submitted_ = 0;
    bool recording_ = false;
    uint32_t frame_res_ = 0; // index into frames_
    uint32_t target_ = 0;    // output slot being rendered

    // Guest physical ranges written by memexport draws that were *not* run on the CPU this frame (--sst_native_memexport=false or failure); draws fetching vertices from them are skipped.
    std::vector<std::pair<uint32_t, uint32_t>> memexport_ranges_;

    // Ranges the CPU memexport wrote this / last frame (diagnostics)
    std::vector<std::pair<uint32_t, uint32_t>> memexport_written_, memexport_written_prev_;

    std::unordered_set<uint32_t> main_rts_, other_rts_;
    float last_depth_clear_ = 1.0f;

    std::unique_ptr<ShaderVariant> debug_ps_; // --sst_native_debug_shading

    std::unique_ptr<MemexportRunner> memexport_;
    MemexportStats memexport_stats_;
    struct MemexportCacheEntry
    {
      uint64_t input_hash = 0, output_hash = 0;
      uint32_t output_begin = 0, output_size = 0;
    };
    std::unordered_map<uint64_t, MemexportCacheEntry> memexport_cache_;
    std::vector<uint32_t> memexport_ucode_, memexport_indices_;
    std::vector<float> memexport_constants_;

    // Pipelines
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_;
    std::unordered_set<uint64_t> failed_pipelines_;

    // Hand-off
    std::mutex slot_mutex_;
    int latest_ = -1;
    int pinned_ = -1;

    // render thread stats
    struct Stats
    {
      uint64_t draws = 0, drawn = 0;
      uint64_t skip_shader = 0, skip_pending = 0, skip_decl = 0, skip_prim = 0, skip_rt = 0,
               skip_pipeline = 0, skip_data = 0, skip_cull = 0, skip_memexport = 0;
      uint64_t memexport_draws = 0, memexport_run = 0, memexport_cached = 0, memexport_failed = 0;
      double memexport_ms = 0;
      uint64_t memexport_bad_outputs = 0, memexport_bad_logged = 0;
      uint64_t morph_consumers = 0, morph_consumer_overrun = 0, morph_consumer_stale = 0,
               morph_consumer_bad = 0, morph_consumer_bad_logged = 0;
      uint64_t clears = 0, clear_logged = 0, sample_logged = 0, depth_only = 0, shaders_from_objects = 0, skip_shader_logged = 0;
      uint64_t upload_bytes = 0;
      uint64_t pipelines_created = 0, pipelines_failed = 0;
      std::unordered_map<uint32_t, uint64_t> prims;          // xenos prim -> draws
      std::unordered_map<uint32_t, uint64_t> clear_flags;    // flags -> count
      std::unordered_map<uint64_t, uint64_t> render_targets; // (rt0 object<<32 | size word)
    } stats_;
    uint64_t last_stats_frame_ = 0;
    std::unordered_set<uint64_t> logged_once_;
    bool LogOnce(uint64_t key) { return logged_once_.insert(key).second; }
  };
}