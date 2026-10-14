// sst_rematch - ultrawide layouts for SumoTool scenes

#include "menu/scene_widen.h"

#include <algorithm>
#include <tuple>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include <fmt/format.h>

namespace sst::menu::widen
{
  namespace
  {
    // big-endian helpers
    uint32_t Rd32(const uint8_t *p)
    {
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    uint16_t Rd16(const uint8_t *p)
    {
      return uint16_t((p[0] << 8) | p[1]);
    }

    void Wr32(uint8_t *p, uint32_t v)
    {
      p[0] = uint8_t(v >> 24);
      p[1] = uint8_t(v >> 16);
      p[2] = uint8_t(v >> 8);
      p[3] = uint8_t(v);
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
      Wr32(p, u);
    }

    void Put32(Bytes &b, uint32_t v)
    {
      const size_t at = b.size();
      b.resize(at + 4);
      Wr32(b.data() + at, v);
    }

    void PutF(Bytes &b, float f)
    {
      uint32_t u;
      std::memcpy(&u, &f, 4);
      Put32(b, u);
    }

    constexpr uint32_t Tag(const char (&s)[5])
    {
      return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
    }

    // tuning
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

    // scene model
    struct Chunk
    {
      uint32_t tag, magic;
      Bytes data;
    };

    struct Track
    {
      uint16_t count = 0, kind = 0;
      float w = 0, h = 0;
      Bytes keys;
      uint32_t order = 0; // position of the keys in the file (new tracks go last)
      // Keyframe sizes: kind 0 0x28, kind 2 0x38, kind 1 (particles) 0x64
      size_t KeySize() const { return kind == 2 ? 0x38 : kind == 1 ? 0x64
                                                                   : 0x28; }
      uint8_t *Key(int k) { return keys.data() + size_t(k) * KeySize(); }
      float X(int k) const { return RdF(keys.data() + size_t(k) * KeySize() + 0xC); }
      float Sx(int k) const { return RdF(keys.data() + size_t(k) * KeySize() + 0x14); }
    };

    struct Object
    {
      uint32_t type = 0;
      Bytes blob;                     // GROP: header only (0x1C bytes)
      std::vector<uint32_t> children; // GROP
      uint32_t order = 0;             // position in the file (new objects go last)
    };

    struct Ptex
    {
      float uv[8]; // TL, TR, BR, BL
      uint32_t page;
    };

    struct SceneEntry
    {
      uint32_t hash, list_at, zero; // list_at: where the list was (lists keep their order)
      float w, h;
      std::vector<uint32_t> list;
    };

    struct Scene
    {
      std::vector<Chunk> chunks;
      std::map<uint32_t, Track> tracks;
      std::map<uint32_t, Object> objects;
      std::map<uint32_t, Ptex> ptex;
      // Texture pages: data offset in the .GPU file and the 0x34-byte header (D3D resource header, then the fetch constant at +0x1C)
      struct Page
      {
        uint32_t data_offset;
        Bytes header;
      };

      std::vector<Page> pages;
      std::vector<SceneEntry> scenes;
      std::map<uint32_t, uint32_t> parent; // object -> its group
      uint32_t next_order = 0x7FFF0000;
      std::set<uint32_t> used;

      Chunk *Find(uint32_t tag)
      {
        for (Chunk &c : chunks)
          if (c.tag == tag)
            return &c;

        return nullptr;
      }

      uint32_t NewHash(uint32_t seed)
      {
        uint32_t h = seed ^ 0x57494445u; // "WIDE"
        do
        {
          h = h * 0x9E3779B1u + 0x7F4A7C15u;
          h ^= h >> 15;
        } while (!h || used.count(h));
        used.insert(h);
        return h;
      }
    };

    std::optional<std::vector<Chunk>> ReadChunks(const uint8_t *d, size_t n, std::string &error)
    {
      std::vector<Chunk> out;
      size_t o = 0;

      while (o + 16 <= n)
      {
        const uint32_t tag = Rd32(d + o);

        if (!tag)
          break;

        const uint32_t csize = Rd32(d + o + 4), dsize = Rd32(d + o + 8);

        if (csize < 16 || o + 16 + dsize > n || dsize + 16 > csize)
        {
          error = "bad chunk header";
          return std::nullopt;
        }

        out.push_back({tag, Rd32(d + o + 12), Bytes(d + o + 16, d + o + 16 + dsize)});
        o += csize;
      }

      return out;
    }

    Bytes WriteChunks(const std::vector<Chunk> &chunks)
    {
      Bytes out;

      for (const Chunk &c : chunks)
      {
        const uint32_t csize = (16 + uint32_t(c.data.size()) + 0x3F) & ~0x3Fu;
        Put32(out, c.tag);
        Put32(out, csize);
        Put32(out, uint32_t(c.data.size()));
        Put32(out, c.magic);
        out.insert(out.end(), c.data.begin(), c.data.end());
        out.resize(out.size() + (csize - 16 - c.data.size()), 0);
      }

      return out;
    }

    std::map<uint32_t, Bytes> SplitBlobs(const Bytes &data, const std::vector<uint32_t> &offsets, size_t start, bool &ok)
    {
      std::vector<uint32_t> sorted = offsets;
      std::sort(sorted.begin(), sorted.end());
      sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
      std::map<uint32_t, Bytes> out;
      ok = true;

      for (size_t i = 0; i < sorted.size(); ++i)
      {
        const uint32_t a = sorted[i];
        const uint32_t b = i + 1 < sorted.size() ? sorted[i + 1] : uint32_t(data.size());

        if (a < start || b < a || b > data.size())
        {
          ok = false;
          return {};
        }

        out[a] = Bytes(data.begin() + a, data.begin() + b);
      }

      return out;
    }

    bool Parse(const uint8_t *data, size_t size, Scene &s, std::string &error)
    {
      auto chunks = ReadChunks(data, size, error);

      if (!chunks)
        return false;

      s.chunks = std::move(*chunks);
      Chunk *kfrm = s.Find(Tag("KFRM"));
      Chunk *objs = s.Find(Tag("OBJS"));
      Chunk *scne = s.Find(Tag("SCNE"));

      if (!kfrm || !objs || !scne)
        return error = "KFRM/OBJS/SCNE missing", false;

      { // KFRM
        const Bytes &k = kfrm->data;

        if (k.size() < 4)
          return error = "KFRM too small", false;

        const uint32_t n = Rd32(k.data());

        if (4 + size_t(n) * 0x14 > k.size())
          return error = "KFRM count", false;

        std::vector<uint32_t> offs;

        for (uint32_t i = 0; i < n; ++i)
          offs.push_back(Rd32(k.data() + 4 + 0x14 * i + 4));

        bool ok;
        auto blobs = SplitBlobs(k, offs, 4 + size_t(n) * 0x14, ok);

        if (!ok)
          return error = "KFRM offsets", false;

        for (uint32_t i = 0; i < n; ++i)
        {
          const uint8_t *e = k.data() + 4 + 0x14 * i;
          Track t;
          t.count = Rd16(e + 8);
          t.kind = Rd16(e + 10);
          t.w = RdF(e + 12);
          t.h = RdF(e + 16);
          t.keys = blobs[Rd32(e + 4)];
          t.order = Rd32(e + 4);

          if (t.keys.size() < t.count * t.KeySize())
            return error = "KFRM keys", false;

          s.tracks[Rd32(e)] = std::move(t);
          s.used.insert(Rd32(e));
        }
      }
      { // OBJS
        const Bytes &o = objs->data;

        if (o.size() < 12)
          return error = "OBJS too small", false;

        const uint32_t n = Rd32(o.data() + 8);

        if (12 + size_t(n) * 16 > o.size())
          return error = "OBJS count", false;

        std::vector<uint32_t> offs;

        for (uint32_t i = 0; i < n; ++i)
          offs.push_back(Rd32(o.data() + 12 + 16 * i + 8));

        bool ok;
        auto blobs = SplitBlobs(o, offs, 12 + size_t(n) * 16, ok);

        if (!ok)
          return error = "OBJS offsets", false;

        for (uint32_t i = 0; i < n; ++i)
        {
          const uint8_t *e = o.data() + 12 + 16 * i;
          const uint32_t off = Rd32(e + 8);
          Object obj{Rd32(e + 4), blobs[off], {}, off};

          if (obj.type == Tag("GROP"))
          {
            if (obj.blob.size() < 0x1C)
              return error = "GROP size", false;

            const uint32_t count = Rd32(obj.blob.data() + 0x14);

            if (Rd32(obj.blob.data() + 0x18) != off + 0x1C || obj.blob.size() < 0x1C + 4 * count)
              return error = "GROP children", false;

            for (uint32_t c = 0; c < count; ++c)
              obj.children.push_back(Rd32(obj.blob.data() + 0x1C + 4 * c));

            obj.blob.resize(0x1C);
          }

          if (obj.type == Tag("TXTR") && obj.blob.size() < 12)
            return error = "TXTR size", false;

          s.used.insert(Rd32(e));
          s.objects[Rd32(e)] = std::move(obj);
        }

        for (auto &[h, o] : s.objects)
          for (uint32_t c : o.children)
            s.parent[c] = h;
      }

      if (Chunk *p = s.Find(Tag("PTEX")))
      { // textures
        const Bytes &d = p->data;

        if (d.size() < 12)
          return error = "PTEX too small", false;

        const uint32_t n = Rd32(d.data()), pages = Rd32(d.data() + 4), off = Rd32(d.data() + 8);

        if (off != 12 + 40 * n || off + 8 * size_t(pages) > d.size())
          return error = "PTEX layout", false;

        for (uint32_t i = 0; i < n; ++i)
        {
          const uint8_t *e = d.data() + 12 + 40 * i;
          Ptex t;

          for (int c = 0; c < 8; ++c)
            t.uv[c] = RdF(e + 4 + 4 * c);

          t.page = Rd32(e + 36);
          s.ptex[Rd32(e)] = t;
          s.used.insert(Rd32(e));
        }
        // Page table, then the headers in order (checked: every scene has this layout)
        if (d.size() - off != 60 * size_t(pages))
          return error = "PTEX page headers", false;

        for (uint32_t i = 0; i < pages; ++i)
        {
          const uint32_t hoff = Rd32(d.data() + off + 8 * i);

          if (hoff != off + 8 * pages + 52 * i)
            return error = "PTEX page header order", false;

          s.pages.push_back({Rd32(d.data() + off + 8 * i + 4), Bytes(d.begin() + hoff, d.begin() + hoff + 52)});
        }
      }
      { // SCNE
        const Bytes &d = scne->data;

        if (d.size() < 4)
          return error = "SCNE too small", false;

        const uint32_t n = Rd32(d.data());

        if (4 + 24 * size_t(n) > d.size())
          return error = "SCNE count", false;

        for (uint32_t i = 0; i < n; ++i)
        {
          const uint8_t *e = d.data() + 4 + 24 * i;
          const uint32_t cnt = Rd32(e + 4), lo = Rd32(e + 12);
          SceneEntry se{Rd32(e), lo, Rd32(e + 8), RdF(e + 16), RdF(e + 20), {}};

          if (lo + 4 * size_t(cnt) > d.size())
            return error = "SCNE list", false;

          for (uint32_t j = 0; j < cnt; ++j)
            se.list.push_back(Rd32(d.data() + lo + 4 * j));

          s.scenes.push_back(std::move(se));
        }
      }

      return true;
    }

    // Rebuilds the chunks; `relocs` gets the pointer fields per chunk
    void Write(Scene &s, std::vector<std::pair<uint32_t, std::vector<uint32_t>>> &relocs)
    {
      { // KFRM
        // Keys stay in their original order (new tracks' last)
        std::vector<std::pair<uint32_t, uint32_t>> by_order;

        for (auto &[h, t] : s.tracks)
          by_order.push_back({t.order, h});

        std::sort(by_order.begin(), by_order.end());
        std::map<uint32_t, uint32_t> where;
        uint32_t at = 4 + 0x14 * uint32_t(s.tracks.size());

        for (auto &[ord, h] : by_order)
        {
          where[h] = at;
          at += uint32_t(s.tracks[h].keys.size());
        }

        Bytes k;
        Put32(k, uint32_t(s.tracks.size()));
        std::vector<uint32_t> rel;

        for (auto &[h, t] : s.tracks)
        {
          rel.push_back(uint32_t(k.size()) + 4);
          Put32(k, h);
          Put32(k, where[h]);
          Put32(k, (uint32_t(t.count) << 16) | t.kind);
          PutF(k, t.w);
          PutF(k, t.h);
        }

        for (auto &[ord, h] : by_order)
          k.insert(k.end(), s.tracks[h].keys.begin(), s.tracks[h].keys.end());

        s.Find(Tag("KFRM"))->data = std::move(k);
        relocs.push_back({Tag("KFRM"), rel});
      }
      { // OBJS
        std::vector<std::pair<uint32_t, uint32_t>> by_order;

        for (auto &[h, o] : s.objects)
          by_order.push_back({o.order, h});

        std::sort(by_order.begin(), by_order.end());
        const uint32_t n = uint32_t(s.objects.size());
        const uint32_t at = 12 + 16 * n;
        std::map<uint32_t, uint32_t> where;
        Bytes blobs;
        std::vector<uint32_t> rel;

        for (auto &[ord, h] : by_order)
        {
          Object &o = s.objects[h];
          where[h] = at + uint32_t(blobs.size());
          Bytes b = o.blob;

          if (o.type == Tag("GROP"))
          {
            Wr32(b.data() + 0x14, uint32_t(o.children.size()));
            Wr32(b.data() + 0x18, where[h] + 0x1C);
            rel.push_back(where[h] + 0x18);

            for (uint32_t c : o.children)
              Put32(b, c);
          }

          blobs.insert(blobs.end(), b.begin(), b.end());
        }

        Bytes d;
        Put32(d, 0);
        Put32(d, 0);
        Put32(d, n);

        for (auto &[h, o] : s.objects)
        {
          Put32(d, h);
          Put32(d, o.type);
          rel.push_back(uint32_t(d.size()));
          Put32(d, where[h]);
          Put32(d, 0);
        }

        d.insert(d.end(), blobs.begin(), blobs.end());
        s.Find(Tag("OBJS"))->data = std::move(d);
        relocs.push_back({Tag("OBJS"), rel});
      }

      if (Chunk *p = s.Find(Tag("PTEX")))
      {
        const uint32_t n = uint32_t(s.ptex.size()), np = uint32_t(s.pages.size());
        const uint32_t off = 12 + 40 * n;
        Bytes d;
        Put32(d, n);
        Put32(d, np);
        Put32(d, off);

        for (auto &[h, t] : s.ptex)
        {
          Put32(d, h);

          for (float f : t.uv)
            PutF(d, f);

          Put32(d, t.page);
        }

        std::vector<uint32_t> rel = {8};

        for (uint32_t i = 0; i < np; ++i)
        {
          rel.push_back(uint32_t(d.size()));
          Put32(d, off + 8 * np + 52 * i);
          Put32(d, s.pages[i].data_offset);
        }

        for (const auto &pg : s.pages)
          d.insert(d.end(), pg.header.begin(), pg.header.end());

        p->data = std::move(d);
        relocs.push_back({Tag("PTEX"), rel});
      }
      { // SCNE
        std::vector<size_t> by_at(s.scenes.size());

        for (size_t i = 0; i < by_at.size(); ++i)
          by_at[i] = i;

        std::stable_sort(by_at.begin(), by_at.end(), [&](size_t a, size_t b)
                         { return s.scenes[a].list_at < s.scenes[b].list_at; });
        std::vector<uint32_t> lo(s.scenes.size());
        uint32_t at = 4 + 24 * uint32_t(s.scenes.size());

        for (size_t i : by_at)
        {
          lo[i] = at;
          at += 4 * uint32_t(s.scenes[i].list.size());
        }

        Bytes d;
        Put32(d, uint32_t(s.scenes.size()));
        std::vector<uint32_t> rel;

        for (size_t i = 0; i < s.scenes.size(); ++i)
        {
          const SceneEntry &se = s.scenes[i];
          Put32(d, se.hash);
          Put32(d, uint32_t(se.list.size()));
          Put32(d, se.zero);
          rel.push_back(uint32_t(d.size()));
          Put32(d, lo[i]);
          PutF(d, se.w);
          PutF(d, se.h);
        }

        for (size_t i : by_at)
          for (uint32_t h : s.scenes[i].list)
            Put32(d, h);

        s.Find(Tag("SCNE"))->data = std::move(d);
        relocs.push_back({Tag("SCNE"), rel});
      }
    }

    bool PatchRel(const uint8_t *data, size_t size, const std::vector<std::pair<uint32_t, std::vector<uint32_t>>> &relocs, Bytes &out, std::string &error)
    {
      auto chunks = ReadChunks(data, size, error);

      if (!chunks)
        return false;

      size_t replaced = 0;

      for (Chunk &c : *chunks)
      {
        if (c.tag != Tag("RELO") || c.data.size() < 4)
          continue;

        const uint32_t target = Rd32(c.data.data());

        for (const auto &[tag, offs] : relocs)
        {
          if (tag != target)
            continue;

          Bytes d;
          Put32(d, tag);

          for (uint32_t o : offs)
          {
            Put32(d, 0x10000);
            Put32(d, o);
          }

          Put32(d, 0);
          Put32(d, 0);
          c.data = std::move(d);
          ++replaced;
        }
      }

      if (replaced != relocs.size())
        return error = "RELO chunks missing", false;

      out = WriteChunks(*chunks);
      return true;
    }

    // layout helpers
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
        if (n > best_n || (n == best_n && std::llabs(x) < std::llabs(best)))
          best = x, best_n = n;

      return float(double(best) / 10000.0);
    }

    bool ConstSx(const Track &t, float &sx)
    {
      if (!t.count)
        return false;

      sx = t.Sx(0);

      for (int k = 1; k < t.count; ++k)
        if (std::fabs(t.Sx(k) - sx) > 1e-4f)
          return false;

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

    // Adds `child` next to `after` (in its group, or in the scene lists that hold it)
    void InsertAfter(Scene &s, uint32_t after, uint32_t child)
    {
      if (auto p = s.parent.find(after); p != s.parent.end())
      {
        auto &kids = s.objects[p->second].children;
        auto it = std::find(kids.begin(), kids.end(), after);
        kids.insert(it == kids.end() ? kids.end() : it + 1, child);
        s.parent[child] = p->second;
        return;
      }

      for (SceneEntry &se : s.scenes)
        if (std::find(se.list.begin(), se.list.end(), after) != se.list.end())
        {
          se.list.push_back(child);
          std::sort(se.list.begin(), se.list.end());
        }
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
        if (dst[i] <= dst[i - 1])
          return false;
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
        if (o.type == Tag("GROP"))
        {
          groups.push_back(h);

          for (uint32_t c : o.children)
            parent[c] = h;
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
            log->push_back(fmt::format("stars: group {:08X} row y {:.3f}: {} columns {:.3f}..{:.3f} "
                                       "(spacing {:.4f}), +{} per side",
                                       g, double(std::get<1>(key)) / 1000.0, cols.size(), cols.front().first, cols.back().first, d, more));
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

  // Main menu (SSTMainMenu.DAT, scene 83D07B85): a seventh category row, QUIT, below OPTIONS (see scene_widen.h)
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
      if (Rd32(bar.Key(k)) == kOptionsBarLabel)
        bar_options = k;

    if (bar_options < 0 || bar.kind != 0)
      return error = "cursor bar keys", false;
    // Clone the Xbox LIVE subtree with the row's hashes
    std::vector<uint32_t> new_hashes = {row.parent, row.row, row.highlight};

    for (uint32_t i = 0; i < 8; ++i)
      new_hashes.push_back(row.first_child + i);

    for (uint32_t h : new_hashes)
      if (s.used.count(h))
        return error = fmt::format("hash {:08X} in use", h), false;

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
        if (Rd32(t.Key(k)) == 0xEBD48B4Cu)
          two = RdF(t.Key(k) + 0x10) - closed;

      if (closed >= 0 && two > 0.05f && two + 0.041f < options_shift)
      {
        options_shift = two + 0.041f;

        for (int k = 0; k < t.count; ++k)
          if (Rd32(t.Key(k)) == 0x6893313Au)
            WrF(t.Key(k) + 0x10, closed + options_shift);
      }
    }
    // Pushed down by OPTIONS' sub-items: keys from_label (y 0) and to_label (y shift) at frames past the row's own animations
    {
      Track &t = s.tracks[row.row];

      if (t.kind != 0 || !t.count)
        return error = "row keys", false;

      Bytes key(t.Key(0), t.Key(0) + t.KeySize());

      for (int k = 0; k < t.count; ++k)
        if (RdF(t.Key(k) + 0x10) == 0.0f)
          key.assign(t.Key(k), t.Key(k) + t.KeySize());

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
      if (se.hash == kScene && std::find(se.list.begin(), se.list.end(), kLiveParent) != se.list.end())
      {
        se.list.push_back(row.parent);
        std::sort(se.list.begin(), se.list.end());
        listed = true;
      }

    if (!listed)
      return error = "scene list", false;

    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> relocs;
    Write(s, relocs);
    Bytes new_rel;

    if (!PatchRel(rel, rel_size, relocs, new_rel, error))
      return false;

    out_dat = WriteChunks(s.chunks);
    out_rel = std::move(new_rel);
    return true;
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
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> relocs;
    Write(s, relocs);
    Bytes new_rel;

    if (!PatchRel(rel, rel_size, relocs, new_rel, error))
      return false;

    out_dat = WriteChunks(s.chunks);
    out_rel = std::move(new_rel);
    return true;
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
    // A sprite is scaled and turned about its pivot (blob +0x2C / +0x30: offset from the key's position); its centre is pivot + scale * (half size - pivot)
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

    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> relocs;
    Write(s, relocs);
    Bytes new_rel;

    if (!PatchRel(rel, rel_size, relocs, new_rel, error))
      return false;

    out_dat = WriteChunks(s.chunks);
    out_rel = std::move(new_rel);
    return true;
  }

  bool RoundTrip(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, Bytes &out_dat, Bytes &out_rel, std::string &error)
  {
    Scene s;

    if (!Parse(dat, dat_size, s, error))
      return false;

    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> relocs;
    Write(s, relocs);

    if (!PatchRel(rel, rel_size, relocs, out_rel, error))
      return false;

    out_dat = WriteChunks(s.chunks);
    return true;
  }

  bool WantsWideLayout(const std::string &name)
  {
    static const char *kSkip[] = {"hud.dat", "hudicons.dat", "spechud.dat", "stadiumig.dat", "ingame.dat", "pausemenu.dat"};

    if (!name.ends_with(".dat"))
      return false;
    // Language files (<stem>_EN.dat ...) hold only texts and fonts
    static const char *kLangs[] = {"_en", "_fr", "_ge", "_it", "_jp", "_sp", "_us"};

    for (const char *l : kLangs)
      if (name.size() > 7 && name.compare(name.size() - 7, 3, l) == 0)
        return false;

    for (const char *n : kSkip)
      if (name == n)
        return false;

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

    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> relocs;
    Write(s, relocs);
    Bytes new_rel;

    if (!PatchRel(rel, rel_size, relocs, new_rel, error))
      return false;

    out_dat = WriteChunks(s.chunks);
    out_rel = std::move(new_rel);
    return true;
  }
  namespace
  {
    // A kind 0 keyframe from a template key (other fields kept)
    Bytes MakeKey(const uint8_t *tmpl, uint32_t label, uint32_t frame, uint32_t colour, float x, float y, float sx, float sy)
    {
      Bytes k(tmpl, tmpl + 0x28);
      Wr32(k.data(), label);
      Wr32(k.data() + 4, frame);
      Wr32(k.data() + 8, colour);
      WrF(k.data() + 0xC, x);
      WrF(k.data() + 0x10, y);
      WrF(k.data() + 0x14, sx);
      WrF(k.data() + 0x18, sy);
      return k;
    }

    // A one-key (or several keys') track: kind 0, size w x h
    Track MakeTrack(Scene &s, float w, float h, const std::vector<Bytes> &keys)
    {
      Track t;
      t.kind = 0;
      t.w = w;
      t.h = h;
      t.count = uint16_t(keys.size());

      for (const Bytes &k : keys)
        t.keys.insert(t.keys.end(), k.begin(), k.end());

      t.order = s.next_order++;
      return t;
    }

    // A new object: a copy of `from`'s blob with its own hash (TXTR / TEXT: +8, GROP / GORD: +0)
    Object &AddObject(Scene &s, uint32_t from, uint32_t h, Track track)
    {
      Object o = s.objects.at(from);
      o.order = s.next_order++;
      o.children.clear();

      if (o.type == Tag("GROP") || o.type == Tag("GORD"))
        Wr32(o.blob.data(), h);
      else
        Wr32(o.blob.data() + 8, h);

      s.used.insert(h);
      s.tracks[h] = std::move(track);
      return s.objects[h] = std::move(o);
    }

    void AddChild(Scene &s, uint32_t group, uint32_t child)
    {
      s.objects[group].children.push_back(child);
      s.parent[child] = group;
    }

    // TEXT blob fields
    void SetText(Object &o, uint32_t text_id, uint32_t scissor, int align)
    {
      Wr32(o.blob.data() + 4, text_id);
      Wr32(o.blob.data() + 0xC, scissor);
      o.blob[0x20] = 0;
      o.blob[0x21] = uint8_t(align); // 0 left, 1 centre, 2 right
      o.blob[0x22] = 0;              // no wrapping
    }

    bool AddIconPage(Scene &s, const IconPage &p, std::string &error)
    {
      if (!s.Find(Tag("PTEX")) || s.pages.empty())
        return error = "no texture pages", false;

      if (!p.width || p.width % 32 || !p.height || p.slots.empty() || p.slots.size() * p.height > p.width || (p.data_offset & 0xFFF))
        return error = "icon page layout", false;

      for (uint32_t h : p.slots)
        if (s.used.count(h))
          return error = fmt::format("texture {:08X} exists", h), false;

      Scene::Page pg = s.pages[0];
      uint8_t *fc = pg.header.data() + 0x1C; // fetch constant
      Wr32(fc, 0x00000002u | ((p.width / 32) << 22)); // linear, pitch
      Wr32(fc + 4, 0x00000086u);                       // k_8_8_8_8, 8-in-32 (the loader adds the buffer address)
      Wr32(fc + 8, (p.width - 1) | ((p.height - 1) << 13));
      Wr32(fc + 12, 0x00000C14u);                      // ARGB in memory
      pg.data_offset = p.data_offset;
      s.pages.push_back(std::move(pg));
      const uint32_t page = uint32_t(s.pages.size() - 1);

      for (size_t k = 0; k < p.slots.size(); ++k)
      {
        Ptex t;
        const float u0 = float(k * p.height) / float(p.width), u1 = float((k + 1) * p.height) / float(p.width);
        const float uv[8] = {u0, 0.0f, u1, 0.0f, u1, 1.0f, u0, 1.0f};
        std::memcpy(t.uv, uv, sizeof(uv));
        t.page = page;
        s.ptex[p.slots[k]] = t;
        s.used.insert(p.slots[k]);
      }

      return true;
    }

    bool Finish(Scene &s, const uint8_t *rel, size_t rel_size, Bytes &out_dat, Bytes &out_rel, std::string &error)
    {
      std::vector<std::pair<uint32_t, std::vector<uint32_t>>> relocs;
      Write(s, relocs);
      Bytes new_rel;

      if (!PatchRel(rel, rel_size, relocs, new_rel, error))
        return false;

      out_dat = WriteChunks(s.chunks);
      out_rel = std::move(new_rel);
      return true;
    }
  }

  bool AddAchievementList(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, AchievementList &list, Bytes &out_dat, Bytes &out_rel, std::string &error)
  {
    // RECORDS page: the content group (scrolls between d32c49f0 and 03ccad8d) in group 07c39636, the panel's clip (SCIS), a section's blue panel, a label (left) and a value (right), the title group and text
    constexpr uint32_t kContent = 0x165C0375, kParent = 0x07C39636, kClip = 0xD8018F46, kPanel = 0x127BA0C0;
    constexpr uint32_t kLabel = 0x6BE99606, kValue = 0x55CC6209, kTitleGroup = 0xA4AB0C99, kTitle = 0xECC2F67F;
    constexpr uint32_t kBottom = 0x03CCAD8D; // the content group's end key (top: d32c49f0)
    Scene s;

    if (!Parse(dat, dat_size, s, error))
      return false;

    for (uint32_t h : {kContent, kParent, kClip, kPanel, kLabel, kValue, kTitleGroup, kTitle})
      if (!s.objects.count(h) || !s.tracks.count(h))
        return error = fmt::format("object {:08X} missing", h), false;

    const size_t rows = list.icon_slot.size();

    if (!rows || rows > 0x400 || list.desc_scale.size() != rows || list.visible_rows < 1)
      return error = "rows", false;

    std::vector<uint32_t> fresh = {list.list, list.summary};

    for (uint32_t i = 0; i < rows; ++i)
      for (uint32_t b : {list.row_group, list.row_panel, list.row_icon, list.row_icon_locked, list.row_name, list.row_desc, list.row_score})
        fresh.push_back(b + i);

    for (uint32_t h : fresh)
      if (s.used.count(h))
        return error = fmt::format("{:08X} already used", h), false;

    for (const SceneEntry &se : s.scenes)
      if (se.hash == list.page_hash || !list.page_hash)
        return error = "page hash", false;

    for (uint32_t h : {kContent, kClip, kPanel, kLabel, kValue, kTitle})
      if (s.tracks[h].kind != 0 || !s.tracks[h].count)
        return error = fmt::format("track {:08X}", h), false;

    if (s.parent[kContent] != kParent || s.parent[kTitle] != kTitleGroup || s.objects[kLabel].blob.size() < 0x34 || s.objects[kPanel].blob.size() < 0x30)
      return error = "records page layout", false;

    if (!AddIconPage(s, list.page, error))
      return false;

    // The clip area (the records content group and its sections sit at the parent's origin)
    const uint8_t *clip_key = s.tracks[kClip].Key(0);
    const float clip_top = RdF(clip_key + 0x10), clip_h = RdF(clip_key + 0x18);

    if (!(clip_top > 0.1f && clip_top < 0.5f && clip_h > 0.2f && clip_h < 0.8f))
      return error = fmt::format("clip {:.3f} + {:.3f}", clip_top, clip_h), false;

    const float pitch = clip_h / float(list.visible_rows);
    list.pitch = pitch;
    const float range = std::max(0.0f, pitch * float(rows) - clip_h);
    // The list: the content group's track (fades in with the page), its bottom key at the end of the list
    {
      Track t = s.tracks[kContent];
      t.order = s.next_order++;
      bool bottom = false;

      for (int k = 0; k < t.count; ++k)
        if (Rd32(t.Key(k)) == kBottom)
        {
          WrF(t.Key(k) + 0x10, -range);
          bottom = true;
        }

      if (!bottom)
        return error = "content keys", false;

      AddObject(s, kContent, list.list, std::move(t));
    }

    const uint8_t *panel_key = s.tracks[kPanel].Key(0);
    const uint8_t *label_key = s.tracks[kLabel].Key(0);
    const uint8_t *value_key = s.tracks[kValue].Key(0);
    const float panel_h = s.tracks[kPanel].h;
    const float label_x = RdF(label_key + 0xC), value_x = RdF(value_key + 0xC);
    // Layout inside a row (layout heights): panel inset 0.004 top and bottom; icon (64 px, 0.95) centred; texts' baselines
    const float icon_w = float(list.page.height) / 1280.0f, icon_h = float(list.page.height) / 720.0f, icon_s = 0.95f;
    const float icon_x = label_x - 0.022f, text_x = icon_x + icon_w * icon_s + 0.018f;

    for (uint32_t i = 0; i < rows; ++i)
    {
      const uint32_t g = list.row_group + i;
      {
        Object &o = AddObject(s, list.list, g, MakeTrack(s, 1.0f, 1.0f, {MakeKey(s.tracks[kContent].Key(0), 0x5E391A57, 0, 0xFFFFFFFF, 0.0f, pitch * float(i), 1.0f, 1.0f)}));
        (void)o;
        AddChild(s, list.list, g);
      }
      const float top = clip_top;
      // panel
      {
        Track t = MakeTrack(s, s.tracks[kPanel].w, panel_h, {MakeKey(panel_key, 0x5E391A57, 0, 0xFFFFFFFF, RdF(panel_key + 0xC), top + 0.004f, RdF(panel_key + 0x14), (pitch - 0.008f) / panel_h)});
        AddObject(s, kPanel, list.row_panel + i, std::move(t));
        AddChild(s, g, list.row_panel + i);
      }
      // icons (colour, locked): the panel's object with another texture, one layer nearer
      for (int locked = 0; locked < 2; ++locked)
      {
        const uint32_t h = (locked ? list.row_icon_locked : list.row_icon) + i;
        const int slot = list.icon_slot[i] + (locked ? list.locked_offset : 0);

        if (slot < 0 || size_t(slot) >= list.page.slots.size())
          return error = "icon slot", false;

        Track t = MakeTrack(s, icon_w, icon_h, {MakeKey(panel_key, 0x5E391A57, 0, 0xFFFFFFFF, icon_x, top + (pitch - icon_h * icon_s) / 2, icon_s, icon_s)});
        Object &o = AddObject(s, kPanel, h, std::move(t));
        Wr32(o.blob.data(), list.page.slots[slot]);
        Wr32(o.blob.data() + 0x28, Rd32(o.blob.data() + 0x28) - 1);
        WrF(o.blob.data() + 0x2C, 0.0f);
        WrF(o.blob.data() + 0x30, 0.0f);
        AddChild(s, g, h);
      }
      // texts
      struct
      {
        uint32_t h, from;
        int align;
        float x, y, scale;
        uint32_t colour;
      } texts[] = {
          {list.row_name + i, kLabel, 0, text_x, top + pitch * 0.385f, 1.0f, 0xFFFFFFFF},
          {list.row_desc + i, kLabel, 0, text_x, top + pitch * 0.72f, list.desc_scale[i], 0xBFD4F2FF},
          {list.row_score + i, kValue, 2, value_x, top + pitch * 0.385f, 0.85f, 0xD7E4F7FF}, // (the unlock date)
      };

      for (const auto &tx : texts)
      {
        const uint8_t *tk = s.tracks[tx.from].Key(0);
        Object &o = AddObject(s, tx.from, tx.h, MakeTrack(s, s.tracks[tx.from].w, s.tracks[tx.from].h, {MakeKey(tk, 0x5E391A57, 0, tx.colour, tx.x, tx.y, tx.scale, tx.scale)}));
        SetText(o, list.text_blank, kClip, tx.align);
        AddChild(s, g, tx.h);
      }
    }

    // Summary right of the title (the title's font, right aligned at the panel's right side)
    {
      const uint8_t *tk = s.tracks[kTitle].Key(0);
      Object &o = AddObject(s, kTitle, list.summary, MakeTrack(s, s.tracks[kTitle].w, s.tracks[kTitle].h, {MakeKey(tk, Rd32(tk), Rd32(tk + 4), 0xFFFFFFFF, 1.0f - RdF(tk + 0xC), RdF(tk + 0x10), RdF(tk + 0x14), RdF(tk + 0x18))}));
      SetText(o, list.text_blank, Rd32(s.objects[kTitle].blob.data() + 0xC), 2);
    }

    // Both on a page of their own, entered with the RECORDS page by the port's script only (the RECORDS page shows and hides its own objects, children included). The records content group and the title group sit at the layout origin, so the positions stay
    {
      SceneEntry se = s.scenes.front();
      se.hash = list.page_hash;
      se.list = {std::min(list.list, list.summary), std::max(list.list, list.summary)};
      se.list_at = 0xFFFFFFF0u;
      auto at = std::find_if(s.scenes.begin(), s.scenes.end(), [&](const SceneEntry &e)
                             { return e.hash > list.page_hash; });
      s.scenes.insert(at, std::move(se));
    }

    return Finish(s, rel, rel_size, out_dat, out_rel, error);
  }

  bool AddAchievementPopup(const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, const AchievementPopup &popup, Bytes &out_dat, Bytes &out_rel, std::string &error)
  {
    // MsgBox "WARNING" dialog parts: its outer group, background picture, title bar, title text and body text
    constexpr uint32_t kGroup = 0x1A5DF58D, kPicture = 0x4AE89809, kBar = 0xBA63C930, kTitle = 0xD46F11C8, kBody = 0x8C70A56C;
    Scene s;

    if (!Parse(dat, dat_size, s, error))
      return false;

    for (uint32_t h : {kGroup, kPicture, kBar, kTitle, kBody})
      if (!s.objects.count(h) || !s.tracks.count(h))
        return error = fmt::format("object {:08X} missing", h), false;

    std::vector<uint32_t> fresh = {popup.group, popup.bar, popup.header, popup.name};

    for (int k = 0; k < AchievementPopup::kLines; ++k)
      fresh.push_back(popup.panel[k]), fresh.push_back(popup.desc[k]);

    for (size_t k = 0; k < popup.page.slots.size(); ++k)
      fresh.push_back(popup.icon_base + uint32_t(k));

    for (uint32_t h : fresh)
      if (!h || s.used.count(h))
        return error = fmt::format("{:08X} already used", h), false;

    for (const SceneEntry &se : s.scenes)
      if (se.hash == popup.page_hash)
        return error = "page exists", false;

    if (s.tracks[kGroup].kind != 0 || s.tracks[kPicture].kind != 0 || s.tracks[kTitle].kind != 0 || s.tracks[kBody].kind != 0 || s.tracks[kBar].kind != 2 || s.objects[kPicture].blob.size() < 0x34 || s.objects[kTitle].blob.size() < 0x34 || s.scenes.empty())
      return error = "dialog layout", false;

    if (!AddIconPage(s, popup.page, error))
      return false;

    // Box: top centre of the 16:9 layout. Body: the name's baseline, then the description lines (scale 0.78)
    constexpr float kX = 0.30f, kY = 0.03f, kW = 0.40f, kBarH = 0.032f;
    constexpr float kName = 0.040f, kFirstLine = 0.070f, kLine = 0.027f, kBottom = 0.018f, kDescScale = 0.78f;
    auto body_height = [&](int lines)
    {
      return kFirstLine + kLine * float(lines - 1) + kBottom;
    };
    const float max_h = kBarH + body_height(AchievementPopup::kLines);
    const uint8_t *gk = s.tracks[kGroup].Key(0);
    AddObject(s, kGroup, popup.group, MakeTrack(s, 1.0f, 1.0f, {MakeKey(gk, popup.hide_label, 0, 0xFFFFFFFF, 0.0f, -(kY + max_h + 0.03f), 1.0f, 1.0f), MakeKey(gk, popup.show_label, 10, 0xFFFFFFFF, 0.0f, 0.0f, 1.0f, 1.0f)}));

    // panel pictures, one per description height
    for (int k = 0; k < AchievementPopup::kLines; ++k)
    {
      const Track &pt = s.tracks[kPicture];
      const float h = kBarH + body_height(k + 1);
      Object &o = AddObject(s, kPicture, popup.panel[k], MakeTrack(s, pt.w, pt.h, {MakeKey(pt.keys.data(), 0x5E391A57, 0, 0xFFFFFFFF, kX, kY, kW / pt.w, h / pt.h)}));
      Wr32(o.blob.data() + 0xC, 0); // no clip
      AddChild(s, popup.group, popup.panel[k]);
    }

    // title bar (a gradient rectangle: kind 2 keys, same layout up to the scale)
    {
      Track t = s.tracks[kBar];
      t.order = s.next_order++;
      t.count = 1;
      t.keys.resize(t.KeySize());
      Wr32(t.Key(0), 0x5E391A57);
      Wr32(t.Key(0) + 4, 0);
      WrF(t.Key(0) + 0xC, kX);
      WrF(t.Key(0) + 0x10, kY);
      WrF(t.Key(0) + 0x14, kW / t.w);
      WrF(t.Key(0) + 0x18, kBarH / t.h);
      AddObject(s, kBar, popup.bar, std::move(t));
      AddChild(s, popup.group, popup.bar);
    }

    // icons, one per slot (the page shows the one for the achievement), centred on the name and first line
    const float icon_w = float(popup.page.height) / 1280.0f, icon_h = float(popup.page.height) / 720.0f, icon_s = 0.72f;
    const float body_top = kY + kBarH;

    for (size_t k = 0; k < popup.page.slots.size(); ++k)
    {
      const Track &pt = s.tracks[kPicture];
      const uint32_t h = popup.icon_base + uint32_t(k);
      Object &o = AddObject(s, kPicture, h, MakeTrack(s, icon_w, icon_h, {MakeKey(pt.keys.data(), 0x5E391A57, 0, 0xFFFFFFFF, kX + 0.012f, body_top + (body_height(1) - icon_h * icon_s) / 2, icon_s, icon_s)}));
      Wr32(o.blob.data(), popup.page.slots[k]);
      Wr32(o.blob.data() + 0xC, 0);
      Wr32(o.blob.data() + 0x28, 0); // in front of the panel
      WrF(o.blob.data() + 0x2C, 0.0f);
      WrF(o.blob.data() + 0x30, 0.0f);
      AddChild(s, popup.group, h);
    }

    // texts: header (title font, centred in the bar), name and description lines (body font)
    const float text_x = kX + 0.012f + icon_w * icon_s + 0.014f;
    struct Text
    {
      uint32_t h, from;
      int align;
      float x, y, scale;
      uint32_t colour;
    };
    std::vector<Text> texts = {
        {popup.header, kTitle, 1, kX + kW / 2, kY + kBarH * 0.78f, 0.62f, 0xFFFFFFFF},
        {popup.name, kBody, 0, text_x, body_top + kName, 1.0f, 0xFFFFFFFF},
    };

    for (int k = 0; k < AchievementPopup::kLines; ++k)
      texts.push_back({popup.desc[k], kBody, 0, text_x, body_top + kFirstLine + kLine * float(k), kDescScale, 0xD7E4F7FF});

    // (kPopupDescWidth = the room from text_x to the box's right edge less 0.012, 0.326 x 1280 px, at kDescScale)

    for (const auto &tx : texts)
    {
      const Track &tt = s.tracks[tx.from];
      Object &o = AddObject(s, tx.from, tx.h, MakeTrack(s, tt.w, tt.h, {MakeKey(tt.keys.data(), 0x5E391A57, 0, tx.colour, tx.x, tx.y, tx.scale, tx.scale)}));
      SetText(o, popup.text_blank, 0, tx.align);
      Wr32(o.blob.data() + 0x30, 0); // front
      AddChild(s, popup.group, tx.h);
    }

    // The page: a new scene entry (kept in hash order) listing the group
    SceneEntry se = s.scenes.front();
    se.hash = popup.page_hash;
    se.list = {popup.group};
    se.list_at = 0xFFFFFFF0u;
    auto at = std::find_if(s.scenes.begin(), s.scenes.end(), [&](const SceneEntry &e)
                           { return e.hash > popup.page_hash; });
    s.scenes.insert(at, std::move(se));
    return Finish(s, rel, rel_size, out_dat, out_rel, error);
  }

}