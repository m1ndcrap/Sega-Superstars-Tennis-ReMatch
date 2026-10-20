// The VS screen spread at ultrawide. The loadingscreen script places the characters at locators of the 3D scene; we grab the locator, and the model, locator pair object copies the locator's 4x4 world matrix to the model. Each character placed at one of the four slot locators is moved along the slots' line so its distance from the line's middle grows by the screen's widening (aspect / (16/9)).

#include "menu/vs_screen.h"

#include "menu/hud_anchor.h"
#include "menu/menu_internal.h"
#include "menu/video_settings.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <mutex>
#include <array>
#include <set>
#include <vector>

REXCVAR_DECLARE(bool, sst_vs_spread);
REXCVAR_DECLARE(bool, sst_hud_anchor);

namespace sst::menu::vs
{
  namespace
  {
    constexpr uint32_t kLoadingScreenScript = 0xD052E6D9; // lookup2("loadingscreen")
    constexpr uint32_t kCharSelectScript = 0x4BC618A9;    // lookup2("charselect"): the same PlayerSelect scene and slot locators
    constexpr uint32_t kGetLocator = 0x0E01, kPlaceAtLocator = 0x0112, kSetPixelPosition = 0x041F;
    constexpr uint32_t kNodeMatrix = 0x20, kModelMatrix = 0xB0;

    // 4:20 (sub_82283498): the screen width the scripts lay pixel positions out in, an int at 0x825F0540 (not always 1280: the rings' centre is half of it)
    constexpr uint32_t kScriptWidthAt = 0x825F0540;
    constexpr float kLayoutWidth = 1280.0f;

    // A loading ring's centre from the x 4:31 gives it (MsgBox.dat's ring group: its picture group sits at -0.45 scaled 0.75), in fractions of the width: the centre spreads, so the ring stays on its character
    constexpr float kRingCentre = 0.04f;

    // Script value types of the scene's nodes: registered types (16 and up) kept by the natives' result wrapper (sub_8220F888); a locator's is at 0x82646910
    constexpr uint32_t kLocatorTypeAt = 0x82646910, kFirstUserType = 0x10;

    // The four character slots' locator names (14:1's third argument, the same in singles, doubles and tournaments; the script's other locators are lights)
    constexpr uint32_t kSlotLocators[] = {0xEDAE6A9D, 0x3A81C37F, 0xE0A83558, 0xAB7D76F8};

    std::atomic<uint32_t> g_module{0};    // the loadingscreen module (0: not seen yet)
    std::atomic<uint32_t> g_cs_module{0}; // the charselect module
    std::mutex g_mutex;
    std::set<uint32_t> g_locators; // its 14:1 results
    bool g_logged = false;
    std::atomic<bool> g_ring_logged{false};

    float LoadF(uint8_t *base, uint32_t a) { return std::bit_cast<float>(Load32(base, a)); }
    void StoreF(uint8_t *base, uint32_t a, float f) { Store32(base, a, std::bit_cast<uint32_t>(f)); }

    bool Plausible(uint32_t p) { return p >= 0x40000000 && p < 0xFFFFF000; }

    // The spread factor (aspect / (16/9)); 1 when off
    float Factor()
    {
      if (!REXCVAR_GET(sst_vs_spread) || !REXCVAR_GET(sst_hud_anchor) || video::Boot().four_three)
        return 1.0f;

      return 1.0f + 2.0f * hud::ShiftFor(CameraAspect());
    }

    // 1: the loadingscreen script, 2: charselect, 0: neither
    int FromScript(PPCContext &ctx, uint8_t *base)
    {
      if (!ctx.r4.u32)
        return 0;

      const uint32_t module = Load32(base, ctx.r4.u32 + 8), vs = g_module.load(), cs = g_cs_module.load();
      return vs && module == vs ? 1 : cs && module == cs ? 2
                                                         : 0;
    }

    // A node's world matrix (row-major, translation in row 3), or 0
    uint32_t NodeMatrix(uint8_t *base, uint32_t node)
    {
      if (!Plausible(node))
        return 0;

      const uint32_t m = Load32(base, node + kNodeMatrix);
      return Plausible(m) ? m : 0;
    }

    void Spread(uint8_t *base, uint32_t model)
    {
      const float s = Factor();

      if (s <= 1.001f || !Plausible(model))
        return;

      const uint32_t m = model + kModelMatrix;

      if (std::fabs(LoadF(base, m + 0x3C) - 1.0f) > 1e-3f) // (not row-major with the translation in row 3)
        return;

      float p[3] = {LoadF(base, m + 0x30), LoadF(base, m + 0x34), LoadF(base, m + 0x38)};
      std::lock_guard lock(g_mutex);

      // The line of the characters: from the outermost locators (their matrices are the ones 1:18 copies, so safe to read), as the camera's right axis; its middle as the view axis. (The camera node's layout is not known)
      std::vector<std::array<float, 3>> spots;

      for (uint32_t loc : g_locators)
      {
        if (const uint32_t lm = NodeMatrix(base, loc))
          spots.push_back({LoadF(base, lm + 0x30), LoadF(base, lm + 0x34), LoadF(base, lm + 0x38)});
      }

      if (spots.size() < 2)
        return;

      // The pair furthest apart
      size_t a = 0, b = 1;
      float best = -1.0f;

      for (size_t i = 0; i < spots.size(); ++i)
      {
        for (size_t j = i + 1; j < spots.size(); ++j)
        {
          const float dx = spots[j][0] - spots[i][0], dz = spots[j][2] - spots[i][2], d = dx * dx + dz * dz;

          if (d > best)
            best = d, a = i, b = j;
        }
      }

      if (best < 1e-6f)
        return;

      const float len = std::sqrt(best);
      float right[3] = {(spots[b][0] - spots[a][0]) / len, 0.0f, (spots[b][2] - spots[a][2]) / len};
      const float eye[3] = {0.5f * (spots[a][0] + spots[b][0]), 0.0f, 0.5f * (spots[a][2] + spots[b][2])};

      const float lateral = (p[0] - eye[0]) * right[0] + (p[1] - eye[1]) * right[1] + (p[2] - eye[2]) * right[2];

      for (int i = 0; i < 3; ++i)
        StoreF(base, m + 0x30 + 4 * i, p[i] + right[i] * lateral * (s - 1.0f));

      if (!g_logged)
      {
        g_logged = true;
        REXLOG_INFO("[sst-menu] VS / character select screen: characters spread x{:.3f} along the locators' line (direction {:.2f} {:.2f}, middle {:.1f} {:.1f}, {} locators)", s, right[0], right[2], eye[0], eye[2], spots.size());
      }
    }
  }

  void OnScriptStart(uint8_t *base, uint32_t front_end, uint32_t script)
  {
    if ((script != kLoadingScreenScript && script != kCharSelectScript) || !front_end)
      return;

    if (const uint32_t node = FindScriptNode(base, front_end, script))
      (script == kLoadingScreenScript ? g_module : g_cs_module) = Load32(base, node + kNodeModule);

    std::lock_guard lock(g_mutex);
    g_locators.clear();
    g_logged = false;
    g_ring_logged = false;
  }

  Pending BeforeNative(PPCContext &ctx, uint8_t *base, uint32_t id)
  {
    Pending p;

    if (id != kGetLocator && id != kPlaceAtLocator && id != kSetPixelPosition)
      return p;

    const int from = FromScript(ctx, base);

    if (!from || Factor() <= 1.001f)
      return p;

    const uint32_t values = ctx.r8.u32, types = ctx.r9.u32, count = ctx.r10.u32;
    p.id = id;
    p.out = ctx.r3.u32;

    // Loading rings: 4:31(object, x, y) in layout pixels
    if (id == kSetPixelPosition)
    {
      // (Character select's rings are MsgBox.dat objects placed the same way, one per player's place; its other 4:31 places a centred sprite, which stays)
      if (count >= 2 && values && types && *GuestPtr(base, types + 1) == kFloat)
      {
        const uint32_t width = Load32(base, kScriptWidthAt);
        const float w = width >= 320 && width <= 16384 ? float(width) : kLayoutWidth;
        const float x = LoadF(base, values + 4), c = x + kRingCentre * w, half = 0.5f * w;
        const float moved = half + (c - half) * Factor() - kRingCentre * w;
        StoreF(base, values + 4, moved);

        if (!g_ring_logged.exchange(true))
          REXLOG_INFO("[sst-menu] {}: loading ring x {:.1f} -> {:.1f} (script width {})", from == 1 ? "VS screen" : "character select", x, moved, width);
      }

      return p;
    }

    if (id == kGetLocator)
    {
      const uint32_t name = count >= 3 && values ? Load32(base, values + 8) : 0;

      if (std::find(std::begin(kSlotLocators), std::end(kSlotLocators), name) == std::end(kSlotLocators))
        return p;
    }

    if (id == kPlaceAtLocator)
    {
      if (count < 2 || !values || !types || *GuestPtr(base, types) < kFirstUserType || *GuestPtr(base, types + 1) != (Load32(base, kLocatorTypeAt) & 0xFF))
        return p;

      p.model = Load32(base, values);
      p.locator = Load32(base, values + 4);
    }

    p.active = true;
    return p;
  }

  void AfterNative(uint8_t *base, const Pending &p)
  {
    if (!p.active)
      return;

    if (p.id == kGetLocator)
    {
      // The result {value, type}: a locator node
      if (!p.out || Load32(base, p.out + 4) != Load32(base, kLocatorTypeAt))
        return;

      const uint32_t node = Load32(base, p.out);

      if (!Plausible(node))
        return;

      std::lock_guard lock(g_mutex);
      g_locators.insert(node);
      return;
    }

    if (p.id == kPlaceAtLocator)
    {
      bool ours;

      {
        std::lock_guard lock(g_mutex);
        ours = g_locators.count(p.locator) != 0;
      }

      if (ours)
        Spread(base, p.model);
    }
  }
}