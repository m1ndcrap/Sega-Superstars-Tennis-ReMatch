// The SumoTool scene format

#include "menu/scene_format.h"

#include <algorithm>

namespace sst::menu::scene
{
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

      for (auto &[h, obj] : s.objects)
      {
        for (uint32_t c : obj.children)
          s.parent[c] = h;
      }
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
  void Write(Scene &s, Relocs &relocs)
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
      {
        for (uint32_t h : s.scenes[i].list)
          Put32(d, h);
      }

      s.Find(Tag("SCNE"))->data = std::move(d);
      relocs.push_back({Tag("SCNE"), rel});
    }
  }

  bool PatchRel(const uint8_t *data, size_t size, const Relocs &relocs, Bytes &out, std::string &error)
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

  Chunk *FindChunk(std::vector<Chunk> &chunks, uint32_t tag)
  {
    for (Chunk &c : chunks)
    {
      if (c.tag == tag)
        return &c;
    }

    return nullptr;
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
    {
      if (std::find(se.list.begin(), se.list.end(), after) != se.list.end())
      {
        se.list.push_back(child);
        std::sort(se.list.begin(), se.list.end());
      }
    }
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