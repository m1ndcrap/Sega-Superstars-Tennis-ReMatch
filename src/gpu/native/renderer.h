// sst_rematch - native renderer: D3D12 frame recording and draw submission

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu/native/memexport.h"
#include "gpu/native/texture_cache.h"

#include <d3d12.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#include <wrl/client.h>

namespace rex::ui::d3d12
{
  class D3D12Provider;
}

namespace sst::native
{
  class ShaderCache;
  struct PipelineJob;
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
    bool immediate = false; // BeginVertices/EndVertices (game's immediate-mode helpers)
  };

  struct ResolveCall
  {
    uint32_t device = 0, flags = 0;
    uint32_t src_rect = 0;    // guest D3DRECT* (x1, y1, x2, y2) or 0
    uint32_t dst_texture = 0; // guest texture object (fetch constant at +0x1C)
    uint32_t dst_point = 0;   // guest D3DPOINT* or 0
    uint32_t level = 0, slice = 0;
    uint32_t clear_color = 0; // guest D3DVECTOR4* or 0
    float clear_z = 0;
  };

  // A snapshot of guest memory [begin, end) that GuestPtr / be32 read instead of live memory on the calling thread (render thread: what the hook saw)
  struct GuestOverlay
  {
    uint32_t begin = 0, end = 0;
    const uint8_t *data = nullptr;
  };

  void SetGuestOverlays(const GuestOverlay *overlays, uint32_t count);

  class Renderer
  {
  public:
    static constexpr uint32_t kSlots = 3;

    // Output textures: the last one shown, frames waiting to be shown, the one being rendered
    static constexpr uint32_t kOutputs = 5;
    Renderer();
    ~Renderer();

    // physical_membase: host address of guest physical memory (fetch constants hold physical addresses)
    bool Initialize(const rex::ui::d3d12::D3D12Provider &provider, ShaderCache *shaders, const uint8_t *physical_membase, uint32_t width, uint32_t height);
    void Shutdown();

    // render thread
    void Clear(const uint8_t *base, uint32_t device, uint32_t flags, uint32_t color, float z, uint32_t stencil);
    void Draw(const uint8_t *base, const DrawCall &call);
    void EndFrame(); // Swap
    void Resolve(const uint8_t *base, const ResolveCall &call);

    // Render-target trace (--sst_native_rt_trace / F10): one frame's sequence of passes, clears, resolves and other events, logged at the frame's end
    void TraceEvent(const uint8_t *base, uint32_t device, const std::string &what);
    void LogStats(uint64_t frame);
    uint64_t frames() const { return submitted_; }

    // Commands run on a separate render thread (perf log wording only)
    void SetThreaded(bool threaded) { threaded_ = threaded; }

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
      ID3D12Resource *resource = nullptr;
      uint64_t offset = 0;
    };

    // Profiling: time on the game's render thread by phase (TSC ticks, summed over the logging period), frame pacing and GPU time per frame
  public:
    enum Phase : uint32_t
    {
      kPhOther,
      kPhMemexport,
      kPhShaders,
      kPhInputs,
      kPhIndices,
      kPhPipeline,
      kPhVertices,
      kPhConstants,
      kPhTextures,
      kPhRecord,
      kPhClear,
      kPhSubmit,
      kPhFenceWait,
      kPhCount
    };

    struct Profile
    {
      uint64_t ticks[kPhCount] = {};
      uint64_t period_tsc = 0;
      std::chrono::steady_clock::time_point period_start{}, last_frame{};
      double frame_ms_sum = 0, frame_ms_max = 0, gpu_ms_sum = 0, gpu_ms_max = 0;
      uint32_t frames = 0, over_60 = 0, over_30 = 0, gpu_frames = 0, over_20 = 0;
    };

    static uint64_t Tick() { return __rdtsc(); }

    // Attributes elapsed time to one phase at a time; the rest goes to the last
    struct PhaseClock
    {
      PhaseClock(Profile &p, Phase ph) : prof(p), phase(ph), start(Tick()) {}
      ~PhaseClock() { prof.ticks[phase] += Tick() - start; }

      void Switch(Phase next)
      {
        const uint64_t now = Tick();
        prof.ticks[phase] += now - start;
        phase = next;
        start = now;
      }

      Profile &prof;
      Phase phase;
      uint64_t start;
    };

  private:
    Profile prof_;
    bool threaded_ = false;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestamps_;
    Microsoft::WRL::ComPtr<ID3D12Resource> timestamp_readback_;
    uint64_t timestamp_frequency_ = 0;
    std::array<bool, kSlots> timestamp_pending_{};
    void CreateTimestampQueries();
    void ReadGpuTime(uint32_t slot);
    void LogProfile();

    // Constant files uploaded this frame, by content (XXH3 of the guest bytes)
    std::unordered_map<uint64_t, D3D12_GPU_VIRTUAL_ADDRESS> constant_cache_;

    // Uploads the first `bytes` of a stage's constant file (the registers its shader reads; 4096 = all)
    D3D12_GPU_VIRTUAL_ADDRESS UploadConstants(const uint8_t *guest, uint32_t bytes = 4096);

    // Bytes of the constant file a shader reads: up to its highest register, or everything with relative addressing. Cached per entry
    uint32_t ConstantBytes(const ShaderEntry *entry);
    std::unordered_map<const ShaderEntry *, uint32_t> constant_bytes_;

    // Geometry cache: decoded vertex / converted index data in one video memory pool (bump allocated; when full, everything is re-stored on next use)
    struct GeoEntry
    {
      uint64_t hash = 0; // XXH3 of the guest source data last stored
      uint64_t offset = 0, generation = 0;
      uint32_t size = 0;           // bytes stored (0 = nothing yet)
      uint32_t changes = 0;        // times the source data changed
      uint32_t vmin = 0, vmax = 0; // index entries: referenced vertex range
      bool dynamic = false;        // changes too often to cache
      uint32_t stable = 0;         // consecutive checks with unchanged data
      uint64_t checked_frame = 0;  // frame of the last hash check
    };

    // An entry whose data never changed in several checks and was already checked this frame is trusted for the rest of the frame (static meshes are drawn several times a frame: shadow cascades + scene). Every entry is still checked once per frame: data rewritten in place between frames (UI, menus, loading screens) must show up on the next frame, not a few later
    bool GeoTrusted(const GeoEntry &e) const
    {
      return e.changes == 0 && e.stable >= 4 && e.size && e.generation == geo_generation_ && e.checked_frame == submitted_;
    }

    void GeoChecked(GeoEntry &e, bool unchanged)
    {
      e.stable = unchanged ? e.stable + 1 : 0;
      e.checked_frame = submitted_;
    }

    bool GeoCacheUsable();
    void GeoStore(GeoEntry &e, const Upload &src, uint32_t size);
    D3D12_GPU_VIRTUAL_ADDRESS GeoAddress(const GeoEntry &e) const;
    Microsoft::WRL::ComPtr<ID3D12Resource> geo_pool_;
    uint64_t geo_pool_size_ = 0, geo_offset_ = 0, geo_generation_ = 1;
    D3D12_RESOURCE_STATES geo_state_ = D3D12_RESOURCE_STATE_COMMON;
    bool geo_failed_ = false;
    std::unordered_map<uint64_t, GeoEntry> geo_entries_;

    bool CreateRootSignature();
    bool CreateDescriptorsAndPlaceholders();
    void CreateDebugPixelShader();
    Upload Allocate(size_t size, size_t alignment = 256);
    void BeginFrame();
    void WaitFence(uint64_t value);
    ShaderEntry *ResolveShader(const uint8_t *base, uint32_t object, bool vertex);
    ShaderEntry *ResolveShaderSlow(const uint8_t *base, uint32_t object, bool vertex, uint32_t vcopy, uint32_t vsize, uint32_t ucode_ptr);

    // Memory-export (morph) draws: run on the CPU, writing guest memory
    void RunMemexport(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry);
    void RecordMemexportRanges(const uint8_t *base, uint32_t dev);

    // Background memexport jobs not yet collected (oldest first)
    struct PendingMemexport
    {
      uint64_t ticket = 0;
      std::shared_ptr<MemexportJob> job;
      uint64_t cache_key = 0, input_hash = 0, vs_hash = 0;
      uint32_t count = 0, literals = 0, stream_count = 0;
      bool indexed = false;
      uint32_t begin[4] = {}, size[4] = {}, format[4] = {};
    };

    std::vector<PendingMemexport> pending_memexports_;
    void WaitMemexports(uint32_t begin, uint32_t end);
    void CollectMemexports(bool all);
    void FinishMemexport(const PendingMemexport &p);

    const char *LoadVsState(const uint8_t *base, uint32_t dev, uint32_t vs_obj, const ShaderEntry *vs_entry, std::vector<uint32_t> &ucode, float *constants, uint32_t bools[8], uint32_t fetch[192], uint32_t loops[32], uint32_t *literals);
    bool ComputeFetchRecords(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, uint32_t first_vertex, uint32_t vertex_count);
    std::unordered_map<uint32_t, std::vector<int32_t>> fetch_records_;
    bool EnsureCaptureResources();
    ID3D12PipelineState *GetSoPipeline(const ShaderVariant *vs, uint64_t layout_hash, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout);
    void CaptureDraw(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, const ShaderVariant *vs, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout, uint64_t layout_hash, uint32_t first_vertex, uint32_t vertex_count, uint32_t stream0, bool reads_morph, float half_x, float half_y);
    void FinishCapture();
    void CheckMorphJumps(const ShaderEntry *vs_entry, const DrawCall &call, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout, const uint8_t *host, uint32_t host_inputs, uint32_t first_vertex, uint32_t vertex_count, uint32_t stream0);

    // Screen extent (NDC) of a small draw, from running its vertex shader on the CPU (aspect ratio: tells 2D UI from 3D, and full-screen layers from elements). flat: every vertex has w = 1 (2D)
    struct DrawExtent
    {
      bool ok = false, flat = false;
      float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    };

    DrawExtent MeasureDraw(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, uint32_t first_vertex, uint32_t vertex_count);
    void CheckVsPositions(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, uint32_t first_vertex, uint32_t vertex_count, uint32_t stream0, bool reads_morph, const float *host, uint32_t host_inputs);
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t slot) const;

    // Every guest render-target surface (colour or depth) is a host texture at its logical size (max of the surface size and the viewports drawn with; the main scene's 640x736 tile surface is drawn as one 1280x736 image). Resolves copy them into host textures registered with the texture cache under the destination's guest address; the frame shown is the last full-size resolve (the game's front buffer copy)
    struct HostRT
    {
      Microsoft::WRL::ComPtr<ID3D12Resource> resource;
      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // RTV / DSV format
      uint32_t width = 0, height = 0;
      bool depth = false;
      uint32_t view = 0; // RTV or DSV index in rt_rtv_heap_ / rt_dsv_heap_
      uint32_t srv = 0;  // shader-visible SRV slot (resolve source)
      D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
      uint32_t guest_format = 0;
      float scale = 1;      // host size = width / height (guest pixels) x scale
      uint32_t samples = 1; // MSAA (3D scene targets, VIDEO > ANTI-ALIASING)
    };

    struct ResolvedTexture
    {
      Microsoft::WRL::ComPtr<ID3D12Resource> resource;
      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
      uint32_t width = 0, height = 0, slices = 1;
      bool volume = false;
      uint32_t guest_format = 0;
      std::vector<uint32_t> rtvs; // per slice
      uint32_t srv = 0;           // identity view (presenting)
      bool rb_swapped = false;    // stored red/blue swapped (ARGB-style destination)
      D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
      float scale = 1; // host size = width / height x scale
    };

    bool RenderTargetsEnabled() const;
    HostRT *GetHostRT(const uint8_t *base, uint32_t surface, bool depth, float scale, uint32_t samples, uint32_t min_width, uint32_t min_height);
    void SetRTState(HostRT &rt, D3D12_RESOURCE_STATES state);
    D3D12_CPU_DESCRIPTOR_HANDLE RtRtv(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE RtDsv(uint32_t index) const;
    uint32_t AllocateRtSrv();
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(uint32_t slot) const;
    ResolvedTexture *GetResolvedTexture(uint32_t page, uint32_t guest_format, uint32_t width, uint32_t height, uint32_t slices, bool volume, float scale);
    bool CreateBlitResources();

    // kind: 0 copy, 1 copy from MSAA (average / sample 0 for depth), 2 FXAA, 3 FPS counter text (alpha blended)
    ID3D12PipelineState *GetBlitPipeline(DXGI_FORMAT format, uint32_t kind);

    // Copies src_srv texels (src_x, src_y)+ into `rtv` at dst rect (draws a triangle; the caller handles states). Restores the draw root signature
    void Blit(uint32_t src_srv, int32_t src_x, int32_t src_y, D3D12_CPU_DESCRIPTOR_HANDLE rtv, DXGI_FORMAT format, const D3D12_RECT &dst, bool swap_rb = false, float gamma = 0, uint32_t src_samples = 1, bool fxaa = false);
    void RestoreDrawState();
    void PresentRenderTargets();
    std::unordered_map<uint32_t, HostRT> color_rts_, depth_rts_;
    std::unordered_map<uint32_t, std::unique_ptr<ResolvedTexture>> resolved_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rt_rtv_heap_, rt_dsv_heap_;
    uint32_t rt_rtv_next_ = 0, rt_dsv_next_ = 0, rt_srv_next_ = 0, dsv_size_ = 0, srv_size_ = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> blit_root_;
    std::vector<uint8_t> blit_vs_, blit_ps_, blit_ps_ms_, blit_ps_fxaa_, blit_ps_text_;

    // VIDEO > FPS COUNTER: frames shown per second (new frames handed to the display), drawn in the output's top-left corner
    std::atomic<uint64_t> shown_frames_{0};
    uint64_t fps_last_frames_ = 0, fps_last_tick_ = 0;
    uint32_t fps_tenths_ = 0;
    void DrawFpsCounter(const D3D12_RECT &output);
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> blit_pipelines_;
    uint32_t present_page_ = 0; // page of this frame's front-buffer resolve (0: none)
    HostRT *last_color_rt_ = nullptr;
    uint64_t bound_targets_ = 0; // (rtv, dsv) pair bound for draws, 0 = rebind

    // Replaced targets, released once the GPU has passed this fence value
    std::vector<std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> retired_rts_;

    struct RtTrace
    {
      bool requested = false, active = false;
      uint64_t frame = 0;
      std::vector<std::string> lines;
      uint64_t pass_key = 0;
      bool has_pass = false;
      std::string pass_head;
      uint32_t pass_draws = 0, pass_morph = 0, pass_index = 0;
      std::unordered_map<uint32_t, uint32_t> pass_prims;
      std::vector<std::string> pass_textures;
      std::unordered_set<uint64_t> pass_texture_keys, pass_ps, surfaces_logged;
    } rt_trace_;

    void TraceBeginFrame();
    void TraceState(const uint8_t *base, uint32_t dev); // opens a new pass if the state changed
    void TraceClosePass();
    void TraceDraw(const uint8_t *base, uint32_t dev, const DrawCall &call, bool morph);
    std::string DescribeSurface(const uint8_t *base, uint32_t surface);
    std::string DescribeTexture(const uint32_t fetch[6]);

    // Textures: fills the SharedConstants descriptor indices (sc[0..63]) for the samplers the shaders fetch from
    struct TextureUsage
    {
      uint32_t mask = 0;
      TextureFetchUse uses[32];
    };

    const TextureUsage &GetTextureUsage(const ShaderEntry *entry);
    void BindTextures(const uint8_t *base, uint32_t dev, const ShaderEntry *vs_entry, const ShaderEntry *ps_entry, uint32_t *sc);
    std::unique_ptr<TextureCache> textures_;
    std::unordered_map<const ShaderEntry *, TextureUsage> texture_usage_;

    ID3D12PipelineState *GetPipeline(const struct PipelineKey &key, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout, const ShaderVariant *vs, const ShaderVariant *ps);

    const rex::ui::d3d12::D3D12Provider *provider_ = nullptr;
    ID3D12Device *device_ = nullptr;
    ShaderCache *shaders_ = nullptr;
    const uint8_t *physical_membase_ = nullptr;
    uint32_t width_ = 0, height_ = 0; // guest output size (1280x720)

    // Internal resolution (latched per frame from the VIDEO page cvars): host render targets, resolve textures and outputs are their guest size x a scale - scale_ (sst_resolution / 720) for the UI / final image and the output, scene_scale_ (x sst_render_scale %) for the 3D scene's targets (the surfaces drawn with MSAA), shadow_scale_ (sst_shadow_size / 1024) for the shadow map's (resolved into stacked textures)
    float scale_ = 1, scene_scale_ = 1, shadow_scale_ = 1;
    bool shadows_off_ = false;
    bool bloom_off_ = false; // VIDEO > BLOOM OFF: the quarter-size glow / bloom targets stay black
    bool fxaa_ = false;      // VIDEO > ANTI-ALIASING FXAA: on the 3D scene's colour resolves
    uint32_t msaa_ = 1;      // VIDEO > ANTI-ALIASING MSAA: samples of the 3D scene's targets

    std::unordered_map<uint32_t, uint32_t> msaa_support_; // format << 8 | samples -> usable
    uint32_t UsableSamples(DXGI_FORMAT format, uint32_t samples);
    bool BloomSurface(const HostRT &rt, uint32_t surface) const;
    float gamma_ = 1; // present: output = colour ^ gamma_ (sst_brightness)

    std::atomic<bool> fps30_{false}; // read by Acquire (plugin thread)
    std::unordered_set<uint32_t> scene_surfaces_, shadow_surfaces_;

    // VIDEO > ASPECT RATIO. The game's camera uses the chosen aspect (see graphics_menu.cpp) and the presenter stretches the 16:9 frame to it; the UI is drawn squeezed by ui_fit_ about the frame's centre so it keeps its proportions in a 16:9 box. Draws that sample full-screen copies (the 3D scene, the glow) are composites and stay stretched
    float ui_fit_x_ = 1.0f, ui_fit_y_ = 1.0f;
    int aspect_ = 1;
    std::unordered_set<uint32_t> composite_pages_; // resolve textures holding full-screen images
    bool draw_composite_ = false;                  // this draw samples one of them
    std::array<float, kOutputs> output_scale_{};
    float depth_scale_ = 1;
    uint64_t fps30_acquires_ = 0;
    bool ApplyScale(); // (re)creates the current output / depth at scale_
    float SurfaceScale(const uint8_t *base, uint32_t dev, uint32_t color, uint32_t depth);

    // Output ring (handed to the plugin) + depth
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kOutputs> outputs_;
    std::array<uint64_t, kOutputs> output_seq_{}; // frame sequence held (0 = none)
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

    // Guest physical ranges written by memexport draws that were *not* run on the CPU this frame (--sst_native_memexport=false or failure); draws fetching vertices from them are skipped
    std::vector<std::pair<uint32_t, uint32_t>> memexport_ranges_;

    // Ranges the CPU memexport wrote this / last frame (diagnostics)
    std::vector<std::pair<uint32_t, uint32_t>> memexport_written_, memexport_written_prev_;

    // --sst_native_rt=auto: render targets seen drawing with / without a full-frame viewport (used to decide clears), and the last depth clear value (this game clears depth to 0: reversed depth)
    std::unordered_set<uint32_t> main_rts_, other_rts_;
    float last_depth_clear_ = 1.0f;

    std::unique_ptr<ShaderVariant> debug_ps_; // --sst_native_debug_shading

    // CPU memexport
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

    // --sst_native_check_vs
    struct CheckEntry
    {
      uint64_t vs = 0;
      uint32_t stream = 0, prim = 0;
      bool morph = false;
      uint64_t checked = 0, flagged = 0;
    };

    std::unordered_map<uint64_t, CheckEntry> check_entries_;

    // Morph consumer jump check
    struct MorphHistory
    {
      std::vector<float> positions;
      float extent = 0;
      uint64_t frame = 0, draw = 0;
    };

    std::unordered_map<uint64_t, MorphHistory> morph_history_;

    struct ClipHistory
    {
      std::vector<float> ndc; // x/w, y/w per vertex (NaN behind the camera)
      uint64_t frame = 0;
    };

    std::unordered_map<uint64_t, ClipHistory> clip_history_;
    std::unordered_map<uint64_t, uint32_t> clip_occurrence_; // per frame

    struct LateRange
    {
      const uint8_t *ptr;
      size_t size;
      uint64_t hash;
      bool index;
    };

    struct LateCheck
    {
      uint64_t vs = 0, draw = 0;
      uint32_t prim = 0, count = 0;
      bool morph = false;
      std::vector<LateRange> ranges;
    };

    std::vector<LateCheck> late_checks_;

    // F9 capture (GPU stream output vs CPU)
    struct CapturedDraw
    {
      uint64_t vs_hash = 0, seq = 0;
      uint32_t prim = 0, count = 0, first_vertex = 0, vertex_count = 0, stream0 = 0;
      bool indexed = false, morph = false;
      float half_x = 0, half_y = 0;
      size_t so_offset = 0;
      std::vector<float> cpu; // CPU clip positions (empty if not run)
      std::string detail;     // inputs (live / original fetch, host values)
    };

    bool capture_requested_ = false, capture_active_ = false, capture_quiet_ = false;
    uint64_t capture_mismatch_logged_ = 0, capture_frames_logged_ = 0;
    size_t so_used_ = 0;
    std::vector<CapturedDraw> captured_;
    std::string capture_detail_;
    const float *capture_host_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> so_buffer_, so_readback_, so_zero_;
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> so_pipelines_;

    struct MorphWrite
    {
      uint64_t frame = 0, draw = 0;
      bool cached = false;
      uint32_t size = 0;
      uint64_t hash = 0; // XXH3 of the output right after the morph
    };

    std::unordered_map<uint32_t, MorphWrite> morph_writes_;
    uint64_t draw_seq_ = 0;

    // Shader object address -> hash of its container copy at the last bind
    std::unordered_map<uint32_t, uint64_t> object_vhash_;
    std::unordered_map<uint32_t, uint32_t> object_ucode_; // microcode pointer at the last bind

    // Objects already validated this frame (ResolveShader fast path)
    struct ResolvedObject
    {
      uint64_t frame = UINT64_MAX;
      uint32_t ucode = 0;
      ShaderEntry *entry = nullptr;
    };

    std::unordered_map<uint32_t, ResolvedObject> resolved_objects_;
    std::vector<uint32_t> check_ucode_, check_indices_;
    std::vector<float> check_constants_, check_positions_;

    // Pipelines
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_;
    std::unordered_set<uint64_t> failed_pipelines_;
    struct PipelineWorkers;
    std::unique_ptr<PipelineWorkers> pipeline_workers_;
    std::unordered_map<uint64_t, std::shared_ptr<PipelineJob>> pending_pipelines_;
    std::unordered_map<const ShaderVariant *, uint64_t> dxil_hashes_;
    uint64_t DxilHash(const ShaderVariant *v);
    void FinishPipeline(uint64_t hash, PipelineJob &job, const struct PipelineKey &key);

    // Hand-off
    std::mutex slot_mutex_;
    int latest_ = -1;
    int pinned_ = -1;

    // Frame pacing at the hand-off (slot_mutex_): frames finished vs taken by the plugin at the guest's swaps; repeats = the same frame taken twice, skips = finished frames never taken
    uint64_t latest_seq_ = 0, acquired_seq_ = 0;
    int acquired_slot_ = -1;
    std::condition_variable frame_cv_; // a new frame finished (latest_seq_)
    uint64_t acquires_ = 0, acquire_repeats_ = 0, acquire_skips_ = 0;

    // Stats (render thread)
    struct Stats
    {
      uint64_t draws = 0, drawn = 0;
      uint64_t skip_shader = 0, skip_pending = 0, skip_decl = 0, skip_prim = 0, skip_rt = 0, skip_pipeline = 0, skip_data = 0, skip_cull = 0, skip_memexport = 0;
      uint64_t memexport_draws = 0, memexport_run = 0, memexport_cached = 0, memexport_failed = 0;
      uint64_t memexport_async = 0, memexport_waits = 0;
      double memexport_wait_ms = 0;
      double memexport_ms = 0;
      uint64_t memexport_bad_outputs = 0, memexport_bad_logged = 0;
      uint64_t morph_consumers = 0, morph_consumer_overrun = 0, morph_consumer_stale = 0, morph_consumer_bad = 0, morph_consumer_bad_logged = 0;
      uint64_t check_draws = 0, check_flagged = 0, check_logged = 0;
      uint64_t morph_jump_checks = 0, morph_jump_draws = 0, morph_jump_logged = 0;
      uint64_t morph_modified = 0, morph_modified_logged = 0;
      uint64_t check_jump_checks = 0, check_jump_draws = 0, check_jump_logged = 0;
      uint64_t late_checked = 0, late_changed = 0, late_changed_index = 0, late_logged = 0;
      uint64_t shader_rebinds = 0, shader_rebinds_logged = 0, computed_fetch_draws = 0, fetch_remapped = 0;
      uint64_t clears = 0, clear_logged = 0, sample_logged = 0, depth_only = 0, shaders_from_objects = 0, skip_shader_logged = 0;
      uint64_t upload_bytes = 0;
      uint64_t textured_draws = 0, texture_placeholders = 0, shadow_placeholders = 0;
      uint64_t constants_uploaded = 0, constants_reused = 0;
      uint64_t resolves = 0, resolves_logged = 0;
      uint64_t resolves_done = 0, resolves_skipped = 0, rects_converted = 0, host_rts = 0, presented_from_resolve = 0;
      uint64_t geo_vertex_hits = 0, geo_index_hits = 0, geo_stores = 0, geo_store_bytes = 0, geo_dynamic = 0, geo_flushes = 0, geo_trusted = 0;
      uint64_t scissored_draws = 0, scissor_logged = 0;
      uint64_t pipelines_created = 0, pipelines_failed = 0, pipelines_from_cache = 0, skip_pipeline_pending = 0;
      double pipeline_ms = 0;
      std::unordered_map<uint32_t, uint64_t> prims;          // xenos prim -> draws
      std::unordered_map<uint32_t, uint64_t> clear_flags;    // flags -> count
      std::unordered_map<uint64_t, uint64_t> render_targets; // (rt0 object<<32 | size word)
    } stats_;

    uint64_t last_stats_frame_ = 0;
    std::unordered_set<uint64_t> logged_once_;
    bool LogOnce(uint64_t key) { return logged_once_.insert(key).second; }
  };
}