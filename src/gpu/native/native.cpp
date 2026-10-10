// sst_rematch - native D3D12 renderer (exe side). See native.h.

#include "gpu/native/native.h"

#include "gpu/frame_source.h"
#include "gpu/native/shader_cache.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xmemory.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

#include <d3d12.h>
#include <wrl/client.h>

REXCVAR_DEFINE_INT32(sst_native_log_every, 600, "SST",
                     "Native renderer: print a status line every N frames (0 = never)");

namespace sst::native {

using Microsoft::WRL::ComPtr;
using rex::ui::d3d12::D3D12Presenter;
using rex::ui::d3d12::D3D12Provider;

namespace {

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

uint32_t be32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr + rex::memory::detail::PhysicalHostOffset(addr), 4);
  return __builtin_bswap32(v);
}

// --- frame output ring --------------------------------------------------------------
// The native renderer draws each frame into one of kSlots textures and hands the
// newest finished one to the plugin (gpu/frame_source.h).
class FrameOutput {
 public:
  static constexpr uint32_t kSlots = 3;

  bool Initialize(const D3D12Provider& provider, uint32_t width, uint32_t height) {
    provider_ = &provider;
    width_ = width;
    height_ = height;
    ID3D12Device* device = provider.GetDevice();

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = kSlots;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap_)))) return false;
    rtv_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = D3D12Presenter::kGuestOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (uint32_t i = 0; i < kSlots; ++i) {
      if (FAILED(device->CreateCommittedResource(
              &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
              D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&slots_[i]))))
        return false;
      device->CreateRenderTargetView(slots_[i].Get(), nullptr, Rtv(i));
      if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&allocators_[i]))))
        return false;
    }
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(),
                                         nullptr, IID_PPV_ARGS(&list_))))
      return false;
    list_->Close();
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return event_ != nullptr;
  }

  void Shutdown() {
    if (fence_) WaitFence(fence_value_);
    if (event_) CloseHandle(event_);
    event_ = nullptr;
    list_.Reset();
    for (auto& a : allocators_) a.Reset();
    for (auto& s : slots_) s.Reset();
    rtv_heap_.Reset();
    fence_.Reset();
  }

  // Game render thread: renders and publishes one frame.
  void RenderFrame(const float clear_color[4]) {
    // A slot that is neither the newest frame nor pinned by the plugin. With
    // three slots one is always free.
    uint32_t slot = 0;
    {
      std::lock_guard lock(mutex_);
      while (int(slot) == latest_ || int(slot) == pinned_) ++slot;
    }

    const uint32_t a = uint32_t(submitted_ % kSlots);
    WaitFence(allocator_fence_[a]);
    allocators_[a]->Reset();
    list_->Reset(allocators_[a].Get(), nullptr);

    ID3D12Resource* target = slots_[slot].Get();
    auto to_rt = Transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &to_rt);
    list_->ClearRenderTargetView(Rtv(slot), clear_color, 0, nullptr);
    // (increment B: the frame's draws go here)
    auto to_src = Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(1, &to_src);
    list_->Close();

    ID3D12CommandQueue* queue = provider_->GetDirectQueue();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence_.Get(), ++fence_value_);
    allocator_fence_[a] = fence_value_;
    ++submitted_;

    std::lock_guard lock(mutex_);
    latest_ = int(slot);
  }

  // Plugin side (command-processor thread).
  ID3D12Resource* Acquire(uint32_t* slot, uint32_t* width, uint32_t* height) {
    std::lock_guard lock(mutex_);
    if (latest_ < 0) return nullptr;
    pinned_ = latest_;
    *slot = uint32_t(latest_);
    *width = width_;
    *height = height_;
    return slots_[latest_].Get();
  }
  void Release(uint32_t slot) {
    std::lock_guard lock(mutex_);
    if (pinned_ == int(slot)) pinned_ = -1;
  }

  uint64_t frames() const { return submitted_; }

 private:
  D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t i) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(i) * rtv_size_;
    return h;
  }
  void WaitFence(uint64_t value) {
    if (!value || fence_->GetCompletedValue() >= value) return;
    if (SUCCEEDED(fence_->SetEventOnCompletion(value, event_)))
      WaitForSingleObject(event_, INFINITE);
  }

  const D3D12Provider* provider_ = nullptr;
  uint32_t width_ = 0, height_ = 0;
  std::array<ComPtr<ID3D12Resource>, kSlots> slots_;
  ComPtr<ID3D12DescriptorHeap> rtv_heap_;
  uint32_t rtv_size_ = 0;
  std::array<ComPtr<ID3D12CommandAllocator>, kSlots> allocators_;
  std::array<uint64_t, kSlots> allocator_fence_{};
  ComPtr<ID3D12GraphicsCommandList> list_;
  ComPtr<ID3D12Fence> fence_;
  HANDLE event_ = nullptr;
  uint64_t fence_value_ = 0;
  uint64_t submitted_ = 0;

  std::mutex mutex_;  // latest_ / pinned_
  int latest_ = -1;
  int pinned_ = -1;
};

// --- renderer state -------------------------------------------------------------------

struct State {
  std::mutex mutex;  // serializes hook entry points against Shutdown
  const D3D12Provider* provider = nullptr;
  SstSetFrameSourceFn set_frame_source = nullptr;
  FrameOutput output;
  ShaderCache shaders;

  // Per frame (render thread).
  bool have_clear = false;
  float clear_color[4] = {};
  uint32_t clears = 0;
  uint32_t first_clear_flags = 0;
};

State* g_state = nullptr;
std::atomic<bool> g_active{false};

ID3D12Resource* AcquireThunk(void* user, uint32_t* slot, uint32_t* width, uint32_t* height) {
  return static_cast<FrameOutput*>(user)->Acquire(slot, width, height);
}
void ReleaseThunk(void* user, uint32_t slot) { static_cast<FrameOutput*>(user)->Release(slot); }

HMODULE FindPlugin() {
  // rex::system::LoadGpuPlugin's per-config names.
  for (const wchar_t* name : {L"rexgpu-sst.dll", L"rexgpu-sstd.dll", L"rexgpu-sstrd.dll"})
    if (HMODULE h = GetModuleHandleW(name)) return h;
  return nullptr;
}

}  // namespace

// --- lifetime -------------------------------------------------------------------------

void Initialize(rex::Runtime* runtime) {
  HMODULE plugin = FindPlugin();
  if (!plugin) return;  // another GPU plugin (xenos): stay inactive

  auto set_source = reinterpret_cast<SstSetFrameSourceFn>(
      reinterpret_cast<void*>(GetProcAddress(plugin, SST_SET_FRAME_SOURCE_EXPORT)));
  auto* gs = runtime ? runtime->graphics_system() : nullptr;
  auto* provider = gs ? static_cast<const D3D12Provider*>(gs->provider()) : nullptr;
  if (!set_source || !provider || !provider->GetDevice()) {
    REXLOG_ERROR("[sst-native] sst GPU plugin is loaded but {} - native renderer disabled",
                 !set_source ? "has no frame-source export (old build?)" : "has no D3D12 device");
    return;
  }

  auto state = std::make_unique<State>();
  state->provider = provider;
  state->set_frame_source = set_source;
  if (!state->output.Initialize(*provider, 1280, 720)) {
    REXLOG_ERROR("[sst-native] failed to create the frame output - native renderer disabled");
    return;
  }
  state->shaders.Initialize(*provider,
                            rex::filesystem::GetExecutableFolder() / "cache" / "shaders");

  const SstFrameSource source = {sizeof(SstFrameSource), &state->output, AcquireThunk,
                                 ReleaseThunk};
  set_source(&source);

  g_state = state.release();
  g_active.store(true, std::memory_order_release);
  REXLOG_INFO("[sst-native] native renderer active (1280x720)");
}

void Shutdown() {
  if (!g_active.exchange(false)) return;
  State* s = g_state;
  std::lock_guard lock(s->mutex);   // no hook is inside the renderer now
  s->set_frame_source(nullptr);     // waits for a copy in progress in the plugin
  s->shaders.Shutdown();
  s->output.Shutdown();
  // g_state is left allocated: a late hook call only sees g_active == false.
}

bool Active() { return g_active.load(std::memory_order_acquire); }

// --- hooks ----------------------------------------------------------------------------

void OnCreateShader(const uint8_t* base, uint32_t container, uint32_t object) {
  if (!Active()) return;
  const uint32_t flags = be32(base, container);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) return;
  const uint32_t size = be32(base, container + 4) + be32(base, container + 8);
  if (size == 0 || size > (1u << 20)) return;

  std::lock_guard lock(g_state->mutex);
  if (!Active()) return;
  const uint8_t* host = base + container + rex::memory::detail::PhysicalHostOffset(container);
  ShaderEntry* entry = g_state->shaders.Register(host, size);
  g_state->shaders.BindObject(object, entry);
}

void OnClear(uint32_t flags, uint32_t color, float /*z*/, uint32_t /*stencil*/) {
  if (!Active()) return;
  std::lock_guard lock(g_state->mutex);
  State& s = *g_state;
  if (s.clears++ == 0) s.first_clear_flags = flags;
  // Xbox D3DCLEAR_TARGET0..3 are the low four bits.
  if ((flags & 0xF) && !s.have_clear) {
    s.have_clear = true;
    s.clear_color[0] = float((color >> 16) & 0xFF) / 255.0f;
    s.clear_color[1] = float((color >> 8) & 0xFF) / 255.0f;
    s.clear_color[2] = float(color & 0xFF) / 255.0f;
    s.clear_color[3] = 1.0f;
  }
}

void OnSwap() {
  if (!Active()) return;
  std::lock_guard lock(g_state->mutex);
  if (!Active()) return;
  State& s = *g_state;

  // Increment A: the frame is a clear. The guest's own clear colour when it
  // cleared a colour target this frame, else teal (distinct from the plugin's
  // blue placeholder, so the hand-off is visible).
  static constexpr float kNoClear[4] = {0.0f, 0.35f, 0.30f, 1.0f};
  s.output.RenderFrame(s.have_clear ? s.clear_color : kNoClear);

  const uint64_t frame = s.output.frames();
  const int every = REXCVAR_GET(sst_native_log_every);
  if (frame == 1 || (every > 0 && frame % uint64_t(every) == 0)) {
    REXLOG_INFO("[sst-native] frame {}: {} guest clears (first flags {:X}), colour {}", frame,
                s.clears, s.first_clear_flags,
                s.have_clear ? fmt::format("{:.2f},{:.2f},{:.2f}", s.clear_color[0],
                                           s.clear_color[1], s.clear_color[2])
                             : std::string("none (teal)"));
    s.shaders.LogStats("so far");
  }
  s.have_clear = false;
  s.clears = 0;
  s.first_clear_flags = 0;
}

}  // namespace sst::native
