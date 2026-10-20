// Ultrawide HUD anchoring

#include "menu/hud_anchor.h"
#include "menu/be_bytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

#include <fmt/format.h>

namespace sst::menu::hud
{
  namespace
  {
    struct Track
    {
      uint8_t *keys;
      uint16_t count, kind;
      float w, h = 0.0f;

      size_t KeySize() const { return kind == 2 ? 0x38 : kind == 1 ? 0x64
                                                                   : 0x28; } // 1: PART

      float X() const { return count ? RdF(keys + 0xC) : 0.0f; }
      float Y() const { return count ? RdF(keys + 0x10) : 0.0f; }
    };

    struct Object
    {
      uint32_t type;
      std::vector<uint32_t> children;
      float pivot_x = 0.0f; // TXTR: point the picture scales about (relative to its x)
    };
  }

  bool IsHudScene(const std::string &name)
  {
    static const char *kNames[] = {"hud.dat", "hudicons.dat", "spechud.dat", "stadiumig.dat", "ingame.dat"};

    for (const char *n : kNames)
    {
      if (name == n)
        return true;
    }

    return false;
  }

  bool IsEdgeHudScene(const std::string &name)
  {
    return name == "bghuds.dat";
  }

  bool IsSpreadScene(const std::string &name)
  {
    return name == "loading.dat" || name == "charselect.dat";
  }

  namespace
  {
    int Run(uint8_t *d, size_t n, float shift, std::vector<std::string> *log, bool sides, bool edge_only, bool spread);
  }

  int Anchor(uint8_t *d, size_t n, float shift, std::vector<std::string> *log, bool sides, bool edge_only)
  {
    return Run(d, n, shift, log, sides, edge_only, false);
  }

  int Spread(uint8_t *d, size_t n, float shift, std::vector<std::string> *log)
  {
    return Run(d, n, shift, log, false, false, true);
  }

  namespace
  {
    int Run(uint8_t *d, size_t n, float shift, std::vector<std::string> *log, bool sides, bool edge_only, bool spread)
    {
      if (shift <= 0.0f)
        return 0;

      // Chunks
      uint8_t *kfrm = nullptr, *objs = nullptr, *scne = nullptr;
      size_t kfrm_size = 0, objs_size = 0, scne_size = 0;

      for (size_t o = 0; o + 16 <= n;)
      {
        const uint32_t tag = Rd32(d + o), csize = Rd32(d + o + 4), dsize = Rd32(d + o + 8);

        if (!tag || csize < 16 || o + 16 + dsize > n)
          break;

        if (tag == Tag("KFRM"))
          kfrm = d + o + 16, kfrm_size = dsize;

        if (tag == Tag("OBJS"))
          objs = d + o + 16, objs_size = dsize;

        if (tag == Tag("SCNE"))
          scne = d + o + 16, scne_size = dsize;

        o += csize;
      }

      if (!kfrm || !objs || !scne || kfrm_size < 4 || objs_size < 12 || scne_size < 0x1C)
        return 0;

      std::map<uint32_t, Track> tracks;
      {
        const uint32_t count = Rd32(kfrm);

        if (4 + size_t(count) * 0x14 > kfrm_size)
          return 0;

        for (uint32_t i = 0; i < count; ++i)
        {
          const uint8_t *e = kfrm + 4 + 0x14 * i;
          Track t{kfrm + Rd32(e + 4), uint16_t((e[8] << 8) | e[9]), uint16_t((e[10] << 8) | e[11]), RdF(e + 12), RdF(e + 16)};

          if (size_t(t.keys - kfrm) + t.count * t.KeySize() > kfrm_size)
            return 0;

          tracks[Rd32(e)] = t;
        }
      }

      std::map<uint32_t, Object> objects;
      {
        const uint32_t count = Rd32(objs + 8);

        if (12 + size_t(count) * 16 > objs_size)
          return 0;

        for (uint32_t i = 0; i < count; ++i)
        {
          const uint8_t *e = objs + 12 + 16 * i;
          const uint32_t off = Rd32(e + 8);
          Object o{Rd32(e + 4), {}};

          if (o.type == Tag("TXTR") && off + 0x34 <= objs_size)
            o.pivot_x = RdF(objs + off + 0x2C);

          if (o.type == Tag("GROP") && off + 0x1C <= objs_size)
          {
            const uint32_t kids = Rd32(objs + off + 0x14), list = Rd32(objs + off + 0x18);

            if (list + 4 * size_t(kids) <= objs_size)
            {
              for (uint32_t c = 0; c < kids; ++c)
                o.children.push_back(Rd32(objs + list + 4 * c));
            }
          }

          objects[Rd32(e)] = std::move(o);
        }
      }

      // Horizontal extent of an object (fractions of the layout width): TXTR quads span [x, x + w], texts count as their anchor point, groups add their own offset to their children's
      std::function<void(uint32_t, float, int, float &, float &)> extent = [&](uint32_t h, float parent_x, int depth, float &lo, float &hi)
      {
        auto o = objects.find(h);

        if (o == objects.end() || depth > 16)
          return;

        auto t = tracks.find(h);
        const float x = parent_x + (t != tracks.end() ? t->second.X() : 0.0f);

        if (o->second.type == Tag("TXTR"))
        {
          const float w = t != tracks.end() ? std::fabs(t->second.w) : 0.0f;
          lo = std::min(lo, x), hi = std::max(hi, x + w);
        }
        else if (o->second.type == Tag("GORD"))
        { // solid rectangle: w x key scale
          const float w = t != tracks.end() && t->second.count ? std::fabs(t->second.w * RdF(t->second.keys + 0x14)) : 0.0f;
          lo = std::min(lo, x), hi = std::max(hi, x + w);
        }
        else if (o->second.type == Tag("TEXT"))
        {
          lo = std::min(lo, x), hi = std::max(hi, x);
        }
        else if (o->second.type == Tag("GROP"))
        {
          for (uint32_t c : o->second.children)
            extent(c, x, depth + 1, lo, hi);
        }
      };

      // The pictures of an object as boxes {x0, x1, y0, y1} (first keyframes, as `extent`): TXTR quads and GORD rectangles
      struct Box
      {
        float x0, x1, y0, y1;
      };

      std::function<void(uint32_t, float, float, int, std::vector<Box> &)> boxes = [&](uint32_t h, float parent_x, float parent_y, int depth, std::vector<Box> &out)
      {
        auto o = objects.find(h);

        if (o == objects.end() || depth > 16)
          return;

        auto t = tracks.find(h);
        const bool keyed = t != tracks.end() && t->second.count;
        const float x = parent_x + (keyed ? t->second.X() : 0.0f), y = parent_y + (keyed ? t->second.Y() : 0.0f);

        if (keyed && (o->second.type == Tag("TXTR") || o->second.type == Tag("GORD")))
        {
          const bool gord = o->second.type == Tag("GORD");
          const float w = std::fabs(t->second.w * (gord ? RdF(t->second.keys + 0x14) : 1.0f));
          const float hh = std::fabs(t->second.h * (gord ? RdF(t->second.keys + 0x18) : 1.0f));
          out.push_back({x, x + w, y, y + hh});
        }
        else if (o->second.type == Tag("GROP"))
        {
          for (uint32_t c : o->second.children)
            boxes(c, x, y, depth + 1, out);
        }
      };

      // Does the object hold a text centred on the frame (x = 0.5: the game centres texts on their x)? Banners like CHANGE COURT are a centred text with sparkles whose first keys sit left of it, which made their extent look like a left-side element
      std::function<bool(uint32_t, float, int)> centred_text = [&](uint32_t h, float parent_x, int depth)
      {
        auto o = objects.find(h);

        if (o == objects.end() || depth > 16)
          return false;

        auto t = tracks.find(h);
        const float x = parent_x + (t != tracks.end() ? t->second.X() : 0.0f);

        if (o->second.type == Tag("TEXT"))
          return std::fabs(x - 0.5f) < 0.005f;

        if (o->second.type == Tag("GROP"))
        {
          for (uint32_t c : o->second.children)
          {
            if (centred_text(c, x, depth + 1))
              return true;
          }
        }

        return false;
      };

      // Parking sizes: an object's extent relative to its own position, from its pictures (TXTR / GORD; texts only when there are no pictures), each part counted at its resting place (the keyframe nearest the frame)
      struct Ext
      {
        float lo = 1e9f, hi = -1e9f;
        bool Valid() const { return lo <= hi; }
        void Add(float a, float b) { lo = std::min(lo, a), hi = std::max(hi, b); }
      };

      std::map<uint32_t, std::pair<Ext, Ext>> sizes; // object -> (pictures, texts)
      std::function<std::pair<Ext, Ext>(uint32_t, int)> sizes_of;
      std::function<float(uint32_t, const Ext &)> RestX = [&](uint32_t h, const Ext &e)
      {
        auto t = tracks.find(h);

        if (t == tracks.end() || !t->second.count)
          return 0.0f;

        float rest = 0.0f, best = 1e9f, best_abs = 1e9f;

        for (uint16_t k = 0; k < t->second.count; ++k)
        {
          const float x = RdF(t->second.keys + k * t->second.KeySize() + 0xC);
          const float lo = x + (e.Valid() ? e.lo : 0.0f), hi = x + (e.Valid() ? e.hi : 0.0f);
          const float out = std::max({0.0f, -hi, lo - 1.0f});

          if (out < best - 1e-5f || (std::fabs(out - best) <= 1e-5f && std::fabs(x) < best_abs))
            best = out, best_abs = std::fabs(x), rest = x;
        }

        return rest;
      };

      sizes_of = [&](uint32_t h, int depth) -> std::pair<Ext, Ext>
      {
        if (auto it = sizes.find(h); it != sizes.end())
          return it->second;

        std::pair<Ext, Ext> r;
        auto o = objects.find(h);

        if (o == objects.end() || depth > 16)
          return r;

        auto t = tracks.find(h);

        if (o->second.type == Tag("TXTR") || o->second.type == Tag("GORD"))
        {
          if (t != tracks.end() && t->second.count)
          {
            // A picture scaled by sx about its pivot: left = x + pivot (1 - sx)
            const float sx = RdF(t->second.keys + 0x14);
            const float w = t->second.w * sx;
            const float left = o->second.pivot_x * (1.0f - sx);
            r.first.Add(left + std::min(0.0f, w), left + std::max(0.0f, w));
          }
        }
        else if (o->second.type == Tag("TEXT"))
        {
          r.second.Add(0.0f, 0.0f);
        }
        else if (o->second.type == Tag("GROP"))
        {
          for (uint32_t c : o->second.children)
          {
            const auto cs = sizes_of(c, depth + 1);
            const float x = RestX(c, cs.first.Valid() ? cs.first : cs.second);

            if (cs.first.Valid())
              r.first.Add(x + cs.first.lo, x + cs.first.hi);

            if (cs.second.Valid())
              r.second.Add(x + cs.second.lo, x + cs.second.hi);
          }
        }

        sizes[h] = r;
        return r;
      };

      auto Size = [&](uint32_t h)
      {
        const auto s = sizes_of(h, 0);
        return s.first.Valid() ? s.first : s.second;
      };

      // Top-level objects of every scene in the file: SCNE is u32 count, then {hash, object count, 0, list offset, width, height} per scene
      std::vector<uint32_t> tops;

      std::map<uint32_t, std::vector<uint32_t>> scenes_of; // top-level object -> the scenes listing it
      {
        const uint32_t scenes = Rd32(scne);

        if (4 + 24 * size_t(scenes) > scne_size)
          return 0;

        for (uint32_t s = 0; s < scenes; ++s)
        {
          const uint8_t *e = scne + 4 + 24 * s;
          const uint32_t count = Rd32(e + 4), list = Rd32(e + 12);

          if (list + 4 * size_t(count) > scne_size)
            return 0;

          for (uint32_t i = 0; i < count; ++i)
          {
            const uint32_t h = Rd32(scne + list + 4 * i);
            scenes_of[h].push_back(s);

            if (std::find(tops.begin(), tops.end(), h) == tops.end())
              tops.push_back(h);
          }
        }
      }

      int moved = 0;

      if (spread)
      {
        // Moves one element (at `base`: its parent's place) by its centre's distance from the frame's centre, times 2 x shift. Returns whether it moved
        auto spread_one = [&](uint32_t h, float base, bool child) -> bool
        {
          auto t = tracks.find(h);

          if (t == tracks.end() || !t->second.count)
            return false;

          const Ext e = Size(h);

          if (!e.Valid())
            return false;

          const float rest = RestX(h, e), lo = base + rest + e.lo, hi = base + rest + e.hi;

          // (Backgrounds and bars spanning the frame stay; so do elements off the frame at rest, and ones laid out at the frame's corner, which the game places at run time - the VS screen's speech bubbles)
          if (hi - lo > 0.6f || hi < 0.0f || lo > 1.0f || 0.5f * (lo + hi) < 0.08f)
            return false;

          Track &tr = t->second;
          const float mid = base + 0.5f * (e.lo + e.hi);

          // (Cursors stepping over a grid that stays - character select's player cursors, one key per portrait - stay with the grid)
          {
            std::vector<float> xs;

            for (uint16_t k = 0; k < tr.count; ++k)
            {
              const float c = RdF(tr.keys + k * tr.KeySize() + 0xC) + mid;

              if (c >= 0.0f && c <= 1.0f && std::none_of(xs.begin(), xs.end(), [&](float v)
                                                         { return std::fabs(v - c) < 0.002f; }))
                xs.push_back(c);
            }

            if (xs.size() > 4)
              return false;
          }

          // Each keyframe by its own centre (an element moving between the players' places spreads with each place); keys off the frame - a slide in from the side - move with the rest place, so the slide keeps its shape
          const float rest_delta = (rest + mid - 0.5f) * 2.0f * shift;

          for (uint16_t k = 0; k < tr.count; ++k)
          {
            uint8_t *key = tr.keys + k * tr.KeySize();
            const float x = RdF(key + 0xC);
            const float c = x + mid;
            WrF(key + 0xC, x + (c >= 0.0f && c <= 1.0f ? (c - 0.5f) * 2.0f * shift : rest_delta));
          }

          ++moved;

          if (log)
            log->push_back(fmt::format("{}{:08X} at rest {:.3f}-{:.3f}: spread by {:+.3f} ({} keys)", child ? "  " : "", h, lo, hi, rest_delta, tr.count));

          return true;
        };

        for (const uint32_t h : tops)
        {
          if (spread_one(h, 0.0f, false))
            continue;

          // A group laid across the frame holding one group per player's place (character select's name banners: 2 or 4, each sliding in on its own) spreads place by place
          auto o = objects.find(h);
          auto t = tracks.find(h);

          if (o == objects.end() || o->second.type != Tag("GROP") || t == tracks.end() || !t->second.count)
            continue;

          const auto &kids = o->second.children;

          if (kids.size() < 2 || kids.size() > 4 || std::any_of(kids.begin(), kids.end(), [&](uint32_t c)
                                                                { auto co = objects.find(c); return co == objects.end() || co->second.type != Tag("GROP"); }))
            continue;

          const Ext e = Size(h);

          if (!e.Valid() || e.hi - e.lo <= 0.6f)
            continue;

          const float base = RestX(h, e);

          for (const uint32_t c : kids)
            spread_one(c, base, true);
        }

        return moved;
      }

      // Sides first: elements spanning most of the width (backgrounds, banners) stay centred, the rest go by the centre of their extent. Edge-only scenes: the element must also reach the outer part of its side, or overlap an element that moves to that side (one gauge drawn as several elements moves as one: the Puyo Puyo fever meter's spheres)
      struct Placed
      {
        float lo = 1e9f, hi = -1e9f;
        float by_centre = 0.0f, side = 0.0f;
        bool runtime = false; // laid out about the layout's origin: the game places it at run time
        std::vector<Box> pictures;
      };

      std::map<uint32_t, Placed> placed;

      for (const uint32_t h : tops)
      {
        Placed p;
        extent(h, 0.0f, 0, p.lo, p.hi);
        boxes(h, 0.0f, 0.0f, 0, p.pictures);

        if (p.lo > p.hi || tracks.find(h) == tracks.end())
          continue;

        const float centre = 0.5f * (p.lo + p.hi);

        // Markers laid out about the origin (x 0, partly off the frame) are placed by the game at run time, over the 3D scene: Space Harrier's target reticle, score pop-ups over a hit. Moving them to a side would take them off what they mark
        p.runtime = sides && std::fabs(centre) < 0.02f && p.hi - p.lo < 0.2f;
        p.by_centre = p.runtime ? 0.0f : !sides || (p.hi - p.lo) > 0.6f || centred_text(h, 0.0f, 0) ? 0.0f
                                     : centre < 0.42f                                               ? -1.0f
                                     : centre > 0.58f                                               ? 1.0f
                                                                                                    : 0.0f;
        p.side = edge_only && ((p.by_centre < 0.0f && p.lo > 0.15f) || (p.by_centre > 0.0f && p.hi < 0.85f)) ? 0.0f : p.by_centre;
        placed[h] = p;
      }

      if (edge_only)
      {
        // (Elements of the same scene only: a file holds many layouts drawn at different times)
        auto same_scene = [&](uint32_t a, uint32_t b)
        {
          for (uint32_t sa : scenes_of[a])
          {
            for (uint32_t sb : scenes_of[b])
            {
              if (sa == sb)
                return true;
            }
          }

          return false;
        };

        // Two elements overlap when one of their pictures does (not their bounding boxes: a sparse group would reach decorations in the middle)
        auto overlap = [](const Placed &a, const Placed &b)
        {
          constexpr float kTouch = 0.002f;

          for (const Box &p : a.pictures)
          {
            for (const Box &q : b.pictures)
            {
              if (p.x0 <= q.x1 + kTouch && q.x0 <= p.x1 + kTouch && p.y0 <= q.y1 + kTouch && q.y0 <= p.y1 + kTouch)
                return true;
            }
          }

          return false;
        };

        for (bool changed = true; changed;)
        {
          changed = false;

          for (auto &[h, p] : placed)
          {
            if (p.side != 0.0f || p.by_centre == 0.0f)
              continue;

            for (const auto &[h2, q] : placed)
            {
              // (and lies within the moving element's width: a big decoration drawn over a gauge stays)
              const bool within = p.lo >= q.lo - 0.01f && p.hi <= q.hi + 0.01f;

              if (q.side == p.by_centre && within && overlap(p, q) && same_scene(h, h2))
              {
                p.side = p.by_centre;
                changed = true;
                break;
              }
            }
          }
        }
      }

      for (const uint32_t h : tops)
      {
        auto pl = placed.find(h);
        auto t = tracks.find(h);

        if (pl == placed.end() || t == tracks.end())
          continue;

        const float lo = pl->second.lo, hi = pl->second.hi, side = pl->second.side;

        if (pl->second.runtime)
        {
          if (log)
            log->push_back(fmt::format("{:08X} x {:.3f}-{:.3f} -> placed at run time (stays)", h, lo, hi));

          continue;
        }

        if (log)
        {
          log->push_back(fmt::format("{:08X} x {:.3f}-{:.3f} -> {}", h, lo, hi, side < 0 ? "left" : side > 0 ? "right"
                                                                                                             : "centre"));
        }

        if (side == 0.0f)
        {
          // Centred elements that wait outside the 16:9 frame (banners that slide in, e.g. POINT / OUT / DOUBLE FAULT) would show in the extra width: keyframes that are off the frame move off the wider screen too
          int parked = 0;

          // park(object, where its group rests, every x its group can be at): a keyframe that is off the frame where the group rests moves off the wider screen, if the object also has a keyframe on screen there (it slides in: an unused 2P / 3P banner), or if that keyframe is off the frame wherever the group goes (it simply waits off-screen). Objects that only come on screen as their group slides (a second page) stay. Sizes come from pictures (texts, often blank, only when a group has no pictures), each part at its own resting place. A part that waits off-screen only counts as waiting when its whole group is off that side as well: a group that straddles the edge is one picture cut by it (the MsgBox loading ring sits in the top-left corner until the game moves it next to a player), not a stray
          std::function<void(uint32_t, float, const std::vector<float> &, Ext, int)> park = [&](uint32_t c, float rest_x, const std::vector<float> &group_xs, Ext parent, int depth)
          {
            auto o = objects.find(c);

            if (o == objects.end() || depth > 16)
              return;

            auto ct = tracks.find(c);
            Ext e = Size(c);
            std::vector<float> xs; // this object's keyframe x values

            if (ct != tracks.end())
            {
              for (uint16_t k = 0; k < ct->second.count; ++k)
                xs.push_back(RdF(ct->second.keys + k * ct->second.KeySize() + 0xC));
            }
            else
            {
              xs.push_back(0.0f);
            }

            if (ct != tracks.end() && e.Valid())
            {
              auto off_right = [&](float abs_x)
              {
                return abs_x + e.lo >= 0.98f;
              };

              auto off_left = [&](float abs_x)
              {
                return abs_x + e.hi <= 0.02f;
              };

              bool on_at_rest = false;

              for (float x : xs)
                on_at_rest |= !off_right(rest_x + x) && !off_left(rest_x + x);

              Track &tr = ct->second;

              for (uint16_t k = 0; k < tr.count; ++k)
              {
                const float x = xs[k];

                for (int dir : {1, -1})
                {
                  auto off = [&](float a)
                  {
                    return dir > 0 ? off_right(a + x) : off_left(a + x);
                  };

                  if (!off(rest_x))
                    continue;

                  bool always = true;

                  for (float g : group_xs)
                    always &= off(g);

                  if (parent.Valid())
                    always &= dir > 0 ? parent.lo >= 0.98f : parent.hi <= 0.02f;

                  if (on_at_rest || always)
                  {
                    WrF(tr.keys + k * tr.KeySize() + 0xC, x + float(dir) * shift);
                    ++parked;
                  }

                  break;
                }
              }
            }

            if (o->second.type != Tag("GROP"))
              return;

            const float rest = RestX(c, e);
            std::vector<float> child_xs;

            for (float g : group_xs)
            {
              for (float x : xs)
              {
                if (child_xs.size() < 64)
                  child_xs.push_back(g + x);
              }
            }

            Ext here; // this group at rest, on the layout

            if (e.Valid())
              here.Add(rest_x + rest + e.lo, rest_x + rest + e.hi);

            for (uint32_t k : o->second.children)
              park(k, rest_x + rest, child_xs, here, depth + 1);
          };

          park(h, 0.0f, std::vector<float>{0.0f}, Ext{}, 0);

          if (parked && log)
            log->push_back(fmt::format("{:08X}   {} off-screen keyframes moved out", h, parked));

          continue;
        }

        Track &tr = t->second;

        for (uint16_t k = 0; k < tr.count; ++k)
        {
          uint8_t *key = tr.keys + k * tr.KeySize();
          WrF(key + 0xC, RdF(key + 0xC) + side * shift);
        }

        ++moved;
      }

      return moved;
    }
  }
}