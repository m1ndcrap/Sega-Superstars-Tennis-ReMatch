// sst_rematch - ultrawide HUD anchoring

#include "menu/hud_anchor.h"

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

    uint32_t Rd32(const uint8_t *p)
    {
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    float RdF(const uint8_t *p)
    {
      const uint32_t u = Rd32(p);
      float f;
      std::memcpy(&f, &u, 4);
      return f;
    }

    void WrF(uint8_t *p, float f)
    {
      uint32_t u;
      std::memcpy(&u, &f, 4);
      p[0] = uint8_t(u >> 24), p[1] = uint8_t(u >> 16), p[2] = uint8_t(u >> 8), p[3] = uint8_t(u);
    }

    constexpr uint32_t Tag(const char (&s)[5])
    {
      return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
    }

    struct Track
    {
      uint8_t *keys;
      uint16_t count, kind;
      float w;
      size_t KeySize() const { return kind == 2 ? 0x38 : 0x28; }
      float X() const { return count ? RdF(keys + 0xC) : 0.0f; }
    };

    struct Object
    {
      uint32_t type;
      std::vector<uint32_t> children;
    };

  }

  bool IsHudScene(const std::string &name)
  {
    static const char *kNames[] = {"hud.dat", "hudicons.dat", "spechud.dat", "stadiumig.dat",
                                   "ingame.dat"};
    for (const char *n : kNames)
    {
      if (name == n)
        return true;
    }

    return false;
  }

  int Anchor(uint8_t *d, size_t n, float shift, std::vector<std::string> *log, bool sides)
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
        Track t{kfrm + Rd32(e + 4), uint16_t((e[8] << 8) | e[9]), uint16_t((e[10] << 8) | e[11]), RdF(e + 12)};

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

    int moved = 0;
    const uint32_t top = Rd32(scne + 8), list = Rd32(scne + 0x10);

    if (list + 4 * size_t(top) > scne_size)
      return 0;

    for (uint32_t i = 0; i < top; ++i)
    {
      const uint32_t h = Rd32(scne + list + 4 * i);
      float lo = 1e9f, hi = -1e9f;
      extent(h, 0.0f, 0, lo, hi);
      auto t = tracks.find(h);

      if (lo > hi || t == tracks.end())
        continue;

      const float centre = 0.5f * (lo + hi);
      // Elements spanning most of the width (backgrounds, banners) stay centred.
      const float side = !sides || (hi - lo) > 0.6f ? 0.0f : centre < 0.42f ? -1.0f
                                                         : centre > 0.58f   ? 1.0f
                                                                            : 0.0f;
      if (log)
        log->push_back(fmt::format("{:08X} x {:.3f}-{:.3f} -> {}", h, lo, hi, side < 0 ? "left" : side > 0 ? "right"
                                                                                                           : "centre"));

      if (side == 0.0f)
      {
        // Centred elements that wait outside the 16:9 frame (banners that slide in, e.g. POINT / OUT / DOUBLE FAULT) would show in the extra width: keyframes that are off the frame move off the wider screen too
        int parked = 0;

        std::function<void(uint32_t, float, int)> park = [&](uint32_t c, float parent_x, int depth)
        {
          auto o = objects.find(c);

          if (o == objects.end() || depth > 16)
            return;

          auto ct = tracks.find(c);
          const float x0 = ct != tracks.end() ? ct->second.X() : 0.0f;
          float rlo = 1e9f, rhi = -1e9f; // extent relative to the object's own position
          extent(c, parent_x, depth, rlo, rhi);

          if (ct != tracks.end() && rlo <= rhi)
          {
            rlo -= parent_x + x0, rhi -= parent_x + x0;
            Track &tr = ct->second;

            for (uint16_t k = 0; k < tr.count; ++k)
            {
              uint8_t *key = tr.keys + k * tr.KeySize();
              const float x = RdF(key + 0xC);

              if (parent_x + x + rlo >= 0.98f)
              {
                WrF(key + 0xC, x + shift), ++parked;
              }
              else if (parent_x + x + rhi <= 0.02f)
              {
                WrF(key + 0xC, x - shift), ++parked;
              }
            }
          }

          // Children only under a group that stays put: under a group that slides, the group's own keyframes do the parking (its first key may be off-screen while the children belong on screen)
          bool still = true;

          if (ct != tracks.end())
          {
            for (uint16_t k = 1; k < ct->second.count; ++k)
            {
              if (std::fabs(RdF(ct->second.keys + k * ct->second.KeySize() + 0xC) - x0) > 1e-4f)
                still = false;
            }
          }

          if (o->second.type == Tag("GROP") && still)
          {
            for (uint32_t k : o->second.children)
              park(k, parent_x + x0, depth + 1);
          }
        };

        park(h, 0.0f, 0);

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