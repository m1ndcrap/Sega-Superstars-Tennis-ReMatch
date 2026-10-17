// Ultrawide layouts for SumoTool scenes: menus, loading screens and backgrounds made for 16:9 get layouts that fill a wider screen (bars widened, pictures continued with mirrored strips, backgrounds cropped, the main menu's swoosh stretched, more stars)

#include "menu/scene_format.h"
#include "menu/scene_patches.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>

#include <fmt/format.h>

namespace sst::menu::scenes
{
  using namespace scene;

  namespace
  {
    // The renderer treats a single quad spanning the 16:9 frame as a full-screen layer (stretched) and one touching exactly one side as an edge layer (that side stretched to the screen's edge), within 0.02 of clip space = 0.01 of the layout width. Pictures that get mirrored strips are pulled in by kInset so neither rule applies to them or their strips
    constexpr float kInset = 0.0125f;
    constexpr float kTouchWindow = 0.012f; // keep generated edges this far from 0 / 1
    constexpr float kEdge = 0.003f;        // "touches a side": within this of 0 / 1

    // The main menu's swoosh (SSTMainMenu, texture 1D74AF46: 713 x 720 px)
    constexpr uint32_t kSwooshTexture = 0x1D74AF46;
    constexpr float kSwooshSplit = 430.0f / 713.0f; // drawn as is up to here
    constexpr float kSwooshFlat = 3.0f / 713.0f;    // last columns: the flat band
    constexpr int kSwooshSlices = 16;               // the tail, progressively stretched
    constexpr float kSwooshGrow = 1.0f;             // tail at most (1 + grow) x as long

    // The x an object rests at: its most frequent keyframe x (ties: nearest 0)
    float RestX(const Track &t)
    {
      if (!t.count)
        return 0.0f;

      std::map<int64_t, int> votes;

      for (int k = 0; k < t.count; ++k)
        ++votes[std::llround(double(t.X(k)) * 10000.0)];

      int64_t best = 0;
      int best_n = -1;

      for (auto [x, n] : votes)
      {
        if (n > best_n || (n == best_n && std::llabs(x) < std::llabs(best)))
          best = x, best_n = n;
      }

      return float(double(best) / 10000.0);
    }

    bool ConstSx(const Track &t, float &sx)
    {
      if (!t.count)
        return false;

      sx = t.Sx(0);

      for (int k = 1; k < t.count; ++k)
      {
        if (std::fabs(t.Sx(k) - sx) > 1e-4f)
          return false;
      }

      return sx > 1e-4f;
    }

    // Absolute x of an object's origin (sum of its groups' rest positions), or nullopt if a group on the way is scaled
    std::optional<float> AbsOrigin(Scene &s, uint32_t h)
    {
      float x = 0.0f;
      int guard = 0;

      for (auto it = s.parent.find(h); it != s.parent.end() && guard < 64; it = s.parent.find(it->second), ++guard)
      {
        auto t = s.tracks.find(it->second);

        if (t == s.tracks.end())
          continue;

        float sx;

        if (ConstSx(t->second, sx) && std::fabs(sx - 1.0f) > 1e-3f)
          return std::nullopt;

        x += RestX(t->second);
      }

      return x;
    }

    void ShiftKeysX(Track &t, float dx)
    {
      for (int k = 0; k < t.count; ++k)
        WrF(t.Key(k) + 0xC, t.X(k) + dx);
    }

    // A copy of a TXTR object (new hash, texture `tex`, track `track`), placed after it
    uint32_t CloneTxtr(Scene &s, uint32_t from, uint32_t tex, Track track)
    {
      const uint32_t h = s.NewHash(from);
      Object o = s.objects[from];
      o.order = s.next_order++;
      Wr32(o.blob.data(), tex);
      Wr32(o.blob.data() + 8, h);
      s.objects[h] = std::move(o);
      track.order = s.next_order++;
      s.tracks[h] = std::move(track);
      InsertAfter(s, from, h);
      return h;
    }

    // A texture entry showing columns [a, b] of `tex` (fractions of its width; b < a mirrors)
    uint32_t SubTexture(Scene &s, uint32_t tex, float a, float b, std::map<std::tuple<uint32_t, int64_t, int64_t>, uint32_t> &cache)
    {
      const auto key = std::make_tuple(tex, std::llround(double(a) * 1e6), std::llround(double(b) * 1e6));

      if (auto it = cache.find(key); it != cache.end())
        return it->second;

      const Ptex &t = s.ptex.at(tex);
      const float u0 = t.uv[0], u1 = t.uv[2]; // top left, top right

      auto u = [&](float f)
      {
        return u0 + (u1 - u0) * f;
      };

      Ptex n = t;
      n.uv[0] = n.uv[6] = u(a); // top left, bottom left
      n.uv[2] = n.uv[4] = u(b); // top right, bottom right
      const uint32_t h = s.NewHash(tex);
      s.ptex[h] = n;
      cache[key] = h;
      return h;
    }

    // recipes
    struct Edge
    {
      uint32_t h;
      float x0, w, sx; // absolute left, width (layout fractions), key x scale
      bool left, right;
    };

    std::vector<Edge> EdgeObjects(Scene &s, uint32_t type)
    {
      std::vector<Edge> out;

      for (auto &[h, o] : s.objects)
      {
        if (o.type != type)
          continue;

        auto t = s.tracks.find(h);

        if (t == s.tracks.end() || !t->second.count)
          continue;

        float sx;

        if (!ConstSx(t->second, sx))
        {
          if (type != Tag("GORD"))
            continue;

          sx = t->second.Sx(0);

          if (sx <= 1e-4f)
            continue;
        }

        const auto origin = AbsOrigin(s, h);

        if (!origin)
          continue;

        const float x0 = *origin + RestX(t->second), w = t->second.w * sx;

        if (w <= 0.01f)
          continue;

        const bool left = std::fabs(x0) < kEdge, right = std::fabs(x0 + w - 1.0f) < kEdge;

        if (left || right)
          out.push_back({h, x0, w, sx, left, right});
      }

      return out;
    }

    // Solid bars / gradients: wider by `shift` on each side they touch
    int WidenBars(Scene &s, float shift)
    {
      int n = 0;

      for (const Edge &e : EdgeObjects(s, Tag("GORD")))
      {
        Track &t = s.tracks[e.h];
        const float add = shift * (float(e.left) + float(e.right));
        t.w *= (e.w + add) / e.w;

        if (e.left)
          ShiftKeysX(t, -shift);

        ++n;
      }

      return n;
    }

    // The main menu swoosh: its tail lengthened up to the screen's right edge
    bool Swoosh(Scene &s, const Edge &e, float shift, std::map<std::tuple<uint32_t, int64_t, int64_t>, uint32_t> &cache)
    {
      Object &o = s.objects[e.h];
      const uint32_t tex = Rd32(o.blob.data());
      const Track base = s.tracks[e.h];
      const float a = e.w * kSwooshSplit;            // drawn as is
      const float flat = e.w * kSwooshFlat;          // source of the flat band
      const float ts = e.w - a - flat;               // tail (source)
      const float end = 1.0f + shift + 0.01f - e.x0; // screen edge (relative), a bit beyond
      const float tail = std::min(ts * (1.0f + kSwooshGrow), end - a - 0.005f);

      if (tail < ts)
        return false;

      const float c = tail - ts;
      // Destination boundaries (relative to the swoosh's x): kept clear of the renderer's edge window around the 16:9 frame's right edge
      std::vector<float> src = {0.0f, a}, dst = {0.0f, a};

      for (int i = 1; i <= kSwooshSlices; ++i)
      {
        const float u = float(i) / kSwooshSlices;
        src.push_back(a + u * ts);
        dst.push_back(a + u * ts + c * u * u);
      }

      src.push_back(e.w);
      dst.push_back(end);

      for (float &d : dst)
      {
        const float abs_x = e.x0 + d;

        if (std::fabs(abs_x - 1.0f) < kTouchWindow)
          d = (abs_x < 1.0f ? 1.0f - kTouchWindow : 1.0f + kTouchWindow) - e.x0;
      }

      for (size_t i = 1; i < dst.size(); ++i)
      {
        if (dst[i] <= dst[i - 1])
          return false;
      }

      // Piece 0 is the original object; the rest are copies
      for (size_t i = 0; i + 1 < src.size(); ++i)
      {
        Track t = base;
        t.w = (dst[i + 1] - dst[i]) / e.sx;
        ShiftKeysX(t, dst[i]);
        const uint32_t sub = SubTexture(s, tex, src[i] / e.w, src[i + 1] / e.w, cache);

        if (i == 0)
        {
          s.tracks[e.h] = t;
          Wr32(o.blob.data(), sub);
        }
        else
        {
          CloneTxtr(s, e.h, sub, t);
        }
      }

      return true;
    }

    // A texture entry showing the part [a, b] x [c, d] of `tex` (fractions)
    uint32_t SubRect(Scene &s, uint32_t tex, float a, float b, float c, float d)
    {
      const Ptex &t = s.ptex.at(tex);
      const float u0 = t.uv[0], u1 = t.uv[2], v0 = t.uv[1], v1 = t.uv[5]; // top left, bottom right

      auto u = [&](float f)
      {
        return u0 + (u1 - u0) * f;
      };

      auto v = [&](float f)
      {
        return v0 + (v1 - v0) * f;
      };

      Ptex n = t;
      const float uv[8] = {u(a), v(c), u(b), v(c), u(b), v(d), u(a), v(d)};
      std::memcpy(n.uv, uv, sizeof(uv));
      const uint32_t h = s.NewHash(tex);
      s.ptex[h] = n;
      return h;
    }

    // A court loading background: as wide as the screen, scaled evenly, the top and bottom cropped. (The quad reaches past the 16:9 frame on both sides, so the renderer fits it like the rest of the layout: exactly the screen.)
    void CropToWidth(Scene &s, const Edge &e, float shift)
    {
      Object &o = s.objects[e.h];
      const uint32_t tex = Rd32(o.blob.data());
      const float scale = (e.w + 2 * shift) / e.w; // = screen width / 16:9 width
      const float keep = 1.0f / scale;             // fraction of the height shown
      Track &t = s.tracks[e.h];
      t.w = (e.w + 2 * shift) / e.sx;
      ShiftKeysX(t, -shift);
      Wr32(o.blob.data(), SubRect(s, tex, 0.0f, 1.0f, 0.5f - keep / 2, 0.5f + keep / 2));
    }

    bool IsCourtBackgroundScene(const std::string &name)
    {
      return name.size() == 16 && name.starts_with("bg_") && name.ends_with("loading.dat");
    }

    // Pictures touching a side: a mirrored strip beyond it
    void MirrorStrips(Scene &s, float shift, Stats &stats, std::map<std::tuple<uint32_t, int64_t, int64_t>, uint32_t> &cache, std::vector<std::string> *log, bool crop_background)
    {
      for (const Edge &e : EdgeObjects(s, Tag("TXTR")))
      {
        Object &o = s.objects[e.h];
        const uint32_t tex = Rd32(o.blob.data());

        if (!s.ptex.count(tex))
          continue;

        if (crop_background && e.left && e.right && e.w > 0.95f)
        {
          CropToWidth(s, e, shift);
          ++stats.cropped;

          if (log)
            log->push_back(fmt::format("background {:08X}: cropped to the screen's width", e.h));

          continue;
        }

        if (tex == kSwooshTexture && e.right && !e.left)
        {
          if (Swoosh(s, e, shift, cache))
          {
            ++stats.swoosh;

            if (log)
              log->push_back(fmt::format("swoosh {:08X}: tail to the screen edge", e.h));
          }

          continue;
        }

        if (e.w < 4 * kInset)
          continue;

        if (std::fabs(e.sx - 1.0f) > 1e-3f)
          continue; // scaled about a pivot: left as is

        const Track base = s.tracks[e.h];
        const float l = e.left ? kInset : 0.0f, r = e.right ? kInset : 0.0f;

        // The picture itself, pulled in from the sides it touches
        {
          Track &t = s.tracks[e.h];
          t.w = (e.w - l - r) / e.sx;

          if (e.left)
            ShiftKeysX(t, l);

          Wr32(o.blob.data(), SubTexture(s, tex, l / e.w, (e.w - r) / e.w, cache));
        }

        const float strip = shift + kInset;

        if (e.left)
        { // mirrored about x = inset
          Track t = base;
          t.w = strip / e.sx;
          ShiftKeysX(t, -shift);
          const float outer = std::min(e.w, 2 * kInset + shift);
          CloneTxtr(s, e.h, SubTexture(s, tex, outer / e.w, kInset / e.w, cache), t);
          ++stats.strips;
        }

        if (e.right)
        { // mirrored about x = width - inset
          Track t = base;
          t.w = strip / e.sx;
          ShiftKeysX(t, e.w - kInset);
          const float outer = std::max(0.0f, e.w - 2 * kInset - shift);
          CloneTxtr(s, e.h, SubTexture(s, tex, (e.w - kInset) / e.w, outer / e.w, cache), t);
          ++stats.strips;
        }
      }
    }

    // Star background: each evenly spaced row of star columns runs on further
    constexpr float kPanSpare = 0.6f; // layout widths of extra star columns per side

    // Scenes with the panning star field: the menus' background, and the white transition fade's own copy of it (drawn over everything while menus change)
    bool IsStarFieldScene(const std::string &name)
    {
      return name == "starbg.dat" || name.starts_with("whtfade");
    }

    int MoreStars(Scene &s, float shift, std::vector<std::string> *log)
    {
      int added = 0;
      std::vector<uint32_t> groups;
      std::map<uint32_t, uint32_t> parent;

      for (auto &[h, o] : s.objects)
      {
        if (o.type == Tag("GROP"))
        {
          groups.push_back(h);

          for (uint32_t c : o.children)
            parent[c] = h;
        }
      }

      // Where a group sits on the layout (its own and its parents' first x keys)
      auto layout_x = [&](uint32_t g)
      {
        float x = 0.0f;

        for (int depth = 0; depth < 16; ++depth)
        {
          auto t = s.tracks.find(g);

          if (t != s.tracks.end() && t->second.count)
            x += t->second.X(0);

          auto p = parent.find(g);

          if (p == parent.end())
            break;

          g = p->second;
        }

        return x;
      };

      for (uint32_t g : groups)
      {
        // Rows: same texture, y and width; one static x each
        std::map<std::tuple<uint32_t, int64_t, int64_t>, std::vector<std::pair<float, uint32_t>>> rows;

        for (uint32_t c : s.objects[g].children)
        {
          const Object &o = s.objects[c];
          auto t = s.tracks.find(c);

          if (o.type != Tag("TXTR") || t == s.tracks.end() || t->second.count != 1)
            continue;

          const float y = RdF(t->second.keys.data() + 0x10);
          rows[{Rd32(o.blob.data()), std::llround(double(y) * 1000.0), std::llround(double(t->second.w) * 10000.0)}].push_back({t->second.X(0), c});
        }

        for (auto &[key, cols] : rows)
        {
          // Full rows (6+ columns, the scrolling field) grow on both sides. Short rows (3-5: the main menu's field, which fades out towards the swoosh) grow only on a side where they reach the frame's edge
          if (cols.size() < 3)
            continue;

          const bool full_row = cols.size() >= 6;
          std::sort(cols.begin(), cols.end());
          std::vector<float> gaps;

          for (size_t i = 1; i < cols.size(); ++i)
            gaps.push_back(cols[i].first - cols[i - 1].first);

          std::vector<float> sorted = gaps;
          std::sort(sorted.begin(), sorted.end());
          const float d = sorted[sorted.size() / 2];

          if (d <= 0.01f)
            continue;

          size_t even = 0;

          for (float gp : gaps)
            even += std::fabs(gp - d) < 0.002f;

          if (even * 4 < gaps.size() * 3)
            continue; // not a regular row

          // Full rows also get spare columns beyond the screen: the field pans with its group, and during menu transitions it shows further along than its keyframes reach (a gap at the right edge, even at 16:9)
          const int more = int(std::ceil((shift + (full_row ? kPanSpare : 0.0f)) / d)) + 1;

          if (log)
            log->push_back(fmt::format("stars: group {:08X} row y {:.3f}: {} columns {:.3f}..{:.3f} (spacing {:.4f}), +{} per side", g, double(std::get<1>(key)) / 1000.0, cols.size(), cols.front().first, cols.back().first, d, more));

          const float gx = layout_x(g), w = float(std::get<2>(key)) / 10000.0f;

          for (int side = 0; side < 2; ++side)
          {
            const auto [x, from] = side == 0 ? cols.front() : cols.back();

            if (!full_row && (side == 0 ? gx + x > 0.02f : gx + x + w < 0.98f))
              continue;

            for (int k = 1; k <= more; ++k)
            {
              Track t = s.tracks[from];
              ShiftKeysX(t, (side == 0 ? -d : d) * float(k));
              CloneTxtr(s, from, Rd32(s.objects[from].blob.data()), t);
              ++added;
            }
          }
        }
      }

      return added;
    }
  }

  bool WantsWideLayout(const std::string &name)
  {
    static const char *kSkip[] = {"hud.dat", "hudicons.dat", "spechud.dat", "stadiumig.dat", "ingame.dat", "pausemenu.dat"};

    if (!name.ends_with(".dat"))
      return false;

    // Language files (<stem>_EN.dat ...) hold only texts and fonts
    static const char *kLangs[] = {"_en", "_fr", "_ge", "_it", "_jp", "_sp", "_us"};

    for (const char *l : kLangs)
    {
      if (name.size() > 7 && name.compare(name.size() - 7, 3, l) == 0)
        return false;
    }

    for (const char *n : kSkip)
    {
      if (name == n)
        return false;
    }

    return true;
  }

  bool Widen(const std::string &name, const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, float shift, Bytes &out_dat, Bytes &out_rel, Stats &stats, std::vector<std::string> *log, std::string &error)
  {
    stats = {};

    if (shift <= 0.0f && !IsStarFieldScene(name))
      return true; // (16:9: star fields only)

    shift = std::max(shift, 0.0f);
    {
      std::string ignored;
      auto chunks = ReadChunks(dat, dat_size, ignored);

      if (!chunks)
        return error = ignored, false;

      bool layout = false;

      for (const Chunk &c : *chunks)
        layout |= c.tag == Tag("SCNE");

      if (!layout)
        return true; // text / font tables only (language files)
    }

    Scene s;

    if (!Parse(dat, dat_size, s, error))
      return false;

    std::map<std::tuple<uint32_t, int64_t, int64_t>, uint32_t> cache;

    if (IsStarFieldScene(name))
      stats.stars = MoreStars(s, shift, log);

    stats.bars = WidenBars(s, shift);

    if (name != "starbg.dat")
      MirrorStrips(s, shift, stats, cache, log, IsCourtBackgroundScene(name));

    if (!stats.Total())
      return true;

    return Finish(s, rel, rel_size, out_dat, out_rel, error);
  }
}