// Code for loading game's scene files. Game uses SumoTool scenes which consist of two files: a .DAT (objects, layouts) and a .REL (its relocations). The port modifies some scenes as they load:
//   - PauseMenu: a VIDEO row and its scroll bar (pause_patch.cpp)
//   - SSTMainMenu: the QUIT row (main_menu_quit.h), the VIDEO list's arrows, the achievements list (achievements.cpp)
//   - MsgBox: the achievement popup
//   - any scene, wider than 16:9: a wide layout (scene_widen.cpp); the in-match HUD scenes are anchored to the screen edges instead (hud_anchor.cpp)
//
// Scenes that grow are read into a bigger buffer (sub_82594618 / sub_825946B0), then rewritten once both files are in (sub_82595158).

#include "menu/achievements.h"
#include "menu/port_text.h"
#include "menu/hud_anchor.h"
#include "menu/main_menu_quit.h"
#include "menu/menu_internal.h"
#include "menu/pause_patch.h"
#include "menu/scene_patches.h"
#include "menu/video_settings.h"

#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <optional>
#include <unordered_map>

extern "C" void __imp__sub_82594618(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_825946B0(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_82595158(PPCContext &__restrict, uint8_t *);
extern "C" void sub_82594820(PPCContext &__restrict, uint8_t *); // file size (path)
extern "C" void sub_82122480(PPCContext &__restrict, uint8_t *); // allocate (size, align)
extern "C" void sub_82128A90(PPCContext &__restrict, uint8_t *); // start reading (fs, path, buffer, 0) -> request handle
extern "C" void sub_821289D8(PPCContext &__restrict, uint8_t *); // read file (fs, path, buffer)

namespace sst::menu
{
  namespace
  {
    using scenes::Bytes;

    constexpr uint32_t kFileSystem = 0x82DB97A0; // the game's file system object
    constexpr int32_t kMaxSceneFile = 0x800000;  // bigger files are read as usual
    constexpr uint32_t kPauseDatExtra = 0x4000, kPauseRelExtra = 0x1000;
    constexpr uint32_t kCaretColour = 0x183E8C00u; // the VIDEO list's arrows (RGB in the top 24 bits)

    // How a scene file is read
    enum class FileKind
    {
      kAsUsual,  // the game's buffer
      kPauseDat, // with room for the VIDEO row
      kPauseRel,
      kOtherDat, // with room for a wide layout / the port's additions
      kOtherRel,
    };

    // A buffer read with room to grow: its size, where the loader keeps the file's size, the file
    struct Room
    {
      uint32_t room, size_at;
      std::string name; // lower-case file name
    };

    std::mutex g_mutex;                                    // the maps below
    std::unordered_map<uint32_t, Room> g_rooms;            // buffer -> its room
    std::unordered_map<uint32_t, std::string> g_dat_names; // every scene's DAT buffer -> file name (HUD anchoring)

    bool WideLayoutFor(const std::string &dat_name)
    {
      // The star field's spare columns apply at any aspect but the game's own 4:3 mode
      const bool star_field = dat_name == "starbg.dat" || dat_name.starts_with("whtfade");
      return WideLayouts() || (star_field && REXCVAR_GET(sst_wide_layouts) && !video::Boot().four_three);
    }

    // The full guest path, lower case
    std::string LowerPath(uint8_t *base, uint32_t path)
    {
      std::string s;

      for (uint32_t i = 0; i < 260; ++i)
      {
        const char c = char(*GuestPtr(base, path + i));

        if (!c)
          break;

        s += char(std::tolower(uint8_t(c)));
      }

      return s;
    }

    FileKind KindOf(uint8_t *base, uint32_t path, const std::string &name)
    {
      if (REXCVAR_GET(sst_graphics_menu))
      {
        const FileKind kind = name == "pausemenu.dat" ? FileKind::kPauseDat : name == "pausemenu.rel" ? FileKind::kPauseRel
                                                                                                      : FileKind::kAsUsual;
        static std::atomic<int> logged{0};

        if (name.ends_with(".dat") && logged.fetch_add(1) < 40)
          REXLOG_INFO("[sst-menu] scene file {}{}", LowerPath(base, path), kind != FileKind::kAsUsual ? " (pause menu)" : "");

        if (kind != FileKind::kAsUsual)
          return kind;
      }

      const bool dat = name.ends_with(".dat"), rel = name.ends_with(".rel");

      if (!dat && !rel)
        return FileKind::kAsUsual;

      const std::string as_dat = rel ? name.substr(0, name.size() - 4) + ".dat" : name;
      const bool wide = WideLayoutFor(as_dat) && scenes::WantsWideLayout(as_dat);
      const bool main_menu = as_dat == "sstmainmenu.dat" && (REXCVAR_GET(sst_quit_item) || REXCVAR_GET(sst_graphics_menu));

      if (wide || main_menu || sst::ach::ExtraRoom(name))
        return dat ? FileKind::kOtherDat : FileKind::kOtherRel;

      return FileKind::kAsUsual;
    }

    uint32_t ExtraRoom(FileKind kind, uint32_t size, const std::string &name)
    {
      uint32_t extra = 0;

      switch (kind)
      {
      case FileKind::kPauseDat:
        extra = kPauseDatExtra;
        break;
      case FileKind::kPauseRel:
        extra = kPauseRelExtra;
        break;
      default:
        extra = scenes::ExtraRoom(size, kind == FileKind::kOtherRel);
        break;
      }

      return extra + sst::ach::ExtraRoom(name);
    }

    // The loader's read (r3 path, r4 -> buffer pointer, r5 -> size, r6 alignment) into a buffer with room to grow. Returns as the game's does: the size (synchronous) or a request handle (asynchronous)
    void ReadWithRoom(PPCContext &ctx, uint8_t *base, FileKind kind, bool async, const std::string &name)
    {
      const uint32_t path = ctx.r3.u32, out_buf = ctx.r4.u32, out_size = ctx.r5.u32, align = ctx.r6.u32;
      ctx.r3.u32 = path;
      sub_82594820(ctx, base);
      const int32_t size = int32_t(ctx.r3.u32);
      Store32(base, out_size, uint32_t(size));

      if (size <= 0 || size > kMaxSceneFile)
      {
        ctx.r3.u32 = async ? 0 : uint32_t(size);
        return;
      }

      const uint32_t room = (uint32_t(size) + ExtraRoom(kind, uint32_t(size), name) + 0x7FF) & ~0x7FFu;
      ctx.r3.u32 = room;
      ctx.r4.u32 = align;
      sub_82122480(ctx, base);
      const uint32_t buffer = ctx.r3.u32;
      Store32(base, out_buf, buffer);

      if (buffer)
      {
        std::lock_guard lock(g_mutex);
        g_rooms[buffer] = {room, out_size, name};

        if (kind == FileKind::kOtherDat)
          g_dat_names[buffer] = name;
      }

      ctx.r3.u32 = kFileSystem;
      ctx.r4.u32 = path;
      ctx.r5.u32 = buffer;

      if (async)
      {
        ctx.r6.u32 = 0;
        sub_82128A90(ctx, base);
      }
      else
      {
        sub_821289D8(ctx, base);
        ctx.r3.u32 = uint32_t(size);
      }
    }

    template <void (*Original)(PPCContext &__restrict, uint8_t *)>
    void ReadScene(PPCContext &ctx, uint8_t *base, bool async)
    {
      const uint32_t path = ctx.r3.u32, out_buf = ctx.r4.u32;
      const std::string name = GuestFileName(base, path);
      const FileKind kind = KindOf(base, path, name);

      if (kind != FileKind::kAsUsual)
      {
        ReadWithRoom(ctx, base, kind, async, name);
        sst::text::OnSceneFile(name, Load32(base, out_buf));
        return;
      }

      Original(ctx, base);

      if (name.ends_with(".dat"))
      { // every scene: off-screen elements (all) / HUD sides
        std::lock_guard lock(g_mutex);
        g_dat_names[Load32(base, out_buf)] = name;
      }

      sst::ach::OnSceneFile(base, name, Load32(base, out_buf));
      sst::text::OnSceneFile(name, Load32(base, out_buf)); // (language files: the fonts the port's texts are fitted to)
    }

    // A scene being set up: its two files in guest memory and, when read with room to grow, how much room they have
    struct Scene
    {
      uint8_t *base;
      uint32_t dat = 0, rel = 0, dat_size = 0, rel_size = 0;
      uint32_t dat_room = 0, rel_room = 0, dat_size_at = 0, rel_size_at = 0; // (0: not read with room)
      std::string name;                                                      // the DAT's file name

      uint8_t *Dat() const { return GuestPtr(base, dat); }
      uint8_t *Rel() const { return GuestPtr(base, rel); }
      bool CanGrow() const { return dat_room && rel_room && dat_size <= dat_room && rel_size <= rel_room; }
      bool Fits(const Bytes &d, const Bytes &r) const { return d.size() <= dat_room && r.size() <= rel_room; }

      // The new files in place of the old (they fit). The loader's own record of the sizes (used again when the scene is released) must match them
      void Replace(const Bytes &d, const Bytes &r)
      {
        std::memcpy(Dat(), d.data(), d.size());
        std::memcpy(Rel(), r.data(), r.size());

        if (Load32(base, dat_size_at) == dat_size)
          Store32(base, dat_size_at, uint32_t(d.size()));

        if (Load32(base, rel_size_at) == rel_size)
          Store32(base, rel_size_at, uint32_t(r.size()));

        dat_size = uint32_t(d.size());
        rel_size = uint32_t(r.size());
      }

      // Replace if the result of a rewrite fits; false (and an error saying why) if not
      bool ReplaceIfFits(bool rewritten, const Bytes &d, const Bytes &r, std::string &error)
      {
        if (!rewritten)
          return false;

        if (!Fits(d, r))
        {
          error = "too large";
          return false;
        }

        Replace(d, r);
        return true;
      }
    };

    // Ultrawide: in-match HUD elements to the screen edges (every scene: elements waiting off-screen move out with the edges)
    void AnchorHud(const Scene &s, const std::string &dat_name)
    {
      const float shift = hud::ShiftFor(CameraAspect());

      if (shift <= 0.0f)
        return;

      std::vector<std::string> log;
      const bool edge_only = hud::IsEdgeHudScene(dat_name);
      const bool sides = edge_only || hud::IsHudScene(dat_name);
      const int moved = hud::Anchor(s.Dat(), s.dat_size, shift, sides ? &log : nullptr, sides, edge_only);

      if (!sides)
        return;

      REXLOG_INFO("[sst-menu] {}: {} HUD elements to the screen edges (shift {:.3f})", dat_name, moved, shift);

      for (const std::string &line : log)
        REXLOG_INFO("[sst-menu]   {}", line);
    }

    // Main menu: the QUIT row (before the wide layout, which then covers it too)
    void AddQuitRow(Scene &s)
    {
      const scenes::MainMenuRow row{quit::kParent, quit::kRow, quit::kHighlight, quit::kFirstChild, quit::kTextLabel, quit::kBarLabel, quit::kOptionsFrom, quit::kOptionsTo, 0.0f};
      Bytes d, r;
      std::string error;
      const bool ok = s.ReplaceIfFits(scenes::AddMainMenuRow(s.Dat(), s.dat_size, s.Rel(), s.rel_size, row, d, r, error), d, r, error);
      SetQuitSceneOk(ok);

      if (ok)
        REXLOG_INFO("[sst-menu] sstmainmenu.dat: QUIT row added");
      else
        REXLOG_ERROR("[sst-menu] sstmainmenu.dat: QUIT row not added ({})", error.empty() ? std::string("too large") : error);
    }

    // Settings page: up / down arrows for the VIDEO / KEY BINDINGS lists (video_page.cpp shows them)
    void AddListArrows(Scene &s)
    {
      const scenes::ListCarets carets{kCaretUp, kCaretDown, kCaretHide, kCaretShow, kCaretColour};
      Bytes d, r;
      std::string error;

      if (s.ReplaceIfFits(scenes::AddListCarets(s.Dat(), s.dat_size, s.Rel(), s.rel_size, carets, d, r, error), d, r, error))
        REXLOG_INFO("[sst-menu] sstmainmenu.dat: list arrows added");
      else
        REXLOG_WARN("[sst-menu] sstmainmenu.dat: list arrows not added ({})", error.empty() ? std::string("too large") : error);
    }

    // Settings > GENERAL: a fifth row (LANGUAGE; video_page.cpp)
    std::atomic<bool> g_general_row{false};

    void AddGeneralRow(Scene &s)
    {
      const scenes::GeneralRow row{kGeneralBar, kGeneralValues, kGeneralLabel, kGeneralValue, kGeneralCursorFrom, kGeneralCursorTo};
      Bytes d, r;
      std::string error;
      const bool ok = s.ReplaceIfFits(scenes::AddGeneralRow(s.Dat(), s.dat_size, s.Rel(), s.rel_size, row, d, r, error), d, r, error);
      g_general_row = ok;

      if (ok)
        REXLOG_INFO("[sst-menu] sstmainmenu.dat: GENERAL fifth row added");
      else
        REXLOG_WARN("[sst-menu] sstmainmenu.dat: GENERAL fifth row not added ({}); the list scrolls", error.empty() ? std::string("too large") : error);
    }

    // Achievements: the list on the RECORDS page (main menu), the unlock popup (MsgBox)
    void AddAchievements(Scene &s)
    {
      Bytes d, r;
      bool ok = false;

      if (sst::ach::BuildScene(s.name, s.Dat(), s.dat_size, s.Rel(), s.rel_size, d, r))
      {
        ok = s.Fits(d, r);

        if (ok)
          s.Replace(d, r);
        else
          REXLOG_ERROR("[sst-ach] {}: patched scene too large for its buffer ({} / {}, {} / {})", s.name, d.size(), s.dat_room, r.size(), s.rel_room);
      }

      sst::ach::SceneReady(s.name, ok);
    }

    // Ultrawide: the scene's wide layout
    void AddWideLayout(Scene &s)
    {
      const float shift = hud::ShiftFor(CameraAspect());
      Bytes d, r;
      scenes::Stats stats;
      std::vector<std::string> log;
      std::string error;
      const uint32_t old_dat = s.dat_size, old_rel = s.rel_size;

      if (!scenes::Widen(s.name, s.Dat(), s.dat_size, s.Rel(), s.rel_size, shift, d, r, stats, &log, error))
      {
        REXLOG_WARN("[sst-menu] {}: no wide layout ({})", s.name, error);
        return;
      }

      if (!stats.Total())
        return;

      if (!s.Fits(d, r))
      {
        REXLOG_WARN("[sst-menu] {}: wide layout too large for its buffer ({} / {})", s.name, d.size(), s.dat_room);
        return;
      }

      s.Replace(d, r);
      REXLOG_INFO("[sst-menu] {}: wide layout (shift {:.3f}): {} bars, {} mirrored strips, {} cropped backgrounds, {} swoosh, {} stars (DAT {} -> {}, REL {} -> {} bytes)", s.name, shift, stats.bars, stats.strips, stats.cropped, stats.swoosh, stats.stars, old_dat, d.size(), old_rel, r.size());

      for (const std::string &line : log)
        REXLOG_INFO("[sst-menu]   {}", line);
    }

    // The pause menu: the VIDEO row and its scroll bar. Both files together, or not at all (the patched script needs both)
    void AddPauseVideoRow(Scene &s)
    {
      Bytes d(s.Dat(), s.Dat() + s.dat_size), r(s.Rel(), s.Rel() + s.rel_size);
      std::string error;

      // The scroll thumb's texture: the middle of the yellow 3P tag
      uint32_t thumb_tex = pause::kYellowTag;
      {
        Bytes sd, sr;

        if (scenes::AddSubTexture(d.data(), d.size(), r.data(), r.size(), pause::kYellowTag, pause::kThumbTex, 0.4f, 0.6f, 0.5f, 0.85f, sd, sr, error))
        {
          d = std::move(sd);
          r = std::move(sr);
          thumb_tex = pause::kThumbTex;
        }
        else
        {
          REXLOG_WARN("[sst-menu] pause scroll thumb texture: {}", error);
          error.clear();
        }
      }

      pause::Relocs relocs;
      const auto new_dat = pause::PatchDat(d.data(), d.size(), relocs, error, thumb_tex);
      const auto new_rel = new_dat ? pause::PatchRel(r.data(), r.size(), relocs, error) : std::nullopt;
      const uint32_t old_dat = s.dat_size, old_rel = s.rel_size;
      const bool ok = new_dat && new_rel && s.Fits(*new_dat, *new_rel);
      SetPauseSceneOk(ok);

      if (!ok)
      {
        REXLOG_ERROR("[sst-menu] PauseMenu scene not patched: {}", error.empty() ? std::string("too large") : error);
        return;
      }

      s.Replace(*new_dat, *new_rel);
      REXLOG_INFO("[sst-menu] PauseMenu scene patched: VIDEO row (DAT {} -> {}, REL {} -> {} bytes)", old_dat, s.dat_size, old_rel, s.rel_size);
    }
  }

  bool GeneralRowOk() { return g_general_row; }

  // Ultrawide layouts (scene_widen.cpp): when the game started with an aspect ratio wider than 16:9
  bool WideLayouts()
  {
    return REXCVAR_GET(sst_wide_layouts) && !video::Boot().four_three && hud::ShiftFor(CameraAspect()) > 0.001f;
  }
}

using namespace sst::menu;

// SumoTool reads scene files through sub_82594618 (synchronous) or sub_825946B0 (asynchronous: the in-match loader)
extern "C" REX_FUNC(sub_82594618)
{
  ReadScene<__imp__sub_82594618>(ctx, base, false);
}

extern "C" REX_FUNC(sub_825946B0)
{
  ReadScene<__imp__sub_825946B0>(ctx, base, true);
}

// SumoTool: set up a scene from its DAT and REL: r4 = {DAT pointer, size}, r5 = {REL pointer, size} (pointer in the high word)
extern "C" REX_FUNC(sub_82595158)
{
  Scene s{base};
  s.dat = uint32_t(ctx.r4.u64 >> 32), s.dat_size = uint32_t(ctx.r4.u64);
  s.rel = uint32_t(ctx.r5.u64 >> 32), s.rel_size = uint32_t(ctx.r5.u64);

  std::string dat_name; // any scene's (HUD anchoring)
  {
    std::lock_guard lock(g_mutex);

    if (auto it = g_dat_names.find(s.dat); it != g_dat_names.end())
    {
      dat_name = it->second;
      g_dat_names.erase(it);
    }

    if (auto it = g_rooms.find(s.dat); it != g_rooms.end())
    {
      s.dat_room = it->second.room, s.dat_size_at = it->second.size_at, s.name = it->second.name;
      g_rooms.erase(it);
    }

    if (auto it = g_rooms.find(s.rel); it != g_rooms.end())
    {
      s.rel_room = it->second.room, s.rel_size_at = it->second.size_at;
      g_rooms.erase(it);
    }
  }

  if (!dat_name.empty() && s.dat && s.dat_size && !video::Boot().four_three)
    AnchorHud(s, dat_name);

  if (s.CanGrow())
  {
    // (The port's rows and lists first: the wide layout then covers them too)
    if (s.name == "sstmainmenu.dat" && REXCVAR_GET(sst_quit_item))
      AddQuitRow(s);

    if (s.name == "sstmainmenu.dat" && REXCVAR_GET(sst_graphics_menu))
      AddListArrows(s);

    if (s.name == "sstmainmenu.dat" && REXCVAR_GET(sst_graphics_menu) && REXCVAR_GET(sst_language_menu))
      AddGeneralRow(s);

    if (s.name == "sstmainmenu.dat" || s.name == "msgbox.dat")
      AddAchievements(s);

    if (s.name != "pausemenu.dat" && WideLayoutFor(s.name) && scenes::WantsWideLayout(s.name))
      AddWideLayout(s);

    if (s.name == "pausemenu.dat")
      AddPauseVideoRow(s);
  }

  ctx.r4.u64 = (uint64_t(s.dat) << 32) | s.dat_size;
  ctx.r5.u64 = (uint64_t(s.rel) << 32) | s.rel_size;
  __imp__sub_82595158(ctx, base);
}