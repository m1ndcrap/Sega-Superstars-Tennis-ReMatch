// Adds a VIDEO row to the in-match pause menu

#include "menu/pause_patch.h"
#include "menu/scene_format.h"
#include "menu/script_asm.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

namespace sst::menu::pause
{
  using scene::Chunk;
  using scene::FindChunk;
  using scene::ReadChunks;
  using scene::SplitBlobs;
  using scene::WriteChunks;
  using script::Asm;
  using script::ModuleSize;

  namespace
  {
    // the scene's objects (PauseMenu, Xbox)
    constexpr std::array<uint32_t, 6> kRowGroups = {0x67D16D27, 0x8A139936, 0x8D0D485A, 0xF8A9F905, 0x29150F3E, 0x8DB964AC};
    constexpr std::array<uint32_t, 6> kRowLabels = {0x33FF8B92, 0x8F98A65B, 0x28150F7C, 0x2408EA3F, 0x112267F4, 0xEACC3DFA};
    constexpr uint32_t kRetryRow = 0x8A139936;                                       // template for the new row
    constexpr uint32_t kValueText = 0xA18E2471;                                      // CONTROLLER's value ("TYPE A")
    constexpr std::array<uint32_t, 3> kPanel = {0x30EF67A8, 0xC9A9E37B, 0xCE5A4A4D}; // edges, body

    // The confirmation dialog for the full panel (F73595A0): body, then the YES / NO bar group, cursor group and YES / NO texts
    constexpr uint32_t kDialogBody = 0x0944873D;
    constexpr std::array<uint32_t, 4> kDialogLower = {0xE8F65357, 0x20A4C7FB, 0x97B856AC, 0x778A36F5};
    constexpr uint32_t kValueFont = 0x2207B8BE; // has 0-9 (the row font only has 1-4)
    constexpr float kValueScale = 1.15f;        // its glyphs are smaller than the row font's

    // New objects (hashes chosen outside the scene's)
    constexpr uint32_t kNewRow = 0x53560006;
    constexpr uint32_t kNewRowChild = 0x53560010; // + 0..3, copies of RETRY's children
    constexpr uint32_t kSpareLabel = 0x53560020;  // + row
    constexpr uint32_t kSpareValue = 0x53560030;  // + row
    // The VIDEO page's scroll bar (pause_patch.h)
    constexpr uint32_t kPanelGroup = 0x26F7E3DD; // the panel (edges, body); its colour fades the menu in and out
    constexpr uint32_t kDarkStrip = 0x0944873D;  // the confirmation dialog's body: a dark blue strip (texture 85FE4B82)
    constexpr uint32_t kPlayerTag = 0x400E36C4;  // the "1P" tag (texture 71F80B5B); the scroll thumb uses the 3P tag's texture

    struct Track
    {
      uint16_t count, kind;
      float w, h;
      Bytes keys;
      size_t KeySize() const { return kind == 2 ? 0x38 : 0x28; }
    };

    struct Object
    {
      uint32_t type;
      Bytes blob;                     // GROP: header only (0x1C bytes)
      std::vector<uint32_t> children; // GROP
      uint32_t order;                 // original position (new objects go last)
    };
  }

  // scene
  std::optional<Bytes> PatchDat(const uint8_t *data, size_t size, Relocs &relocs, std::string &error, uint32_t thumb_tex)
  {
    auto chunks_opt = ReadChunks(data, size, error);

    if (!chunks_opt)
      return std::nullopt;

    std::vector<Chunk> &chunks = *chunks_opt;
    Chunk *kfrm = FindChunk(chunks, Tag("KFRM"));
    Chunk *objs = FindChunk(chunks, Tag("OBJS"));
    Chunk *scne = FindChunk(chunks, Tag("SCNE"));

    if (!kfrm || !objs || !scne)
    {
      error = "KFRM/OBJS/SCNE missing";
      return std::nullopt;
    }

    // Tracks
    std::map<uint32_t, Track> tracks;
    {
      const Bytes &k = kfrm->data;

      if (k.size() < 4)
        return error = "KFRM too small", std::nullopt;

      const uint32_t n = Rd32(k.data());

      if (4 + size_t(n) * 0x14 > k.size())
        return error = "KFRM count", std::nullopt;

      std::vector<uint32_t> offs;

      for (uint32_t i = 0; i < n; ++i)
        offs.push_back(Rd32(k.data() + 4 + 0x14 * i + 4));

      bool ok;
      auto blobs = SplitBlobs(k, offs, 4 + size_t(n) * 0x14, ok);

      if (!ok)
        return error = "KFRM offsets", std::nullopt;

      for (uint32_t i = 0; i < n; ++i)
      {
        const uint8_t *e = k.data() + 4 + 0x14 * i;
        Track t{Rd16(e + 8), Rd16(e + 10), RdF(e + 12), RdF(e + 16), blobs[Rd32(e + 4)]};

        if (t.keys.size() < t.count * t.KeySize())
          return error = "KFRM keys", std::nullopt;

        t.keys.resize(t.count * t.KeySize());
        tracks[Rd32(e)] = std::move(t);
      }
    }

    // Objects
    std::map<uint32_t, Object> objects;
    {
      const Bytes &o = objs->data;

      if (o.size() < 12)
        return error = "OBJS too small", std::nullopt;

      const uint32_t n = Rd32(o.data() + 8);

      if (12 + size_t(n) * 16 > o.size())
        return error = "OBJS count", std::nullopt;

      std::vector<uint32_t> offs;

      for (uint32_t i = 0; i < n; ++i)
        offs.push_back(Rd32(o.data() + 12 + 16 * i + 8));

      bool ok;
      auto blobs = SplitBlobs(o, offs, 12 + size_t(n) * 16, ok);

      if (!ok)
        return error = "OBJS offsets", std::nullopt;

      for (uint32_t i = 0; i < n; ++i)
      {
        const uint8_t *e = o.data() + 12 + 16 * i;
        const uint32_t off = Rd32(e + 8);
        Object obj{Rd32(e + 4), blobs[off], {}, off};

        if (obj.type == Tag("GROP"))
        {
          if (obj.blob.size() < 0x1C)
            return error = "GROP size", std::nullopt;

          const uint32_t count = Rd32(obj.blob.data() + 0x14);

          if (Rd32(obj.blob.data() + 0x18) != off + 0x1C || obj.blob.size() < 0x1C + 4 * count)
            return error = "GROP children", std::nullopt;

          for (uint32_t c = 0; c < count; ++c)
            obj.children.push_back(Rd32(obj.blob.data() + 0x1C + 4 * c));

          obj.blob.resize(0x1C);
        }

        objects[Rd32(e)] = std::move(obj);
      }
    }

    // Everything this patch touches must be there (this is the known file)
    auto need = [&](uint32_t h, uint32_t type)
    {
      auto it = objects.find(h);
      return it != objects.end() && it->second.type == type && tracks.count(h);
    };

    for (int r = 0; r < 6; ++r)
    {
      if (!need(kRowGroups[r], Tag("GROP")) || !need(kRowLabels[r], Tag("TEXT")))
        return error = "pause rows not found", std::nullopt;
    }

    if (!need(kValueText, Tag("TEXT")))
      return error = "value text not found", std::nullopt;

    for (uint32_t h : kPanel)
    {
      if (!need(h, Tag("TXTR")))
        return error = "panel not found", std::nullopt;
    }

    if (!need(kDialogBody, Tag("TXTR")) || !(tracks[kDialogBody].h > 0.1f))
      return error = "dialog not found", std::nullopt;

    for (uint32_t h : kDialogLower)
    {
      if (!objects.count(h) || !tracks.count(h))
        return error = "dialog parts not found", std::nullopt;
    }

    for (uint32_t h : objects[kRetryRow].children)
    {
      if (!objects.count(h) || !tracks.count(h))
        return error = "RETRY row", std::nullopt;
    }

    if (objects[kRetryRow].children.size() != 4 || objects.count(kNewRow))
      return error = "unexpected RETRY row / already patched", std::nullopt;

    auto key_y = [&](uint32_t h)
    {
      return RdF(tracks[h].keys.data() + 0x10);
    };

    const float dy = key_y(kRowGroups[5]) - key_y(kRowGroups[4]);

    if (!(dy > 0.03f && dy < 0.08f))
      return error = "row spacing", std::nullopt;

    // VIDEO goes where RETURN TO CHARACTER SELECT was; that row and RETURN TO MAIN MENU move down one spacing (kVisualOrder)
    const float new_y = key_y(kRowGroups[4]);

    auto move_y = [&](uint32_t h, float by)
    {
      Track &t = tracks[h];

      for (uint16_t k = 0; k < t.count; ++k)
      {
        uint8_t *key = t.keys.data() + k * t.KeySize();
        WrF(key + 0x10, RdF(key + 0x10) + by);
      }
    };

    uint32_t order = 0x7FFF0000;

    auto clone = [&](uint32_t from, uint32_t to)
    {
      Object o = objects[from];
      o.order = order++;

      // Own hash: +0 for GROP / GORD / SCIS, +8 for TEXT / TXTR
      const bool at8 = o.type == Tag("TEXT") || o.type == Tag("TXTR");
      Wr32(o.blob.data() + (at8 ? 8 : 0), to);
      objects[to] = std::move(o);
      tracks[to] = tracks[from];
    };

    // The new row: RETRY's group and children, one spacing below the last row
    clone(kRetryRow, kNewRow);

    objects[kNewRow].children.clear();
    {
      const std::vector<uint32_t> kids = objects[kRetryRow].children;

      for (size_t i = 0; i < kids.size(); ++i)
      {
        const uint32_t h = kNewRowChild + uint32_t(i);
        clone(kids[i], h);

        if (objects[h].type == Tag("TEXT"))
          Wr32(objects[h].blob.data() + 4, kTextVideo);

        objects[kNewRow].children.push_back(h);
      }

      Track &t = tracks[kNewRow];

      for (uint16_t k = 0; k < t.count; ++k)
        WrF(t.keys.data() + k * t.KeySize() + 0x10, new_y);
    }

    // The new row's label is its TEXT child
    uint32_t new_label = 0;

    for (uint32_t h : objects[kNewRow].children)
    {
      if (objects[h].type == Tag("TEXT"))
        new_label = h;
    }

    if (new_label != kNewRowChild + 3)
      return error = "RETRY row layout", std::nullopt;

    // Spare label + value texts for every row (blank until the VIDEO page)
    for (int r = 0; r < kRows; ++r)
    {
      const uint32_t row = r < 6 ? kRowGroups[r] : kNewRow;
      const uint32_t label = r < 6 ? kRowLabels[r] : new_label;
      const uint32_t sl = kSpareLabel + uint32_t(r), sv = kSpareValue + uint32_t(r);
      clone(label, sl);
      Wr32(objects[sl].blob.data() + 4, kTextBlank);
      clone(kValueText, sv);
      Wr32(objects[sv].blob.data() + 0, kValueFont);
      Wr32(objects[sv].blob.data() + 4, kTextBlank);
      Track &t = tracks[sv];

      for (uint16_t k = 0; k < t.count; ++k)
      {
        uint8_t *key = t.keys.data() + k * t.KeySize();
        WrF(key + 0x14, RdF(key + 0x14) * kValueScale);
        WrF(key + 0x18, RdF(key + 0x18) * kValueScale);
      }

      objects[row].children.push_back(sl);
      objects[row].children.push_back(sv);
    }

    move_y(kRowGroups[4], dy);
    move_y(kRowGroups[5], dy);

    // The panel grows by one row
    for (uint32_t h : kPanel)
      tracks[h].h += dy;

    // So does the confirmation dialog over it (RETRY / RETURN TO ...): its body gets one row taller, the YES / NO bar, buttons and cursor move down
    {
      Track &body = tracks[kDialogBody];

      for (uint16_t k = 0; k < body.count; ++k)
      {
        uint8_t *key = body.keys.data() + k * body.KeySize();
        WrF(key + 0x18, RdF(key + 0x18) + dy / body.h);
      }

      for (uint32_t h : kDialogLower)
        move_y(h, dy);
    }

    // The VIDEO page's scroll bar: a group in the panel (hidden until the page shows it) with a dark track along the panel's right side, over the rows, and a yellow thumb with one key per scroll position
    {
      if (!need(kPanelGroup, Tag("GROP")) || !need(kDarkStrip, Tag("TXTR")) || !need(kPlayerTag, Tag("TXTR")) || objects.count(kScrollGroup))
        return error = "scroll bar parts not found", std::nullopt;

      auto first_key = [&](uint32_t h)
      {
        return Bytes(tracks[h].keys.begin(), tracks[h].keys.begin() + tracks[h].KeySize());
      };

      // In the panel group's space (the panel pieces are its children). The panel texture is 300 texels tall: the title bar and its white line take rows 0-42, the bottom bar's white line starts at row 286; the right edge piece's last 10 / 21 texels are its dark rim
      const float body_y = RdF(tracks[kPanel[1]].keys.data() + 0x10), body_h = tracks[kPanel[1]].h;
      const float edge_x = RdF(tracks[kPanel[2]].keys.data() + 0xC), edge_w = tracks[kPanel[2]].w;
      const float y0 = body_y + body_h * 43.0f / 300.0f + 0.003f, height = body_h * (286.0f - 43.0f) / 300.0f - 0.006f;
      const float width = 0.0065f, x = edge_x + edge_w - width; // flush with the panel's right edge
      const float thumb_h = height * float(kScrollVisible) / float(kScrollCount);

      if (!(height > 0.15f && height < 0.6f && x > 0.5f && x < 1.0f))
        return error = "scroll bar layout", std::nullopt;

      // Group: a copy of the panel group's header, no children of its own but the two pieces
      Object g = objects[kPanelGroup];
      g.order = order++;
      Wr32(g.blob.data(), kScrollGroup);
      g.children = {kScrollTrack, kScrollThumb};
      objects[kScrollGroup] = std::move(g);
      objects[kPanelGroup].children.push_back(kScrollGroup);

      {
        Track t = tracks[kPanelGroup];
        Bytes key = first_key(kPanelGroup);
        t.keys.clear();
        t.count = 0;

        for (auto [label, frame, alpha] : {std::tuple{kScrollHide, 0u, 0x00u}, std::tuple{kScrollShow, 10u, 0xFFu}})
        {
          Wr32(key.data(), label);
          Wr32(key.data() + 4, frame);
          Wr32(key.data() + 8, 0xFFFFFF00u | alpha | 0xFF); // (a group's colour does not reach its children: shown / hidden with 4:4 / 4:5)
          WrF(key.data() + 0xC, 0.0f);
          WrF(key.data() + 0x10, 0.0f);
          WrF(key.data() + 0x14, 1.0f);
          WrF(key.data() + 0x18, 1.0f);
          t.keys.insert(t.keys.end(), key.begin(), key.end());
          ++t.count;
        }

        tracks[kScrollGroup] = std::move(t);
      }

      // A TXTR copy with texture `tex` and keys {label, frame, y} at x, scaled to width x h
      // layer: draw order (blob +0x28, lower is in front): the panel is 60, row arrows 48, the row cursor 32
      auto piece = [&](uint32_t from, uint32_t to, uint32_t tex, uint32_t layer, float h, uint32_t colour, const std::vector<std::pair<uint32_t, float>> &keys)
      {
        clone(from, to);
        Wr32(objects[to].blob.data(), tex);
        Wr32(objects[to].blob.data() + 0x28, layer);
        Track &t = tracks[to];
        Bytes key = first_key(from);
        t.keys.clear();
        t.count = 0;
        uint32_t frame = 0;

        for (auto [label, y] : keys)
        {
          Wr32(key.data(), label);
          Wr32(key.data() + 4, frame);

          // The sprite is scaled about its pivot (blob +0x2C / +0x30, from the key's position): put the scaled box's top left at (x, y)
          const float sx = width / t.w, sy = h / t.h;
          const float px = RdF(objects[to].blob.data() + 0x2C), py = RdF(objects[to].blob.data() + 0x30);
          Wr32(key.data() + 8, colour);
          WrF(key.data() + 0xC, x - px * (1 - sx));
          WrF(key.data() + 0x10, y - py * (1 - sy));
          WrF(key.data() + 0x14, sx);
          WrF(key.data() + 0x18, sy);
          WrF(key.data() + 0x20, 0.0f);
          t.keys.insert(t.keys.end(), key.begin(), key.end());
          ++t.count;
          frame += 10;
        }
      };

      piece(kDarkStrip, kScrollTrack, Rd32(objects[kDarkStrip].blob.data()), 47, height, 0xFFFFFFC0u, {{kScrollHide, y0}});
      std::vector<std::pair<uint32_t, float>> positions;

      for (int i = 0; i <= kScrollCount - kScrollVisible; ++i)
        positions.push_back({kScrollPos + uint32_t(i), y0 + (height - thumb_h) * float(i) / float(kScrollCount - kScrollVisible)});

      piece(kPlayerTag, kScrollThumb, thumb_tex, 46, thumb_h, 0xFFFFFFFFu, positions); // in front of the track
    }

    // rebuild KFRM
    {
      Bytes k;
      Put32(k, uint32_t(tracks.size()));
      const uint32_t keys_at = 4 + 0x14 * uint32_t(tracks.size());
      uint32_t at = keys_at;
      std::vector<uint32_t> rel;

      for (auto &[h, t] : tracks)
      {
        rel.push_back(uint32_t(k.size()) + 4);
        Put32(k, h);
        Put32(k, at);
        Put32(k, (uint32_t(t.count) << 16) | t.kind);
        uint32_t u;
        std::memcpy(&u, &t.w, 4);
        Put32(k, u);
        std::memcpy(&u, &t.h, 4);
        Put32(k, u);
        at += uint32_t(t.keys.size());
      }

      for (auto &[h, t] : tracks)
        k.insert(k.end(), t.keys.begin(), t.keys.end());

      kfrm->data = std::move(k);
      relocs.chunks.push_back({Tag("KFRM"), rel});
    }

    // rebuild OBJS (objects keep their order; new ones go last)
    {
      std::vector<std::pair<uint32_t, uint32_t>> by_order; // order, hash

      for (auto &[h, o] : objects)
        by_order.push_back({o.order, h});

      std::sort(by_order.begin(), by_order.end());
      const uint32_t n = uint32_t(objects.size());
      uint32_t at = 12 + 16 * n;
      std::map<uint32_t, uint32_t> where;
      Bytes blobs;
      std::vector<uint32_t> rel_children;

      for (auto &[ord, h] : by_order)
      {
        Object &o = objects[h];
        where[h] = at + uint32_t(blobs.size());
        Bytes b = o.blob;

        if (o.type == Tag("GROP"))
        {
          Wr32(b.data() + 0x14, uint32_t(o.children.size()));
          Wr32(b.data() + 0x18, where[h] + 0x1C);
          rel_children.push_back(where[h] + 0x18);

          for (uint32_t c : o.children)
            Put32(b, c);
        }

        blobs.insert(blobs.end(), b.begin(), b.end());
      }

      Bytes d;
      Put32(d, 0);
      Put32(d, 0);
      Put32(d, n);
      std::vector<uint32_t> rel_table;

      for (auto &[h, o] : objects)
      {
        Put32(d, h);
        Put32(d, o.type);
        rel_table.push_back(uint32_t(d.size()));
        Put32(d, where[h]);
        Put32(d, 0);
      }

      d.insert(d.end(), blobs.begin(), blobs.end());
      objs->data = std::move(d);
      std::vector<uint32_t> rel = rel_children;
      rel.insert(rel.end(), rel_table.begin(), rel_table.end());
      relocs.chunks.push_back({Tag("OBJS"), rel});
    }

    // SCNE: the new row is a top-level object
    {
      Bytes &s = scne->data;

      if (s.size() < 0x1C || Rd32(s.data() + 0x10) != 0x1C)
        return error = "SCNE", std::nullopt;

      const uint32_t n = Rd32(s.data() + 8);

      if (0x1C + 4 * size_t(n) > s.size())
        return error = "SCNE count", std::nullopt;

      std::vector<uint32_t> list;

      for (uint32_t i = 0; i < n; ++i)
        list.push_back(Rd32(s.data() + 0x1C + 4 * i));

      list.push_back(kNewRow);
      std::sort(list.begin(), list.end());
      Bytes d(s.begin(), s.begin() + 0x1C);
      Wr32(d.data() + 8, uint32_t(list.size()));

      for (uint32_t h : list)
        Put32(d, h);

      s = std::move(d);
      relocs.chunks.push_back({Tag("SCNE"), {0x10}});
    }

    return WriteChunks(chunks);
  }

  std::optional<Bytes> PatchRel(const uint8_t *data, size_t size, const Relocs &relocs, std::string &error)
  {
    auto chunks_opt = ReadChunks(data, size, error);

    if (!chunks_opt)
      return std::nullopt;

    std::vector<Chunk> &chunks = *chunks_opt;
    size_t replaced = 0;

    for (Chunk &c : chunks)
    {
      if (c.tag != Tag("RELO") || c.data.size() < 4)
        continue;

      const uint32_t target = Rd32(c.data.data());

      for (const auto &[tag, offs] : relocs.chunks)
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

    if (replaced != relocs.chunks.size())
    {
      error = "RELO chunks missing";
      return std::nullopt;
    }

    return WriteChunks(chunks);
  }

  namespace
  {
    constexpr uint16_t kGlobalHighlight = 2; // keyframe labels: [0] intro, [1] off, [2] on
    constexpr uint16_t kGlobalRows = 3;      // row groups
    constexpr uint16_t kGlobalInput = 4;     // per-player input channels
    constexpr int16_t kSlotCursor = 4, kSlotConfirm = 9, kSlotPlayer = 13;
    constexpr uint32_t kTitle = 0x689DE25C, kTitleText = 0xF9A54306; // "PAUSE"

    // Hidden while the VIDEO page is up: row labels, CONTROLLER / VIBRATION values and arrows
    constexpr std::array<uint32_t, 13> kHide = {0x33FF8B92, 0x8F98A65B, 0x28150F7C, 0x2408EA3F, 0x112267F4, 0xEACC3DFA, kNewRowChild + 3 /* the VIDEO row's label */, 0xA18E2471, 0xAF864C0C, 0x8F3B2FFB, 0x2A449F8F, 0xCD44A386, 0xD7D8B275};
    constexpr uint32_t kSndMove = 0xFB699F5A, kSndSelect = 0x46987907, kSndBack = 0x9FB7A417;
  }

  std::optional<Bytes> PatchScript(const uint8_t *m, size_t size, std::string &error)
  {
    const size_t msize = ModuleSize(m, size);

    if (!msize)
      return error = "not a script module", std::nullopt;

    const uint32_t functab = Rd32(m + 4), t2 = Rd32(m + 8), t3 = Rd32(m + 12), debug = Rd32(m + 28);

    if ((functab | t2 | t3 | debug) & 3)
      return error = "module alignment", std::nullopt;

    // Globals: rows array, untouched (lazily relocated globals have bit 7 clear)
    const uint32_t nglob = Rd32(m + t2);

    if (nglob <= kGlobalInput || t2 + 4 + 5 * nglob > t3)
      return error = "globals", std::nullopt;

    const uint32_t slot = t2 + 4 + 4 * kGlobalRows;
    const uint8_t type = m[t2 + 4 + 4 * nglob + kGlobalRows];

    if (type != 0x86)
      return error = "rows global already used", std::nullopt;

    const uint32_t arr = slot + Rd32(m + slot);

    if (arr + 0x14 > t3 || Rd32(m + arr + 8) != 6)
      return error = "rows array", std::nullopt;

    const uint32_t elems = arr + Rd32(m + arr + 0xC);

    for (int i = 0; i < 6; ++i)
    {
      if (Rd32(m + elems + 8 * i) != kRowGroups[i] || Rd32(m + elems + 8 * i + 4) != 3)
        return error = "rows array contents", std::nullopt;
    }

    // "Select pressed": pushabs 7; pushi 4; index; op eq; jf NEXT; then pushabs 9; pushabs 4; index (confirmation text for the row) - replaced by a jump to the trampoline
    static const uint8_t kSig[] = {0x29, 0x00, 0x07, 0x01, 0x00, 0x00, 0x00, 0x04, 0x23, 0x0D, 0x14, 0x14, 0xFF, 0xFF, 0x29, 0x00, 0x09, 0x29, 0x00, 0x04, 0x23};
    size_t found = 0, at = 0;

    for (size_t p = 0x20; p + sizeof(kSig) <= functab; ++p)
    {
      bool ok = true;

      for (size_t i = 0; i < sizeof(kSig) && ok; ++i)
        ok = (i == 12 || i == 13) || m[p + i] == kSig[i];

      if (ok)
        ++found, at = p;
    }

    if (found != 1)
      return error = "select handler not found", std::nullopt;

    const uint32_t jf_next = uint32_t(at) + 14; // after the jf
    const uint32_t next = jf_next + uint32_t(int16_t(Rd16(m + at + 12)));
    const uint32_t patch_at = jf_next; // pushabs 9; pushabs 4; index (7 bytes)

    // Up / down: the cursor (abs 4) steps through the rows in kVisualOrder. The game's code: pushabs 4; pushi 1; op sub|add; stabs 4; then the wrap to [abs 10, abs 10 + abs 11). Replaced by abs4 = native step(abs4, -1|+1, abs10, abs11), padded with nops
    struct Nav
    {
      std::vector<int> sig; // -1 = any byte
      int dir;
    };

    const Nav kNav[2] = {{{0x29, 0, 4, 0x01, 0, 0, 0, 1, 0x0D, 13, 0x27, 0, 4, // abs4 - 1
                           0x29, 0, 4, 0x29, 0, 10, 0x0D, 16, 0x14, -1, -1,    // < abs10 ?
                           0x29, 0, 4, 0x29, 0, 11, 0x0D, 12, 0x27, 0, 4},     // += abs11
                          -1},
                         {{0x29, 0, 4, 0x01, 0, 0, 0, 1, 0x0D, 12, 0x27, 0, 4,                     // abs4 + 1
                           0x29, 0, 4, 0x29, 0, 10, 0x29, 0, 11, 0x0D, 12, 0x0D, 19, 0x14, -1, -1, // >= end ?
                           0x29, 0, 4, 0x29, 0, 11, 0x0D, 13, 0x27, 0, 4},                         // -= abs11
                          1}};

    std::vector<std::pair<uint32_t, const Nav *>> nav_sites;

    for (const Nav &n : kNav)
    {
      size_t hits = 0, hit = 0;

      for (size_t p = 0x20; p + n.sig.size() <= functab; ++p)
      {
        bool ok = true;

        for (size_t i = 0; i < n.sig.size() && ok; ++i)
          ok = n.sig[i] < 0 || m[p + i] == n.sig[i];

        if (ok)
          ++hits, hit = p;
      }

      if (hits != 1)
        return error = "cursor step not found", std::nullopt;

      nav_sites.push_back({uint32_t(hit), &n});
    }

    // the trampoline, appended to the code
    Asm a(functab);
    const uint32_t tramp = a.pc();
    const int back = a.label(), loop = a.label(), no_redraw = a.label(), same = a.label();

    auto call = [&](uint8_t g, uint8_t i, uint8_t argc, bool keep = false)
    {
      a.native(g, i, argc);

      if (!keep)
        a.pop();
    };

    auto result = [&](int k)
    {
      a.pushi(k);
      call(0x53, kNativeResult, 1, true);
    };

    auto row = [&](auto &&index)
    {
      a.pushg(kGlobalRows);
      index();
      a.index();
      a.op(6);
    };

    auto hl = [&](int k)
    {
      a.pushg(kGlobalHighlight);
      a.pushi(k);
      a.index();
      a.op(6);
    };

    auto highlight = [&](auto &&index, bool on)
    {
      row(index);
      hl(on ? 2 : 1);
      hl(on ? 1 : 2);
      a.pushi(on ? 0 : 1);
      call(4, 10, 4);
    };

    auto fixed = [&](int r)
    {
      return [&a, r]
      {
        a.pushi(r);
      };
    };

    auto from_result = [&](int k)
    {
      return [&, k]
      {
        result(k);
      };
    };

    auto sound = [&](uint32_t h)
    {
      a.pushh(h);
      call(7, 1, 1);
    };

    auto set_text = [&](uint32_t obj, auto &&text)
    {
      a.pushh(obj);
      text();
      call(4, 15, 2);
    };

    // Row r's spare texts show the page's line at r's place, top to bottom
    auto line_of = [](int r)
    {
      for (int v = 0; v < kRows; ++v)
      {
        if (kVisualOrder[v] == r)
          return v;
      }

      return r;
    };

    auto redraw = [&]
    {
      for (int r = 0; r < kRows; ++r)
      {
        const int v = line_of(r);

        set_text(kSpareLabel + r, [&]
                 { a.pushi(v), call(0x53, kNativeLabel, 1, true); });

        set_text(kSpareValue + r, [&]
                 { a.pushi(v), call(0x53, kNativeValue, 1, true); });
      }
    };

    a.pushabs(kSlotCursor);
    a.pushi(kVideoRow);
    a.op(20); // eq
    a.jump(0x14, back);
    sound(kSndSelect);
    call(0x53, kNativeBegin, 0);

    for (uint32_t h : kHide)
    {
      a.pushh(h);
      call(4, 5, 1); // hide
    }

    set_text(kTitle, [&]
             { a.pushh(kTextVideo); });

    highlight(fixed(kVideoRow), false);
    highlight(fixed(0), true);
    redraw();

    a.here(loop);
    a.pushi(0);
    a.yield();

    // Input event, as the pause menu reads it: 3:18(input[9:193(player) * 3])
    a.pushg(kGlobalInput);
    a.pushabs(kSlotPlayer);
    a.op(3); // toint
    a.native(9, 193, 1);
    a.pushi(3);
    a.op(9); // mul
    a.pushi(0);
    a.op(12); // add
    a.index();
    a.op(6);
    a.native(3, 18, 1);
    call(0x53, kNativeEventHash, 1);
    result(2);
    a.jump(0x14, no_redraw);
    redraw();
    a.here(no_redraw);
    result(0);
    result(1);
    a.op(20);
    a.jump(0x13, same);
    highlight(from_result(0), false);
    highlight(from_result(1), true);
    a.here(same);
    const uint32_t sounds[3] = {kSndMove, kSndSelect, kSndBack};

    for (int s = 0; s < 3; ++s)
    {
      const int skip = a.label();
      result(4);
      a.pushi(s + 1);
      a.op(20);
      a.jump(0x14, skip);
      sound(sounds[s]);
      a.here(skip);
    }

    result(3);
    a.jump(0x14, loop);

    // Leave: back to the pause menu, cursor on VIDEO
    highlight(from_result(1), false);
    highlight(fixed(kVideoRow), true);

    for (int r = 0; r < kRows; ++r)
    {
      set_text(kSpareLabel + r, [&]
               { a.pushh(kTextBlank); });

      set_text(kSpareValue + r, [&]
               { a.pushh(kTextBlank); });
    }

    for (uint32_t h : kHide)
    {
      a.pushh(h);
      call(4, 4, 1); // show
    }

    set_text(kTitle, [&]
             { a.pushh(kTitleText); });

    a.jump_abs(0x15, next);

    // Any other row: the game's own code
    a.here(back);
    a.pushabs(kSlotConfirm);
    a.pushabs(kSlotCursor);
    a.index();
    a.jump_abs(0x15, patch_at + 7);

    Bytes tcode;

    if (!a.finish(tcode))
      return error = "trampoline assembly", std::nullopt;

    while (tcode.size() & 3)
      tcode.push_back(0x11); // nop

    const uint32_t d1 = uint32_t(tcode.size());

    // new module; [0, functab) code (patched) | trampoline | [functab, t3) function table and globals | new rows array | [t3, end) symbols and debug info
    const uint32_t new_arr_elems = t3 + d1; // after the old globals block
    const uint32_t d2 = 7 * 8 + 0x14;
    Bytes out(m, m + functab);
    out.insert(out.end(), tcode.begin(), tcode.end());
    out.insert(out.end(), m + functab, m + t3);

    for (int i = 0; i < 6; ++i)
    {
      Put32(out, kRowGroups[i]);
      Put32(out, 3);
    }

    Put32(out, kNewRow);
    Put32(out, 3);
    const uint32_t new_arr = uint32_t(out.size());
    Put32(out, 0);
    Put32(out, 0xFFFF8800);
    Put32(out, 7);
    Put32(out, uint32_t(int32_t(new_arr_elems) - int32_t(new_arr)));
    Put32(out, 0x80000007);
    out.insert(out.end(), m + t3, m + msize);

    // Code: jump to the trampoline instead of reading the confirmation text
    {
      const int32_t rel = int32_t(tramp) - int32_t(patch_at + 3);
      out[patch_at] = 0x15;
      out[patch_at + 1] = uint8_t(uint16_t(rel) >> 8);
      out[patch_at + 2] = uint8_t(rel);

      for (int i = 3; i < 7; ++i)
        out[patch_at + i] = 0x11;
    }

    for (auto [at_nav, n] : nav_sites)
    {
      Asm step(at_nav);
      step.pushabs(kSlotCursor);
      step.pushi(n->dir);
      step.pushabs(10);
      step.pushabs(11);
      step.native(0x53, kNativeStep, 4);
      step.u8(0x27), step.u16(uint16_t(kSlotCursor)); // stabs
      Bytes code;

      if (!step.finish(code) || code.size() > n->sig.size())
        return error = "cursor step", std::nullopt;

      code.resize(n->sig.size(), 0x11); // nop
      std::copy(code.begin(), code.end(), out.begin() + at_nav);
    }

    // Header and the rows global
    Wr32(out.data() + 4, functab + d1);
    Wr32(out.data() + 8, t2 + d1);
    Wr32(out.data() + 12, t3 + d1 + d2);
    Wr32(out.data() + 28, debug + d1 + d2);
    const uint32_t new_slot = slot + d1;
    Wr32(out.data() + new_slot, new_arr - new_slot);

    // Debug info: pointers to the source names
    const uint32_t new_debug = debug + d1 + d2;
    const uint32_t first = Rd32(m + debug);

    for (uint32_t p = 0; p < first - debug; p += 4)
      Wr32(out.data() + new_debug + p, Rd32(m + debug + p) + d1 + d2);

    return out;
  }
}