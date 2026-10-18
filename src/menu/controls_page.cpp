// Controller page stuff. Script calls:
//   - the DualShock 3 diagram while a PlayStation pad is in use. The scene holds two diagrams: the Xbox 360 pad (group 50034B87) and the DualShock 3 (3A12D8E8). The script shows the first and hides the second when the page opens (4:5 on 3A12D8E8), then animates the shown one (4:10, layout types
//     A-D). With a PlayStation pad the two are swapped in every scene native the page calls.
//   - KEY BINDINGS on Y while the keyboard is in use (a fourth navbar button; Y leaves the page like Confirm, and its exit opens the KEY BINDINGS page instead of the Settings list). The KEY BINDINGS page gets a Reset to Defaults button on Y the same way.
//   - VIBRATION greyed out while the keyboard is in use, here and on the pause menu.
//

#include "input/controls.h"
#include "menu/port_text.h"
#include "menu/menu_internal.h"

#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <utility>

extern "C" void __imp__sub_821DFBC0(PPCContext &__restrict, uint8_t *);

namespace sst::menu::controls
{
  namespace
  {
    constexpr uint32_t kXboxPadGroup = 0x50034B87, kPs3PadGroup = 0x3A12D8E8;
    constexpr uint32_t kControllerLayer = 0xABB4502D; // lookup2("SETTINGS_CONTROLLER"): the page's layer, closed (4:3) by the script's Exit
    constexpr uint32_t kNavbarButtons = 0x69F54F57;   // the navbar native's first argument (its button list follows)
    constexpr uint32_t kInputEvents = 0x7074624C;     // the input read native's argument (once per page frame)
    constexpr uint32_t kEventSelect = 0x2FAB294A;
    constexpr uint32_t kSceneObjects = 0x82DB4370; // the scene manager (sub_82582110: object by hash)

    // Native ids (group << 8 | index) the page calls
    constexpr uint32_t kNativeNavbar = 0x205, kNativeInput = 0x306, kNativePauseInput = 0x312;
    constexpr uint32_t kSceneGroup = 4, kSceneLeavePage = 3, kSceneHide = 5, kSceneAnimate = 10;

    struct State
    {
      bool ps = false;        // the DualShock 3 diagram is shown (the two groups are swapped in the page's natives)
      bool open = false;      // the page is up: the diagram follows the device in use
      bool keys_hint = false; // the page offers KEY BINDINGS (Y): the keyboard was in use when it opened
      bool open_keys = false; // Y was pressed: the page's exit opens KEY BINDINGS
      uint32_t y_seen = 0;    // sst::input::YPresses() handled
      uint32_t navbar = 0;    // guest array for the navbar: Select, Confirm, Back + the Y button
      uint32_t scratch = 0;   // guest buffer for direct native calls: +0 result (8), +8 values (8 x 4), +40 types (8)

      // The page's last animation (4:10: object, from, to, flags) on the shown diagram, played on the other one when it takes over
      struct Anim
      {
        uint32_t count = 0;
        uint32_t values[8]{};
        uint8_t types[8]{};
      } pad_anim;
    } g;

    bool WantPs3Diagram()
    {
      return REXCVAR_GET(sst_controller_diagram) && sst::input::CurrentPromptStyle() == sst::input::PromptStyle::kPlayStation;
    }

    bool KeyboardInUse()
    {
      return sst::input::CurrentPromptStyle() == sst::input::PromptStyle::kKeyboard;
    }

    // Shows the diagram for the device in use (keyboard: the Xbox pad's, drawn as a keyboard by the prompt textures)
    void UpdateDiagram(PPCContext &ctx, uint8_t *base, bool force)
    {
      const bool ps = WantPs3Diagram();

      if (!force && ps == g.ps)
        return;

      g.ps = ps;
      const uint32_t shown = ps ? kPs3PadGroup : kXboxPadGroup;
      SceneShow(ctx, base, shown, true);
      SceneShow(ctx, base, ps ? kXboxPadGroup : kPs3PadGroup, false);

      // (Live switch: the newly shown diagram plays the page's last animation, so it has the same layout)
      if (!force && g.pad_anim.count)
      {
        State::Anim a = g.pad_anim;
        a.values[0] = shown;
        SceneCall(ctx, base, sub_82282AA0, a.values, a.types, a.count);
      }

      REXLOG_INFO("[sst-menu] controls page: {} diagram", ps ? "DualShock 3" : "Xbox 360 / keyboard");
    }

    // VIBRATION is grayed (half alpha, similar to how the game grays the main menu's Xbox LIVE item which is now renamed as ONLINE PLAY in this port)
    struct Dim
    {
      uint32_t hash;
      bool active = false;
      uint8_t written = 0, original = 0xFF;
    };

    Dim g_dim_controls[] = {{0x7D139BC8}, {0x9FFE01DC}, {0xC9C4FF84}}; // VIBRATION, OFF, ON (SETTINGS_CONTROLLER\OPTION_2)
    Dim g_dim_pause[] = {{0x2408EA3F}, {0xAF864C0C}};                  // VIBRATION, its value (pause menu row F8A9F905)

    uint32_t FindObject(PPCContext &ctx, uint8_t *base, uint32_t hash)
    {
      PPCContext c = ctx;
      c.r3.u64 = kSceneObjects;
      c.r4.u64 = hash;
      sub_82582110(c, base);
      return c.r3.u32;
    }

    void SetAlpha(PPCContext &ctx, uint8_t *base, uint32_t hash, uint32_t alpha)
    {
      const uint32_t values[2] = {hash, alpha};
      const uint8_t types[2] = {kHash, kInt};
      SceneCall(ctx, base, sub_82283318, values, types, 2);
    }

    // Each frame: halves the texts' alpha (the game's own alpha changes - fades, highlights - are followed), or puts it back
    template <size_t N>
    void DimTexts(PPCContext &ctx, uint8_t *base, Dim (&dims)[N], bool dim)
    {
      for (Dim &d : dims)
      {
        if (!dim && !d.active)
          continue;

        const uint32_t obj = FindObject(ctx, base, d.hash);

        if (!obj)
        {
          d.active = false;
          continue;
        }

        const uint8_t a = *GuestPtr(base, obj + 0xB);

        if (dim)
        {
          if (!d.active || a != d.written)
          {
            d.original = a;
            d.written = uint8_t(a / 2);
            d.active = true;
            SetAlpha(ctx, base, d.hash, d.written);
          }
        }
        else
        {
          if (a == d.written)
            SetAlpha(ctx, base, d.hash, d.original);

          d.active = false;
        }
      }
    }

    // The navbar call (2:5, three arguments, the button list in the second): a copy of the list with a fourth button, Y
    void AddNavbarButton(PPCContext &ctx, uint8_t *base, bool keys_page)
    {
      if (!g.navbar)
        g.navbar = GuestAlloc(0x80);

      // (the button bar's font: Global's)
      const uint32_t label = TextId(base, sst::text::Fit(base, sst::text::Tr(keys_page ? "Reset to Defaults" : "Key Bindings"), "global"));

      if (!g.navbar || !label)
        return;

      // A static array object (as a module's): {class, 0xFFFF8800 (no reference count), count, elements (relative), 0x80000000 | capacity}, elements {value, type}: (button, its text) pairs
      const std::pair<uint32_t, uint32_t> elems[] = {{0, kInt}, {0xF1141A0A, kHash}, {6, kInt}, {0xFBD92248, kHash}, {2, kInt}, {0x3A648438, kHash}, {8, kInt}, {label, kHash}};
      const uint32_t list = ctx.r8.u32 + 8;                      // the second argument
      Store32(base, g.navbar, Load32(base, Load32(base, list))); // (+0: the class pointer of the VM's arrays, as the page's own array has it)
      Store32(base, g.navbar + 4, 0xFFFF8800);
      Store32(base, g.navbar + 8, uint32_t(std::size(elems)));
      Store32(base, g.navbar + 12, 0x14);
      Store32(base, g.navbar + 16, 0x80000000u | uint32_t(std::size(elems)));

      for (size_t i = 0; i < std::size(elems); ++i)
      {
        Store32(base, g.navbar + 0x14 + 8 * uint32_t(i), elems[i].first);
        Store32(base, g.navbar + 0x18 + 8 * uint32_t(i), elems[i].second);
      }

      Store32(base, list, g.navbar);
    }

    bool IsNavbarCall(PPCContext &ctx, uint8_t *base, uint32_t id)
    {
      return id == kNativeNavbar && ctx.r10.u32 == 3 && ctx.r8.u32 && Load32(base, ctx.r8.u32) == kNavbarButtons && *GuestPtr(base, ctx.r9.u32 + 2) == 6;
    }

    // The page frame's input read (3:6 on the input events)
    bool IsInputRead(PPCContext &ctx, uint8_t *base, uint32_t id)
    {
      return g.open && id == kNativeInput && ctx.r10.u32 == 1 && ctx.r8.u32 && Load32(base, ctx.r8.u32) == kInputEvents;
    }

    // Y on the page: the input read returns Confirm (the page leaves as with Confirm, keeping the layout type)
    bool HandleY(PPCContext &ctx, uint8_t *base)
    {
      if (sst::input::YPresses() == g.y_seen)
        return false;

      g.y_seen = sst::input::YPresses();
      const uint32_t out = ctx.r3.u32;
      __imp__sub_821DFBC0(ctx, base);

      if (Load32(base, out + 4) <= kHash)
      {
        Store32(base, out, kEventSelect);
        Store32(base, out + 4, kHash);
        g.open_keys = true;
        REXLOG_INFO("[sst-menu] controls page: Y -> KEY BINDINGS");
      }

      return true;
    }

    // True: handled (the original was called)
    bool OnSceneNative(PPCContext &ctx, uint8_t *base, uint32_t index)
    {
      const uint32_t values = ctx.r8.u32;
      const uint32_t first = Load32(base, values);

      // The page opens (its script hides the DualShock 3 diagram): show the one for the device in use (it may still be hidden from an earlier visit), hide the other. The script's own call stays (on the hidden one) for its result.
      if (index == kSceneHide && first == kPs3PadGroup)
      {
        g.open = true;
        g.pad_anim = {};
        g.keys_hint = KeyboardInUse();
        g.y_seen = sst::input::YPresses();
        UpdateDiagram(ctx, base, true);
        Store32(base, values, g.ps ? kXboxPadGroup : kPs3PadGroup);
        __imp__sub_821DFBC0(ctx, base);
        return true;
      }

      // The page closes
      if (index == kSceneLeavePage && first == kControllerLayer)
      {
        g.open = false;
        g.ps = false;
      }

      // An animation on the diagram (layout type): remembered for a live switch (hashes and numbers only)
      if (g.open && index == kSceneAnimate && (first == kXboxPadGroup || first == kPs3PadGroup))
      {
        State::Anim a;
        a.count = ctx.r10.u32;

        for (uint32_t i = 0; i < a.count; ++i)
        {
          a.values[i] = Load32(base, values + 4 * i);
          a.types[i] = *GuestPtr(base, ctx.r9.u32 + i);

          if (a.types[i] > kHash)
            a.count = 0;
        }

        g.pad_anim = a;
      }

      // DualShock 3 shown: every call on one diagram goes to the other
      if (g.ps)
      {
        for (uint32_t i = 0; i < ctx.r10.u32; ++i)
        {
          const uint32_t v = Load32(base, values + 4 * i);

          if (v == kXboxPadGroup || v == kPs3PadGroup)
            Store32(base, values + 4 * i, v == kXboxPadGroup ? kPs3PadGroup : kXboxPadGroup);
        }
      }

      return false;
    }
  }

  bool OnNative(PPCContext &ctx, uint8_t *base, uint32_t id)
  {
    // The diagram follows the device in use (checked at each native the page's frame calls)
    if (g.open)
      UpdateDiagram(ctx, base, false);

    // A fourth navbar button on Y: Key Bindings on the CONTROLS page (keyboard in use), Reset to Defaults on KEY BINDINGS
    const bool keys_page = !g.open && page::KeyBindingsUp();

    if (((g.open && g.keys_hint) || keys_page) && IsNavbarCall(ctx, base, id))
      AddNavbarButton(ctx, base, keys_page);

    // VIBRATION greyed while the keyboard is in use: the page (its frame's input read) and the pause menu (3:18 input reads in a match)
    if (IsInputRead(ctx, base, id))
      DimTexts(ctx, base, g_dim_controls, KeyboardInUse());

    if (id == kNativePauseInput && sst::input::InMatch())
      DimTexts(ctx, base, g_dim_pause, KeyboardInUse());

    if (g.keys_hint && IsInputRead(ctx, base, id) && HandleY(ctx, base))
      return true;

    if ((id >> 8) == kSceneGroup && ctx.r10.u32 >= 1 && ctx.r10.u32 <= 8 && ctx.r8.u32)
      return OnSceneNative(ctx, base, id & 0xFF);

    return false;
  }

  bool TakeOpenKeyBindings()
  {
    return std::exchange(g.open_keys, false);
  }

  void SceneCall(PPCContext &ctx, uint8_t *base, void (*native)(PPCContext &__restrict, uint8_t *), const uint32_t *values, const uint8_t *types, uint32_t count)
  {
    if (!g.scratch)
      g.scratch = GuestAlloc(64);

    if (!g.scratch)
      return;

    count = std::min(count, 8u);
    std::memset(GuestPtr(base, g.scratch), 0, 48);

    for (uint32_t i = 0; i < count; ++i)
    {
      Store32(base, g.scratch + 8 + 4 * i, values[i]);
      *GuestPtr(base, g.scratch + 40 + i) = types[i];
    }

    PPCContext c = ctx;
    c.r3.u64 = g.scratch;
    c.r5.u64 = g.scratch + 8;
    c.r6.u64 = g.scratch + 40;
    c.r7.u64 = count;
    c.r8.u64 = ctx.r6.u64;
    c.r9.u64 = ctx.r7.u64;
    native(c, base);
  }

  void SceneShow(PPCContext &ctx, uint8_t *base, uint32_t object, bool show)
  {
    const uint8_t type = kHash;
    SceneCall(ctx, base, show ? sub_82282BB8 : sub_82282C28, &object, &type, 1);
  }
}