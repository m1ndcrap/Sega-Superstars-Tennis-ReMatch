// Rows and arrows the port adds to the game's scenes: the main menu's QUIT category, the Settings list's up / down arrows, and a texture entry cut from part of another (the pause menu's scroll thumb)

#include "menu/scene_format.h"
#include "menu/scene_patches.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

#include <fmt/format.h>

namespace sst::menu::scenes
{
  using namespace scene;

  // Main menu (SSTMainMenu.DAT, scene 83D07B85): a seventh category row, QUIT, below OPTIONS (see scene_patches.h)
  bool AddMainMenuRow(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, const MainMenuRow &row, Bytes &out_dat, Bytes &out_rel, std::string &error)
  {
    // The Xbox LIVE row (a category without sub-items): parent group (position) -> row group (accordion keys) + highlight group
    constexpr uint32_t kLiveParent = 0x244E79C8, kLiveRow = 0xDE3CBB75, kLiveHighlight = 0x028A51FF;
    constexpr uint32_t kOptionsParent = 0xADD85F3F; // OPTIONS, the last row
    constexpr uint32_t kCursorBar = 0x67B9D635;     // one key per category (labels: the script's table)
    constexpr uint32_t kDescription = 0xC2B6DFEA;   // description panel below the rows
    constexpr uint32_t kOptionsBarLabel = 0xD90AE79C;
    constexpr uint32_t kScene = 0x83D07B85;
    Scene s;

    if (!Parse(dat, dat_size, s, error))
      return false;

    for (uint32_t h : {kLiveParent, kLiveRow, kLiveHighlight, kOptionsParent, kCursorBar, kDescription})
      if (!s.objects.count(h) || !s.tracks.count(h))
        return error = fmt::format("object {:08X} missing", h), false;

    if (s.objects.count(row.parent))
      return error = "already added", false;

    auto key_y = [&](uint32_t h, int k = 0)
    { return RdF(s.tracks[h].Key(k) + 0x10); };

    // Row spacing: OPTIONS' position minus Xbox LIVE's (both parents have one key)
    const float dy = key_y(kOptionsParent) - key_y(kLiveParent);

    if (!(dy > 0.05f && dy < 0.1f))
      return error = fmt::format("row spacing {:.3f}", dy), false;

    // The cursor bar's OPTIONS key; the new key goes one row below it
    Track &bar = s.tracks[kCursorBar];
    int bar_options = -1;

    for (int k = 0; k < bar.count; ++k)
    {
      if (Rd32(bar.Key(k)) == kOptionsBarLabel)
        bar_options = k;
    }

    if (bar_options < 0 || bar.kind != 0)
      return error = "cursor bar keys", false;

    // Clone the Xbox LIVE subtree with the row's hashes
    std::vector<uint32_t> new_hashes = {row.parent, row.row, row.highlight};

    for (uint32_t i = 0; i < 8; ++i)
      new_hashes.push_back(row.first_child + i);

    for (uint32_t h : new_hashes)
    {
      if (s.used.count(h))
        return error = fmt::format("hash {:08X} in use", h), false;
    }

    uint32_t next_child = row.first_child;

    std::function<bool(uint32_t, uint32_t)> clone = [&](uint32_t from, uint32_t to) -> bool
    {
      Object o = s.objects[from];
      o.order = s.next_order++;
      const bool at8 = o.type == Tag("TEXT") || o.type == Tag("TXTR");

      if (o.blob.size() < (at8 ? 12u : 4u))
        return false;

      Wr32(o.blob.data() + (at8 ? 8 : 0), to);

      if (o.type == Tag("TEXT"))
        Wr32(o.blob.data() + 4, row.text);

      std::vector<uint32_t> kids = o.children;
      o.children.clear();
      s.objects[to] = std::move(o);
      s.used.insert(to);

      if (s.tracks.count(from))
      {
        Track t = s.tracks[from];
        t.order = s.next_order++;
        s.tracks[to] = std::move(t);
      }

      for (uint32_t k : kids)
      {
        uint32_t nk;

        if (k == kLiveRow)
          nk = row.row;
        else if (k == kLiveHighlight)
          nk = row.highlight;
        else if (next_child < row.first_child + 8)
          nk = next_child++;
        else
          return false;

        if (!s.objects.count(k) || !clone(k, nk))
          return false;

        s.objects[to].children.push_back(nk);
        s.parent[nk] = to;
      }

      return true;
    };

    if (!clone(kLiveParent, row.parent))
      return error = "clone", false;

    // One row below OPTIONS
    {
      Track &t = s.tracks[row.parent];

      for (int k = 0; k < t.count; ++k)
        WrF(t.Key(k) + 0x10, key_y(kOptionsParent) + dy);
    }

    // How far OPTIONS' sub-items push what is below: the description panel's OPTIONS keys (closed 1F47BC9D -> open 6893313A)
    float options_shift = row.options_shift;
    {
      Track &t = s.tracks[kDescription];
      float closed = -1, open = -1;

      for (int k = 0; k < t.count; ++k)
      {
        if (Rd32(t.Key(k)) == 0x1F47BC9Du)
          closed = RdF(t.Key(k) + 0x10);

        if (Rd32(t.Key(k)) == 0x6893313Au)
          open = RdF(t.Key(k) + 0x10);
      }

      if (closed >= 0 && open > closed + 0.05f && open < closed + 0.4f)
        options_shift = open - closed;

      // The game pushes the panel further for OPTIONS (3 sub-items) than the rows below a category need: with a row below OPTIONS that leaves a gap and puts the panel under the button bar. A two-item category pushes by ebd48b4c's distance; one more sub-item (0.041 apart) makes the natural distance, used for the new row and the panel alike
      float two = -1;

      for (int k = 0; k < t.count; ++k)
      {
        if (Rd32(t.Key(k)) == 0xEBD48B4Cu)
          two = RdF(t.Key(k) + 0x10) - closed;
      }

      if (closed >= 0 && two > 0.05f && two + 0.041f < options_shift)
      {
        options_shift = two + 0.041f;

        for (int k = 0; k < t.count; ++k)
        {
          if (Rd32(t.Key(k)) == 0x6893313Au)
            WrF(t.Key(k) + 0x10, closed + options_shift);
        }
      }
    }

    // Pushed down by OPTIONS' sub-items: keys from_label (y 0) and to_label (y shift) at frames past the row's own animations
    {
      Track &t = s.tracks[row.row];

      if (t.kind != 0 || !t.count)
        return error = "row keys", false;

      Bytes key(t.Key(0), t.Key(0) + t.KeySize());

      for (int k = 0; k < t.count; ++k)
      {
        if (RdF(t.Key(k) + 0x10) == 0.0f)
          key.assign(t.Key(k), t.Key(k) + t.KeySize());
      }

      for (auto [label, frame, y] : {std::tuple{row.options_from, 40u, 0.0f}, std::tuple{row.options_to, 50u, options_shift}})
      {
        Wr32(key.data(), label);
        Wr32(key.data() + 4, frame);
        WrF(key.data() + 0x10, y);
        t.keys.insert(t.keys.end(), key.begin(), key.end());
        ++t.count;
      }
    }

    // Cursor bar: a key for the new row
    {
      Bytes key(bar.Key(bar_options), bar.Key(bar_options) + bar.KeySize());
      uint32_t last_frame = 0;

      for (int k = 0; k < bar.count; ++k)
        last_frame = std::max(last_frame, Rd32(bar.Key(k) + 4));

      const float bar_dy = key_y(kCursorBar, bar_options) - key_y(kCursorBar, std::max(bar_options - 1, 0));
      Wr32(key.data(), row.bar_label);
      Wr32(key.data() + 4, last_frame + 1);
      WrF(key.data() + 0x10, key_y(kCursorBar, bar_options) + (bar_dy > 0.05f && bar_dy < 0.1f ? bar_dy : dy));
      bar.keys.insert(bar.keys.end(), key.begin(), key.end());
      ++bar.count;
    }

    // The description panel moves down one row (all its keys)
    {
      Track &t = s.tracks[kDescription];

      for (int k = 0; k < t.count; ++k)
        WrF(t.Key(k) + 0x10, RdF(t.Key(k) + 0x10) + dy);
    }

    // The new row is a top-level object of the main menu scene
    bool listed = false;

    for (SceneEntry &se : s.scenes)
    {
      if (se.hash == kScene && std::find(se.list.begin(), se.list.end(), kLiveParent) != se.list.end())
      {
        se.list.push_back(row.parent);
        std::sort(se.list.begin(), se.list.end());
        listed = true;
      }
    }

    if (!listed)
      return error = "scene list", false;

    return Finish(s, rel, rel_size, out_dat, out_rel, error);
  }

  bool AddSubTexture(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, uint32_t from, uint32_t to, float u0, float u1, float v0, float v1, Bytes &out_dat, Bytes &out_rel, std::string &error)
  {
    Scene s;

    if (!Parse(dat, dat_size, s, error))
      return false;

    if (!s.ptex.count(from))
      return error = fmt::format("texture {:08X} missing", from), false;

    if (s.ptex.count(to) || s.used.count(to))
      return error = "already added", false;

    const Ptex &t = s.ptex.at(from);
    const float tu0 = t.uv[0], tu1 = t.uv[2], tv0 = t.uv[1], tv1 = t.uv[5]; // top left, top right, bottom right

    auto u = [&](float f)
    {
      return tu0 + (tu1 - tu0) * f;
    };

    auto v = [&](float f)
    {
      return tv0 + (tv1 - tv0) * f;
    };

    Ptex n = t;
    n.uv[0] = n.uv[6] = u(u0);
    n.uv[2] = n.uv[4] = u(u1);
    n.uv[1] = n.uv[3] = v(v0);
    n.uv[5] = n.uv[7] = v(v1);
    s.ptex[to] = n;
    s.used.insert(to);
    return Finish(s, rel, rel_size, out_dat, out_rel, error);
  }

  bool AddListCarets(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, const ListCarets &carets, Bytes &out_dat, Bytes &out_rel, std::string &error)
  {
    // GENERAL page (scene 6A94D0C1): row 0's group and its value arrows (left 01F961FE, right 91A8461D, mirrored), row 3's group and left arrow; a top-level row group of the page to list the carets with
    constexpr uint32_t kRow0 = 0xE2906172, kLeft0 = 0x01F961FE, kRight0 = 0x91A8461D;
    constexpr uint32_t kRow3 = 0x171A6630, kLeft3 = 0x7FD5AEAE;
    constexpr uint32_t kPageRow = 0xDA2F6C3D;
    Scene s;

    if (!Parse(dat, dat_size, s, error))
      return false;

    for (uint32_t h : {kRow0, kLeft0, kRight0, kRow3, kLeft3, kPageRow})
      if (!s.objects.count(h) || !s.tracks.count(h))
        return error = fmt::format("object {:08X} missing", h), false;

    if (s.used.count(carets.up) || s.used.count(carets.down))
      return error = "already added", false;

    Track &left0 = s.tracks[kLeft0];

    if (left0.kind != 0 || !left0.count || s.tracks[kLeft3].kind != 0 || s.tracks[kRight0].kind != 0)
      return error = "arrow keys", false;

    // The arrows' resting place: their last key; positions are relative to the row groups (one key each)
    auto last = [&](uint32_t h) -> uint8_t *
    {
      Track &t = s.tracks[h];
      return t.Key(t.count - 1);
    };

    auto group_y = [&](uint32_t h)
    {
      return RdF(s.tracks[h].Key(0) + 0x10);
    };

    const float sx = RdF(last(kLeft0) + 0x14), sy = RdF(last(kLeft0) + 0x18);
    const float bw = left0.w * sx, bh = left0.h * sy; // the arrow's box

    // A sprite is scaled and turned about its pivot (blob +0x2C / +0x30: offset from the key's position); its centre is: pivot + scale * (half size - pivot)

    const float px = RdF(s.objects[kLeft0].blob.data() + 0x2C), py = RdF(s.objects[kLeft0].blob.data() + 0x30);

    auto centre = [&](float key, float pivot, float size, float scale)
    {
      return key + pivot + scale * (size / 2 - pivot);
    };

    const float sx_right = RdF(last(kRight0) + 0x14);
    const float x_left = centre(RdF(last(kLeft0) + 0xC), px, left0.w, sx), x_right = centre(RdF(last(kRight0) + 0xC), px, left0.w, sx_right);
    const float y0 = centre(group_y(kRow0) + RdF(last(kLeft0) + 0x10), py, left0.h, sy), y3 = centre(group_y(kRow3) + RdF(last(kLeft3) + 0x10), py, left0.h, sy);
    const float spacing = (y3 - y0) / 3;

    if (!(bw > 0.005f && bw < 0.05f && bh > 0.005f && bh < 0.08f && spacing > 0.04f && spacing < 0.15f && x_right > x_left))
      return error = fmt::format("layout (box {:.3f} x {:.3f}, spacing {:.3f})", bw, bh, spacing), false;

    // Centred between the value arrows, half a row spacing above row 0 / below row 3 (left arrow + 90 degrees = up)
    const float cx = (x_left + x_right) / 2;

    const struct
    {
      uint32_t h;
      float cy, rot;
    } kCarets[] = {{carets.up, y0 - spacing * 0.55f, 1.5707964f}, {carets.down, y3 + spacing * 0.55f, -1.5707964f}};

    for (const auto &c : kCarets)
    {
      Object o = s.objects[kLeft0];
      o.order = s.next_order++;
      Wr32(o.blob.data() + 8, c.h);
      s.objects[c.h] = std::move(o);
      s.used.insert(c.h);
      Track t = left0;
      t.order = s.next_order++;
      Bytes key(last(kLeft0), last(kLeft0) + t.KeySize());
      t.keys.clear();
      t.count = 0;

      for (auto [label, frame, alpha] : {std::tuple{carets.hide, 0u, 0x00u}, std::tuple{carets.show, 10u, 0xFFu}})
      {
        Wr32(key.data(), label);
        Wr32(key.data() + 4, frame);
        Wr32(key.data() + 8, carets.colour | alpha);
        WrF(key.data() + 0xC, cx - px - sx * (left0.w / 2 - px));
        WrF(key.data() + 0x10, c.cy - py - sy * (left0.h / 2 - py));
        WrF(key.data() + 0x20, c.rot);
        t.keys.insert(t.keys.end(), key.begin(), key.end());
        ++t.count;
      }

      s.tracks[c.h] = std::move(t);
      InsertAfter(s, kPageRow, c.h);
    }

    return Finish(s, rel, rel_size, out_dat, out_rel, error);
  }
}