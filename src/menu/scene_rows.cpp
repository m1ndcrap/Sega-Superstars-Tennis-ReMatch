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

  // Settings > GENERAL: a fifth row (see scene_patches.h)
  bool AddGeneralRow(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, const GeneralRow &row, Bytes &out_dat, Bytes &out_rel, std::string &error)
  {
    // The cursor group: the four row bars (bar, arrows, scissor, label) and the highlight (bar picture, pointer); its keys pick the highlighted row
    constexpr uint32_t kCursor = 0xAF90E2C8, kMarker = 0xEEFC6790, kPointer = 0x935E2B2E;
    constexpr uint32_t kBars[4] = {0xE2906172, 0x0995353F, 0xDBC25D8D, 0x171A6630};
    constexpr uint32_t kLabel3 = 0x0DD05B3A;                        // SPEED's label (in its bar)
    constexpr uint32_t kValues3 = 0x74D374C2, kValue3 = 0x9BCCFC3F; // SPEED's value row and the text it shows at value 0
    // Cursor timeline frames: row 2 highlighted at 61, row 3 at 70; the pointer fades out at 74 on its way back to row 0
    constexpr uint32_t kRow2Frame = 61, kRow3Frame = 70, kFadedFrame = 74, kIntroEnd = 43;
    constexpr uint32_t kFrom = 400, kFadeOut = 404, kFadeIn = 405, kTo = 409;
    Scene s;

    if (!Parse(dat, dat_size, s, error))
      return false;

    for (uint32_t h : {kCursor, kMarker, kPointer, kBars[0], kBars[1], kBars[2], kBars[3], kLabel3, kValues3, kValue3})
    {
      if (!s.objects.count(h) || !s.tracks.count(h))
        return error = fmt::format("object {:08X} missing", h), false;
    }

    for (uint32_t h : {row.bar, row.values, row.label, row.value})
    {
      if (s.used.count(h))
        return error = fmt::format("hash {:08X} in use (already added?)", h), false;
    }

    // A track's state at `frame` (the keys around it interpolated): a key to copy
    auto eval = [&](const Track &t, uint32_t frame) -> Bytes
    {
      const size_t n = t.KeySize();
      int a = -1, b = -1;

      for (int k = 0; k < t.count; ++k)
      {
        const uint32_t f = Rd32(t.keys.data() + k * n + 4);

        if (f <= frame && (a < 0 || f >= Rd32(t.keys.data() + a * n + 4)))
          a = k;

        if (f > frame && (b < 0 || f < Rd32(t.keys.data() + b * n + 4)))
          b = k;
      }

      if (a < 0)
        a = b;

      Bytes key(t.keys.data() + a * n, t.keys.data() + (a + 1) * n);

      if (b < 0 || b == a)
        return key;

      const uint8_t *kb = t.keys.data() + b * n;
      const float fa = float(Rd32(key.data() + 4)), fb = float(Rd32(kb + 4));
      const float u = fb > fa ? (float(frame) - fa) / (fb - fa) : 0.0f;

      for (size_t off : {0xCu, 0x10u, 0x14u, 0x18u})
        WrF(key.data() + off, RdF(key.data() + off) + u * (RdF(kb + off) - RdF(key.data() + off)));

      for (size_t c = 8; c < 12; ++c)
        key[c] = uint8_t(std::lround(key[c] + u * (int(kb[c]) - int(key[c]))));

      return key;
    };

    auto make = [](Bytes key, uint32_t label, uint32_t frame, float dy) -> Bytes
    {
      Wr32(key.data(), label);
      Wr32(key.data() + 4, frame);
      WrF(key.data() + 0x10, RdF(key.data() + 0x10) + dy);
      return key;
    };

    auto append = [](Track &t, const Bytes &key)
    {
      t.keys.insert(t.keys.end(), key.begin(), key.end());
      ++t.count;
    };

    auto max_frame = [](const Track &t)
    {
      uint32_t m = 0;

      for (int k = 0; k < t.count; ++k)
        m = std::max(m, Rd32(t.keys.data() + k * t.KeySize() + 4));

      return m;
    };

    std::function<void(uint32_t, std::vector<uint32_t> &)> subtree = [&](uint32_t h, std::vector<uint32_t> &out)
    {
      out.push_back(h);

      for (uint32_t c : s.objects[h].children)
      {
        if (s.objects.count(c))
          subtree(c, out);
      }
    };

    // Row spacing: where the highlight is for row 3 minus row 2
    const float dy = RdF(eval(s.tracks[kMarker], kRow3Frame).data() + 0x10) - RdF(eval(s.tracks[kMarker], kRow2Frame).data() + 0x10);

    if (!(dy > 0.05f && dy < 0.15f))
      return error = fmt::format("row spacing {:.3f}", dy), false;

    // The cursor group's subtree: kind-0 keys, all before the new frames
    std::vector<uint32_t> cursor_tree;
    subtree(kCursor, cursor_tree);

    for (uint32_t h : cursor_tree)
    {
      if (s.tracks.count(h) && (s.tracks[h].kind != 0 || max_frame(s.tracks[h]) >= kFrom))
        return error = fmt::format("cursor keys ({:08X})", h), false;
    }

    // Copies of SPEED's bar and value row (hashes: the row's for the four the page script uses, new ones for the rest)
    std::map<uint32_t, uint32_t> copies; // original -> copy

    std::function<uint32_t(uint32_t)> clone = [&](uint32_t from) -> uint32_t
    {
      const uint32_t to = from == kBars[3] ? row.bar : from == kValues3 ? row.values
                                                   : from == kLabel3    ? row.label
                                                   : from == kValue3    ? row.value
                                                                        : s.NewHash(from);

      s.used.insert(to);
      copies[from] = to;
      Object o = s.objects[from];
      o.order = s.next_order++;
      const bool at8 = o.type == Tag("TEXT") || o.type == Tag("TXTR");

      if (o.blob.size() >= (at8 ? 12u : 4u))
        Wr32(o.blob.data() + (at8 ? 8 : 0), to);

      std::vector<uint32_t> kids = o.children;
      o.children.clear();
      s.objects[to] = std::move(o);

      if (s.tracks.count(from))
      {
        Track t = s.tracks[from];
        t.order = s.next_order++;
        s.tracks[to] = std::move(t);
      }

      for (uint32_t k : kids)
      {
        if (!s.objects.count(k))
          continue;

        const uint32_t nk = clone(k);
        s.objects[to].children.push_back(nk);
        s.parent[nk] = to;
      }

      return to;
    };

    clone(kBars[3]);
    clone(kValues3);

    // The copies' scissor references (the value texts are clipped by the bar's scissor) point at the copies
    for (auto [from, to] : copies)
    {
      Object &o = s.objects[to];

      if ((o.type == Tag("TEXT") || o.type == Tag("TXTR")) && o.blob.size() >= 16)
      {
        if (auto it = copies.find(Rd32(o.blob.data() + 0xC)); it != copies.end())
          Wr32(o.blob.data() + 0xC, it->second);
      }
    }

    // The copies' keys, one row lower. The bar's parts: SPEED's slide-in keys, then normal (as SPEED with row 2 highlighted) up to the new stretch, highlighted (as SPEED at frame 70) at its end. The value row keeps its own timeline (values, highlight)
    for (auto [from, to] : copies)
    {
      if (!s.tracks.count(to) || from == kBars[3] || from == kValues3)
        continue;

      Track &t = s.tracks[to];
      const Track &orig = s.tracks[from];
      const bool bar_part = s.parent.count(from) && std::find(cursor_tree.begin(), cursor_tree.end(), from) != cursor_tree.end();

      if (!bar_part || orig.count < 2)
      {
        for (int k = 0; k < t.count; ++k)
          WrF(t.Key(k) + 0x10, RdF(t.Key(k) + 0x10) + dy);

        continue;
      }

      Track nt = t;
      nt.keys.clear();
      nt.count = 0;

      for (int k = 0; k < orig.count; ++k)
      {
        const Bytes key(orig.keys.data() + k * orig.KeySize(), orig.keys.data() + (k + 1) * orig.KeySize());

        if (Rd32(key.data() + 4) < kIntroEnd)
          append(nt, make(key, Rd32(key.data()), Rd32(key.data() + 4), dy));
      }

      const Bytes normal = eval(orig, kRow2Frame), lit = eval(orig, kRow3Frame);
      append(nt, make(normal, row.cursor_from, kIntroEnd, dy));
      append(nt, make(normal, row.cursor_from, kFrom, dy));
      append(nt, make(lit, row.cursor_to, kTo, dy));
      t = std::move(nt);
    }

    // The existing rows on the new stretch: rows 0-2 stay normal, SPEED goes from highlighted to normal; the highlight bar and pointer fade out on SPEED and in one row lower (as they do between rows)
    for (int r = 0; r < 4; ++r)
    {
      std::vector<uint32_t> tree;
      subtree(kBars[r], tree);

      for (uint32_t h : tree)
      {
        if (!s.tracks.count(h) || s.tracks[h].count < 2)
          continue;

        Track &t = s.tracks[h];
        const Bytes at70 = eval(t, kRow3Frame);
        append(t, make(at70, row.cursor_from, kFrom, 0));
        append(t, make(r == 3 ? eval(t, kRow2Frame) : at70, row.cursor_to, kTo, 0));
      }
    }

    for (uint32_t h : {kMarker, kPointer})
    {
      Track &t = s.tracks[h];
      const Bytes lit = eval(t, kRow3Frame), faded = eval(t, kFadedFrame);
      append(t, make(lit, row.cursor_from, kFrom, 0));
      append(t, make(faded, row.cursor_from, kFadeOut, 0));
      append(t, make(faded, row.cursor_to, kFadeIn, dy));
      append(t, make(lit, row.cursor_to, kTo, dy));
    }

    {
      Track &t = s.tracks[kCursor];
      const Bytes key(t.Key(0), t.Key(0) + t.KeySize());
      append(t, make(key, row.cursor_from, kFrom, 0));
      append(t, make(key, row.cursor_to, kTo, 0));
    }

    // In the scene: the bar after SPEED's (in the cursor group, under the highlight), the value row in the page's list
    InsertAfter(s, kBars[3], row.bar);
    InsertAfter(s, kValues3, row.values);

    return Finish(s, rel, rel_size, out_dat, out_rel, error);
  }
}