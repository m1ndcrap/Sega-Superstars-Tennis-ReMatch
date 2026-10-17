// sst_rematch - the achievements' scenes: the list on Options > ACHIEVEMENTS (a page added to the RECORDS scene) and the unlock popup (a page added to MsgBox). achievements.cpp fills them...

#include "menu/scene_format.h"
#include "menu/scene_patches.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <fmt/format.h>

namespace sst::menu::scenes
{
  using namespace scene;

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
      uint8_t *fc = pg.header.data() + 0x1C;          // fetch constant
      Wr32(fc, 0x00000002u | ((p.width / 32) << 22)); // linear, pitch
      Wr32(fc + 4, 0x00000086u);                      // k_8_8_8_8, 8-in-32 (the loader adds the buffer address)
      Wr32(fc + 8, (p.width - 1) | ((p.height - 1) << 13));
      Wr32(fc + 12, 0x00000C14u); // ARGB in memory
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
    {
      if (!s.objects.count(h) || !s.tracks.count(h))
        return error = fmt::format("object {:08X} missing", h), false;
    }

    const size_t rows = list.icon_slot.size();

    if (!rows || rows > 0x400 || list.desc_scale.size() != rows || list.visible_rows < 1)
      return error = "rows", false;

    std::vector<uint32_t> fresh = {list.list, list.summary};

    for (uint32_t i = 0; i < rows; ++i)
    {
      for (uint32_t b : {list.row_group, list.row_panel, list.row_icon, list.row_icon_locked, list.row_name, list.row_desc, list.row_score})
        fresh.push_back(b + i);
    }

    for (uint32_t h : fresh)
    {
      if (s.used.count(h))
        return error = fmt::format("{:08X} already used", h), false;
    }

    for (const SceneEntry &se : s.scenes)
    {
      if (se.hash == list.page_hash || !list.page_hash)
        return error = "page hash", false;
    }

    for (uint32_t h : {kContent, kClip, kPanel, kLabel, kValue, kTitle})
    {
      if (s.tracks[h].kind != 0 || !s.tracks[h].count)
        return error = fmt::format("track {:08X}", h), false;
    }

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
      {
        if (Rd32(t.Key(k)) == kBottom)
        {
          WrF(t.Key(k) + 0x10, -range);
          bottom = true;
        }
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
    {
      if (!s.objects.count(h) || !s.tracks.count(h))
        return error = fmt::format("object {:08X} missing", h), false;
    }

    std::vector<uint32_t> fresh = {popup.group, popup.bar, popup.header, popup.name};

    for (int k = 0; k < AchievementPopup::kLines; ++k)
      fresh.push_back(popup.panel[k]), fresh.push_back(popup.desc[k]);

    for (size_t k = 0; k < popup.page.slots.size(); ++k)
      fresh.push_back(popup.icon_base + uint32_t(k));

    for (uint32_t h : fresh)
    {
      if (!h || s.used.count(h))
        return error = fmt::format("{:08X} already used", h), false;
    }

    for (const SceneEntry &se : s.scenes)
    {
      if (se.hash == popup.page_hash)
        return error = "page exists", false;
    }

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