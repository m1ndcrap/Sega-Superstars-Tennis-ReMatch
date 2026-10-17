// The cameras build their projections with a vertical field of view and an aspect of 16/9. The port hooks the three perspective builders and replaces a 16:9 aspect with the chosen one: a wider value widens the view (Hor+). The renderer squeezes the UI back into a 16:9 box and the presenter shows
// the frame at the chosen aspect. The cameras also keep the aspect they were set up with (from the 16/9 constant at kCameraAspectConstant) and use it for more than the projection, e.g. the area the shadow maps cover, so that constant is changed too.

#include "menu/menu_internal.h"
#include "menu/video_settings.h"

#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <cmath>
#include <set>

#include <fmt/format.h>

namespace sst::menu
{
  namespace
  {
    constexpr float k16By9 = 16.0f / 9.0f;
    constexpr float kAspectValues[5] = {4.0f / 3.0f, k16By9, 1.6f, 21.0f / 9.0f, 32.0f / 9.0f};
    constexpr const char *kAspectNames[5] = {"4:3", "16:9", "16:10", "21:9", "32:9"};

    // The 16/9 the cameras are set up from, in the xex's read-only data
    constexpr uint32_t kCameraAspectConstant = 0x82108440;

    std::atomic<float> g_camera_aspect{k16By9};
    std::mutex g_mutex;

    // Writes `value` over the constant (its page is opened for writing briefly)
    void WriteAspectConstant(uint8_t *base, float value)
    {
      const uint32_t bits = std::bit_cast<uint32_t>(value);

      if (Load32(base, kCameraAspectConstant) == bits)
        return;

      auto *memory = rex::system::kernel_memory();
      auto *heap = memory ? memory->LookupHeap(kCameraAspectConstant) : nullptr;
      const uint32_t page = kCameraAspectConstant & ~0xFFFu;
      uint32_t old_protect = 0;

      if (!heap || !heap->Protect(page, 0x1000, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, &old_protect))
      {
        static bool warned = false;

        if (!warned)
          REXLOG_WARN("[sst-menu] camera aspect constant: could not unprotect {:08X}", page);

        warned = true;
        return;
      }

      Store32(base, kCameraAspectConstant, bits);
      heap->Protect(page, 0x1000, old_protect);
    }

    // Each (builder, caller, aspect, far plane) a perspective builder sees is logged once: cameras that don't follow the wide aspect are found this way. (A shorter far plane, 50% / 25%, changed nothing: logs 034-036, so there is no draw distance option)
    void LogBuilder(uint32_t builder, uint32_t caller, double aspect, double replaced, double fov, double near_plane, double far_plane)
    {
      static std::mutex mutex;
      static std::set<uint64_t> seen;
      const uint64_t key = (uint64_t(caller) << 32) ^ uint64_t(builder) ^ uint64_t(std::llround(aspect * 10000.0)) ^ (uint64_t(std::llround(std::fabs(far_plane))) << 20);
      std::lock_guard lock(mutex);

      if (seen.size() > 512 || !seen.insert(key).second)
        return;

      REXLOG_INFO("[sst-menu] camera: builder {:08X} from {:08X} aspect {:.4f}{} (f1 {:.4f}, f3 {:.3f}, f4 {:.1f})", builder, caller, aspect, replaced != aspect ? fmt::format(" -> {:.4f}", replaced) : std::string(), fov, near_plane, far_plane);
    }

    // A perspective builder's arguments: f1 vertical FOV, f2 aspect, f3 near, f4 far. 16:9 becomes the chosen aspect (other values, e.g. split views, are left alone)
    void FixCameraAspect(PPCContext &ctx, uint32_t builder)
    {
      const double aspect = ctx.f2.f64;

      if (std::fabs(aspect - 16.0 / 9.0) < 0.002)
        ctx.f2.f64 = double(g_camera_aspect.load());

      LogBuilder(builder, uint32_t(ctx.lr), aspect, ctx.f2.f64, ctx.f1.f64, ctx.f3.f64, ctx.f4.f64);
    }
  }

  float CameraAspect() { return g_camera_aspect.load(); }

  void ApplyCameraAspect(uint8_t *base)
  {
    std::lock_guard lock(g_mutex);

    // The aspect ratio the game started with (a new one waits for the restart). 4:3 runs as the game's own 4:3 mode (cameras already 4:3)
    const int v = std::clamp(video::Boot().aspect, 0, 4);
    const float want = v == 0 ? k16By9 : kAspectValues[v];

    if (g_camera_aspect.exchange(want) != want)
      REXLOG_INFO("[sst-menu] camera aspect {:.4f} ({}{})", want, kAspectNames[v], v == 0 ? ", game's 4:3 mode" : "");

    if (base)
      WriteAspectConstant(base, want);
  }
}

// Perspective matrix builders
extern "C" void __imp__sub_8231F950(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_822C2480(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_821227C0(PPCContext &__restrict, uint8_t *);

extern "C" REX_FUNC(sub_8231F950)
{
  sst::menu::FixCameraAspect(ctx, 0x8231F950);
  __imp__sub_8231F950(ctx, base);
}

extern "C" REX_FUNC(sub_822C2480)
{
  sst::menu::FixCameraAspect(ctx, 0x822C2480);
  __imp__sub_822C2480(ctx, base);
}

extern "C" REX_FUNC(sub_821227C0)
{
  sst::menu::FixCameraAspect(ctx, 0x821227C0);
  __imp__sub_821227C0(ctx, base);
}