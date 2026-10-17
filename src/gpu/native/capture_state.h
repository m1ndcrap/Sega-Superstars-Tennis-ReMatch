// The game thread captures each D3D call it hooks (the call's arguments plus copies of the guest memory the call reads that may change before it is drawn) into Commands; the render thread runs them on the Renderer a frame behind. Private to native*.cpp, capture*.cpp and render_thread.cpp.

#pragma once

#include "gpu/frame_source.h"
#include "gpu/native/renderer.h"
#include "gpu/native/shader_cache.h"
#include "gpu/native/guest_watch.h"
#include "gpu/native/texture_cache.h"

#include <rex/system/xmemory.h>

#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

namespace sst::native::capture
{
  // Host address of a guest virtual address
  inline const uint8_t *HostPtr(const uint8_t *base, uint32_t addr) { return base + addr + rex::memory::detail::PhysicalHostOffset(addr); }

  // Big-endian dword at a guest address
  inline uint32_t Be32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, HostPtr(base, addr), 4);
    return __builtin_bswap32(v);
  }

  // A physical address as the CPU sees it (the 0xA0000000 view); virtual addresses unchanged
  inline uint32_t ToVirtual(uint32_t addr) { return addr < 0x20000000u ? (addr | 0xA0000000u) : addr; }

  // Looks like a guest heap / physical-view pointer (not null, not a small integer)
  inline bool Plausible(uint32_t addr) { return addr >= 0x40000000u; }

  // Cheap timestamps for the capture timing (a dozen per draw call): the CPU's time-stamp counter, converted to ms when the perf lines are logged (calibrated against steady_clock over the period). QueryPerformanceCounter was a visible share of the capture
  inline uint64_t Ticks()
  {
#if defined(_M_X64) || defined(__x86_64__)
    return __rdtsc();
#else
    return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
  }

  // The guest D3D device's state block: registers, shader constants, fetch constants and bound objects, at fixed offsets from the device object (see renderer_internal.h). [kDevSnapBegin, kDevSnapEnd) is what a draw reads. It is sent as a delta: the 256-byte chunks that changed since the previous call (a draw typically changes a few constants and pointers, not all 11.5 KB); the render thread keeps its own copy up to date
  constexpr uint32_t kDevSnapBegin = 0x480, kDevSnapEnd = 0x31A0;
  constexpr uint32_t kDevSnapSize = kDevSnapEnd - kDevSnapBegin;
  constexpr uint32_t kDevFetchConstants = 0x480; // 32 texture fetch constants of 6 dwords = 96 vertex fetch constants of 2
  constexpr uint32_t kDevVSConstants = 0x780;    // 256 float4 vertex shader constants
  constexpr uint32_t kDevPSConstants = 0x1780;   // 256 float4 pixel shader constants
  constexpr uint32_t kDevDepthControl = 0x2934;  // RB_DEPTHCONTROL in the register mirror
  constexpr uint32_t kDevIndices = 0x3094;       // bound index buffer object
  constexpr uint32_t kDevPixelShader = 0x3194;   // bound pixel shader object (+0x18: its microcode pointer)
  constexpr uint32_t kDevVertexShader = 0x3198;  // bound vertex shader object (+0x20: its microcode pointer)
  constexpr uint32_t kDevChunk = 256;
  constexpr uint32_t kDevChunks = (kDevSnapSize + kDevChunk - 1) / kDevChunk; // 46
  static_assert(kDevChunks <= 64 && kDevChunk % 32 == 0 && kDevSnapSize % 32 == 0);
  constexpr uint64_t kAllDevChunks = kDevChunks == 64 ? ~uint64_t(0) : (uint64_t(1) << kDevChunks) - 1;

  constexpr uint32_t ChunkSize(uint32_t i) { return std::min(kDevChunk, kDevSnapSize - i * kDevChunk); }

  // Bump allocator for one frame's copies (its chunks are kept and reused frame after frame)
  class Arena
  {
  public:
    uint8_t *Alloc(size_t size);
    void Reset() { current_ = used_ = bytes_ = 0; }
    size_t bytes() const { return bytes_; }

  private:
    struct Chunk
    {
      std::unique_ptr<uint8_t[]> data;
      size_t size = 0;
    };

    std::vector<Chunk> chunks_;
    size_t current_ = 0, used_ = 0, bytes_ = 0;
  };

  // A texture a draw samples: a copy of its data, or (data == nullptr) unchanged since its last copy, with that copy's content hash
  struct TextureSnap
  {
    uint32_t begin, end; // physical
    const uint8_t *data;
    uint64_t hash = 0; // (TextureCache::HashGuest)
  };

  // One hooked D3D call, with the copies it carries (all in the frame's area)
  struct Command
  {
    enum Kind : uint8_t
    {
      kDraw,
      kClear,
      kResolve,
      kEvent,  // render-target trace text
      kShader, // a shader container to register
      kSwap
    };

    Kind kind = kDraw;

    // Small guest ranges the call reads (pointers, rects, microcode), seen by the renderer instead of live memory while it runs
    uint8_t overlay_count = 0;
    GuestOverlay overlays[8];

    DrawCall draw;
    ResolveCall resolve;
    uint32_t device = 0, flags = 0, color = 0, stencil = 0; // clear (device: also event)
    float z = 0;
    const uint8_t *data = nullptr; // event text / shader container
    uint32_t size = 0, object = 0;

    // Textures and small vertex buffers the game may rewrite before the render thread gets to the draw
    const TextureSnap *tex = nullptr;
    uint32_t tex_count = 0;

    const Renderer::GuestCopy *verts = nullptr;
    uint32_t vert_count = 0;

    // Device block delta: chunks set in dev_mask, packed in dev_delta
    uint32_t dev_addr = 0;
    uint64_t dev_mask = 0;
    const uint8_t *dev_delta = nullptr;
  };

  constexpr size_t kBatchCommands = 128;

  struct Batch
  {
    std::vector<Command> commands;
    Arena *arena = nullptr; // set on the batch that ends a frame: recycled once the render thread has run it
    uint64_t frame = 0;
  };

  // Game thread to render thread: batches of commands, and the arenas frames are captured into (one frame of lag at most)
  struct CommandQueue
  {
    std::mutex mutex;
    std::condition_variable work_cv, done_cv;
    std::deque<Batch> batches;
    std::vector<std::unique_ptr<Arena>> arenas;
    std::vector<Arena *> free_arenas;
    uint64_t frames_done = 0;
    bool stop = false;
    std::thread thread;
  };

  // The device block as last sent, and which constant chunks each bound shader reads (published by the renderer: guest_watch::ConstantUse)
  struct DeviceCapture
  {
    uint32_t snap_addr = 0;
    std::unique_ptr<uint8_t[]> snap{new uint8_t[kDevSnapSize]};
    uint64_t constant_use_version = UINT64_MAX;
    std::unordered_map<uint64_t, uint32_t> constant_use; // ShaderKey -> constant chunk mask

    // The compare mask for the last (VS key, PS key, constant-use version): consecutive draws mostly keep their shaders
    uint64_t last_vs = ~0ull, last_ps = ~0ull, last_version = ~0ull, last_mask = 0;

    uint64_t chunks_copied = 0, chunks_total = 0, chunks_skipped = 0; // this logging period
  };

  // The bound vertex shader's live microcode (D3D patches it per vertex declaration): this frame's copies
  struct MicrocodeCapture
  {
    struct Copy
    {
      uint32_t size;
      const uint8_t *data;
      uint32_t stamp = 0; // write_watch::Stamp at the copy / last compare
      bool armed = false;
    };

    std::unordered_map<uint32_t, Copy> copies; // live address -> copy

    // Microcode pages written while the microcode stays the same (busy data on the same pages): over 32 in a frame turns the microcode watch off for the session (each such write is a page fault)
    uint64_t false_frame = 0;
    uint32_t false_alarms = 0;

    bool watch_off = false;
    uint64_t reused_by_watch = 0, skipped = 0; // this logging period
  };

  // The renderer's list of textures and vertex buffers the game rewrites (guest_watch), as last copied
  struct WatchedData
  {
    uint64_t version = UINT64_MAX;
    guest_watch::Set set;
    std::vector<std::pair<uint32_t, uint32_t>> vertices_sorted; // (phys, bytes): a cheap per-fetch-slot test
  };

  struct TextureCapture
  {
    std::unordered_set<uint32_t> copied_this_frame; // base pages
    uint64_t frame_bytes = 0;

    // UI textures not seen changing: content hash of the last copy, so an unchanged one isn't copied again (hashing reads it once; a copy reads it, writes it, and the renderer hashes the copy). The draw carries the hash instead, and the renderer keeps the texture it uploaded from that copy without reading guest memory (the game may already be rewriting it for a later frame)
    struct Check
    {
      uint64_t hash = 0;
      bool known = false;
    };

    std::unordered_map<uint32_t, Check> checks; // page -> content hash at its last copy
    XXH3_state_t *hash_state = nullptr;

    // This logging period
    struct Stat
    {
      uint64_t copies = 0, bytes = 0;
      bool changing = false;
    };

    std::unordered_map<uint32_t, Stat> stats; // by base page
    uint64_t bytes = 0, checks_hashed = 0, checks_unchanged = 0;
  };

  struct VertexCapture
  {
    // Buffers whose watched pages keep getting written while the buffer's own bytes don't change (other data shares the page, or the buffer is streamed): each such write is a page fault, so they go back to per-draw compares for a while
    struct WatchHealth
    {
      uint64_t frame = 0;       // frame of `writes`
      uint32_t writes = 0;      // re-checks this frame (the watch saw a write: real change or not)
      uint64_t quiet_until = 0; // not watched before this frame
    };

    // This frame's last copy of a buffer (reused while the guest data is unchanged: some small buffers are read by hundreds of draws)
    struct LastCopy
    {
      const uint8_t *data;
      uint32_t size;
      uint32_t stamp = 0;            // write_watch::Stamp of [phys, phys + size) when copied / last compared
      bool armed = false;            // stamp is valid (watched since)
      WatchHealth *health = nullptr; // its watch_health entry (looked up once per frame)
    };

    std::unordered_map<uint32_t, LastCopy> copies_this_frame;
    std::unordered_map<uint32_t, WatchHealth> watch_health;
    uint32_t noisy = 0; // buffers put back to comparing (session)

    // This logging period
    struct Stat
    {
      uint64_t draws = 0, bytes = 0, ui_draws = 0;
      uint32_t watched = 0, largest = 0;
    };

    std::unordered_map<uint32_t, Stat> stats; // by buffer
    uint64_t bytes = 0, reused = 0, reused_by_watch = 0, watch_arms = 0, watch_false = 0;
  };

  // The game thread's own cost and pacing (perf log lines, every sst_native_log_every frames)
  struct CapturePerf
  {
    enum Part
    {
      kDevice,
      kMicrocode,
      kIndexScan,
      kTextures,
      kVertices,
      kParts
    };

    uint64_t capture_ticks = 0, part_ticks[kParts] = {};
    uint64_t frame_capture_ticks = 0, capture_max_ticks = 0; // this frame's capture / worst per period
    double wait_ms = 0;                                      // waiting for the render thread
    uint64_t calls = 0, snapshot_bytes = 0, frames = 0;

    // Ticks -> ms over the logging period
    uint64_t period_ticks0 = 0;
    std::chrono::steady_clock::time_point period_t0{};

    // The game's frame pacing (Swap to Swap)
    std::chrono::steady_clock::time_point last_swap{};
    double swap_max_ms = 0;
    uint64_t swaps_over_20 = 0;
  };

  struct State
  {
    // The renderer (render thread; game thread when not threaded). `mutex` serializes its use against Shutdown
    std::mutex mutex;
    SstSetFrameSourceFn set_frame_source = nullptr;
    ShaderCache shaders;
    Renderer renderer;
    const uint8_t *base = nullptr; // guest memory
    bool threaded = false;

    // The render thread's copy of the device block, as of the command being run
    uint32_t rt_dev_addr = 0;
    std::unique_ptr<uint8_t[]> rt_dev{new uint8_t[kDevSnapSize]};

    CommandQueue queue;

    // Capture (game threads, under capture_mutex: D3D calls come from one thread, shader creation may come from others)
    std::mutex capture_mutex;
    Batch batch;            // being filled
    Arena *arena = nullptr; // this frame's copies
    uint64_t frame = 0;
    DeviceCapture device;
    MicrocodeCapture microcode;
    WatchedData watched;
    TextureCapture textures;
    VertexCapture vertices;
    CapturePerf perf;
    bool game_thread_boosted = false;
  };

  inline State *g_state = nullptr;
  inline std::atomic<bool> g_active{false};

  // render_thread.cpp
  void Execute(State &s, const Command &c); // runs one command (render thread, or the game thread when not threaded)
  void StartRenderThread(State &s);
  void StopRenderThread(State &s);

  // capture_perf.cpp
  void NoteSwap(State &s);       // the game's frame pacing
  void LogCapturePerf(State &s); // every sst_native_log_every frames
}