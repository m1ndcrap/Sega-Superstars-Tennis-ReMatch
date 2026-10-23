// The script VM's native calls (sub_821DFBC0). The port's scripts call the port's natives (group 0x53): the list page's (video_page.cpp) and the ones here (QUIT, the boot intro, pause menu navigation). Calls to the game's natives are where the port changes what the game's own pages do (controls_page.cpp, the QUIT row's animation, the pause menu's Esc).

#include "input/controls.h"
#include "menu/achievements.h"
#include "menu/main_menu_quit.h"
#include "menu/video_settings.h"
#include "menu/menu_internal.h"
#include "menu/pause_patch.h"
#include "menu/restart.h"
#include "menu/vs_screen.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <vector>

extern "C" void __imp__sub_821DFBC0(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_8227C100(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_8227BC40(PPCContext &__restrict, uint8_t *);

REXCVAR_DEFINE_BOOL(sst_esrb_notice, true, "SST", "Boot intro: show the ESRB rating card, as on a North American console (the game shows it only when XGetGameRegion reports North America; the SDK reports all regions)");

namespace sst::menu
{
  static_assert(pause::kNativeResult == kResult && pause::kNativeEventHash == kPauseEvent && pause::kNativeBegin == kPauseBegin && pause::kNativeLabel == kPauseLabel && pause::kNativeValue == kPauseValue && pause::kNativeStep == kPauseStep);
  static_assert(quit::kNativeDialogBegin == kQuitDialogBegin && quit::kNativeDialogEnd == kQuitDialogEnd && quit::kNativeQuit == kQuit && quit::kNativeEscape == kQuitEscape);

  namespace
  {
    constexpr uint32_t kNativePauseInput = 0x312;                                      // 3:18: the pause menu's input read
    constexpr uint32_t kSceneAnimateKeys = 0x413;                                      // 4:19: play keys (object, from, to, ...)
    constexpr uint32_t kOptionsCloseFrom = 0xDD6C3C34u, kOptionsCloseTo = 0xF5FDA6B0u; // the rows below OPTIONS as it closes

    std::mutex g_mutex;         // the state below
    uint32_t g_escape_seen = 0; // sst::input::EscapePresses() at the main menu's last frame
    uint32_t g_intro_presses = 0;
    std::chrono::steady_clock::time_point g_intro_timer{};
    bool g_quit_row_pushed = false; // the QUIT row was pushed down by OPTIONS' sub-items

    // Main menu, every frame (argument: menu level, 1 = the category list) -> 1: Esc was pressed there, ask to QUIT
    bool QuitOnEscape(bool category_list)
    {
      sst::input::MainMenuFrame(category_list && !QuitDialogUp());
      const uint32_t presses = sst::input::EscapePresses();
      const bool ask = category_list && !QuitDialogUp() && presses != g_escape_seen;
      g_escape_seen = presses;

      if (ask)
        REXLOG_INFO("[sst-menu] Esc on the main menu: QUIT?");

      return ask;
    }

    // Boot intro: (ms) -> 1 on a new Start / A press, or (ms > 0) once ms have passed since the timer started
    bool IntroSkip(int ms)
    {
      const uint32_t presses = sst::input::ConfirmPresses();

      if (presses != g_intro_presses)
      {
        g_intro_presses = presses;
        REXLOG_INFO("[sst-menu] intro: skipped");
        return true;
      }

      return ms > 0 && std::chrono::steady_clock::now() - g_intro_timer >= std::chrono::milliseconds(ms);
    }

    // Pause menu up / down: the next row in screen order (pause::kVisualOrder) among rows first .. first + count - 1, wrapping
    int PauseStep(int row, int dir, int first, int n)
    {
      std::vector<int> order;

      for (int r : pause::kVisualOrder)
      {
        if (r >= first && r < first + n)
          order.push_back(r);
      }

      for (int r = first; r < first + n; ++r) // (any rows the order doesn't list)
      {
        if (std::find(order.begin(), order.end(), r) == order.end())
          order.push_back(r);
      }

      if (order.empty())
        return row;

      const auto it = std::find(order.begin(), order.end(), row);
      const int i = it == order.end() ? (dir > 0 ? -1 : 0) : int(it - order.begin());
      const int size = int(order.size());
      return order[((i + dir) % size + size) % size];
    }

    // The port's natives but the page's
    void HandleOwnNative(const NativeCall &call, uint32_t index, uint32_t id)
    {
      std::lock_guard lock(g_mutex);

      switch (index)
      {
      case kQuitDialogBegin:

      case kQuitDialogEnd:
        SetQuitDialogUp(index == kQuitDialogBegin);
        sst::input::SetDialogUp(index == kQuitDialogBegin);
        call.ReturnInt(0);
        break;

      case kQuit:
        REXLOG_INFO("[sst-menu] QUIT confirmed: closing the game");
        call.ReturnInt(0);
        RequestQuit();

        // (The normal shutdown, asked for from the script thread, does not finish on its own: end the process once the log is out)
        ExitSoon(1000);
        break;

      case kQuitEscape:
        call.ReturnInt(QuitOnEscape(call.Int(0) == 1));
        break;

      case kIntroBegin:
        g_intro_presses = sst::input::ConfirmPresses();
        g_intro_timer = std::chrono::steady_clock::now();
        call.ReturnInt(0);
        break;

      case kIntroTimer:
        g_intro_timer = std::chrono::steady_clock::now();
        call.ReturnInt(0);
        break;

      case kIntroSkip:
        call.ReturnInt(IntroSkip(call.Int(0)));
        break;

      case kPauseStep:
        call.ReturnInt(PauseStep(call.Int(0), call.Int(1) < 0 ? -1 : 1, call.Int(2), call.Int(3)));
        break;

      default:
        REXLOG_WARN("[sst-menu] unknown native {:04X}", id);
        call.Return(0, kNil);
        break;
      }
    }

    // The scene animations a page native asked for
    void PlayAnims(PPCContext &ctx, uint8_t *base)
    {
      for (const page::Anim &a : page::TakeAnims())
      {
        if (a.key == page::kAnimShow || a.key == page::kAnimHide)
        {
          controls::SceneShow(ctx, base, a.object, a.key == page::kAnimShow);
          continue;
        }

        const uint32_t values[4] = {a.object, a.key, a.key, 0};
        const uint8_t types[4] = {kHash, kHash, kHash, kInt};
        controls::SceneCall(ctx, base, sub_82282AA0, values, types, 4);
      }
    }

    // A native call from the pause menu's script (r4 = the calling VM, +8 its module)
    bool FromPauseMenu(PPCContext &ctx, uint8_t *base)
    {
      return ctx.r4.u32 && IsPauseMenuModule(Load32(base, ctx.r4.u32 + 8));
    }

    // The pause menu's script reads input (3:18) only while the menu is up: there, Esc is 'back', not Start
    void PauseMenuInput(PPCContext &ctx, uint8_t *base)
    {
      if (FromPauseMenu(ctx, base))
        sst::input::PauseMenuFrame();
    }

    // QUIT row: closing OPTIONS plays (kOptionsCloseFrom -> kOptionsCloseTo) on the rows below it, which for this row is the wrong distance; it goes back from where OPTIONS put it instead
    void FixQuitRowAnimation(uint8_t *base, uint32_t values)
    {
      if (Load32(base, values) != quit::kRow)
        return;

      const uint32_t from = Load32(base, values + 4), to = Load32(base, values + 8);
      const bool pushed = g_quit_row_pushed;
      g_quit_row_pushed = to == quit::kOptionsTo;

      if (pushed && from == kOptionsCloseFrom && to == kOptionsCloseTo)
      {
        Store32(base, values + 4, quit::kOptionsTo);
        Store32(base, values + 8, quit::kOptionsFrom);
      }
    }
  }
}

using namespace sst::menu;

// Script VM: call native r5 (group << 8 | index)
extern "C" REX_FUNC(sub_821DFBC0)
{
  const uint32_t id = ctx.r5.u32 & 0xFFFF;

  if ((id >> 8) == sst::ach::kNativeGroup)
  {
    sst::ach::HandleNative(ctx, base, id);
    return;
  }

  if (sst::ach::OverrideNative(ctx, base, id))
    return;

  if ((id >> 8) == kNativeGroup)
  {
    const NativeCall call(ctx, base);

    if (!page::HandleNative(ctx, call, id & 0xFF))
      HandleOwnNative(call, id & 0xFF, id);

    ctx.r3.u32 = call.out;
    PlayAnims(ctx, base);

    // The list pages' per-frame natives: RESOLUTION grayed while it is more than the monitor can show
    if ((id & 0xFF) == kPageFrame || (id & 0xFF) == kPauseEvent || (id & 0xFF) == kEvent)
      page::UpdateDims(ctx, base);

    return;
  }

  if (controls::OnNative(ctx, base, id))
    return;

  if (id == kNativePauseInput && sst::input::InMatch())
    PauseMenuInput(ctx, base);

  if (id == kSceneAnimateKeys && ctx.r10.u32 >= 3 && ctx.r10.u32 <= 8 && ctx.r8.u32)
    FixQuitRowAnimation(base, ctx.r8.u32);

  // The VIDEO page's scroll bar sits in the pause panel, so the panel's intro (the script's first natives, before it reads input) showed it too: after each of the pause script's natives it is hidden again, except on the VIDEO page
  const vs::Pending vs_call = vs::BeforeNative(ctx, base, id);
  const bool pause_menu = sst::input::InMatch() && FromPauseMenu(ctx, base) && !page::ScrollBarShown();
  const uint64_t r6 = ctx.r6.u64, r7 = ctx.r7.u64; // (the scene natives' context, which SceneShow passes on; volatile across the call)

  __imp__sub_821DFBC0(ctx, base);
  vs::AfterNative(base, vs_call);

  if (pause_menu && !page::ScrollBarShown())
  {
    ctx.r6.u64 = r6;
    ctx.r7.u64 = r7;
    controls::SceneShow(ctx, base, pause::kScrollGroup, false);

    // (and parked off the frame: its hidden key)
    const uint32_t values[4] = {pause::kScrollGroup, pause::kScrollHide, pause::kScrollHide, 0};
    const uint8_t types[4] = {kHash, kHash, kHash, kInt};
    controls::SceneCall(ctx, base, sub_82282AA0, values, types, 4);
  }
}

// An on-court object's screen x, -1..1, projected with the game camera (sub_82214BD8). Scripts turn it into a layout x ((x + 1) / 2 * width) for markers over the 3D scene: Space Harrier's target reticles (ballgamehuds). The 2D layout is fitted into the middle 16:9 area, so the marker landed nearer the middle than its target the further out it was: x is scaled by aspect / (16/9) to land on the target across the whole width...
extern "C" REX_FUNC(sub_8227C100)
{
  const uint32_t out = ctx.r3.u32;
  __imp__sub_8227C100(ctx, base);

  const float aspect = sst::menu::CameraAspect();

  if (sst::menu::video::Boot().four_three || aspect <= 16.0f / 9.0f + 0.01f || sst::menu::Load32(base, out + 4) != 1) // (1: a float)
    return;

  const float x = std::bit_cast<float>(sst::menu::Load32(base, out));
  sst::menu::Store32(base, out, std::bit_cast<uint32_t>(x * aspect / (16.0f / 9.0f)));
}

// A player's aim x in layout pixels, (aim + 1) * width / 2 from the aim kept at player+0x190 (-1..1, screen x of the shot). Virtua Squad's shooting missions draw their crosshairs there (ballgamehuds). The shot follows the aim over the whole (wider) screen while the 2D layout is fitted into the
// middle 16:9 area, so the crosshair sat nearer the middle than where the ball went: the pixel x spreads about the middle by aspect / (16/9). (9:33, y, is the same at any width).
extern "C" REX_FUNC(sub_8227BC40)
{
  const uint32_t out = ctx.r3.u32;
  __imp__sub_8227BC40(ctx, base);

  const float aspect = sst::menu::CameraAspect();
  const uint32_t width = sst::menu::Load32(base, 0x825F0540); // 4:20's width

  if (sst::menu::video::Boot().four_three || aspect <= 16.0f / 9.0f + 0.01f || sst::menu::Load32(base, out + 4) != 2 || width < 320 || width > 16384) // (2: an int)
    return;

  const float half = 0.5f * float(width);
  const float x = float(int32_t(sst::menu::Load32(base, out)));
  sst::menu::Store32(base, out, uint32_t(int32_t(std::lround(half + (x - half) * aspect / (16.0f / 9.0f)))));
}

extern "C" REX_FUNC(sub_82280E18)
{
  const uint32_t flag = REXCVAR_GET(sst_esrb_notice) ? 1u : uint32_t(*sst::menu::GuestPtr(base, 0x82DB7F2E));
  sst::menu::Store32(base, ctx.r3.u32, flag);
  sst::menu::Store32(base, ctx.r3.u32 + 4, 2); // int
}