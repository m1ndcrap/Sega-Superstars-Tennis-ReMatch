// Native renderer: the keyboard's prompt pictures (key caps with the bound keys' names, drawn with GDI on Windows, rasterized here elsewhere, at 4x and filtered down)

#include "gpu/native/prompt_art.h"

#include <rex/cvar.h>

#include <xxhash.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>
#endif

namespace sst::native::prompts
{
  namespace
  {
    struct Mask
    {
      uint32_t w = 0, h = 0;
      std::vector<uint8_t> v; // coverage at 1x
    };

#if defined(_WIN32)
    template <typename DrawFn>
    Mask Coverage(uint32_t w, uint32_t h, DrawFn draw)
    {
      constexpr int kS = 4;
      Mask m;
      m.w = w, m.h = h;
      m.v.assign(size_t(w) * h, 0);
      const int W = int(w) * kS, H = int(h) * kS;
      BITMAPINFO bi = {};
      bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
      bi.bmiHeader.biWidth = W;
      bi.bmiHeader.biHeight = -H;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      void *bits = nullptr;
      HDC dc = CreateCompatibleDC(nullptr);
      HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;

      if (!dc || !bmp || !bits)
      {
        if (bmp)
          DeleteObject(bmp);

        if (dc)
          DeleteDC(dc);

        return m;
      }

      HGDIOBJ old = SelectObject(dc, bmp);
      std::memset(bits, 0, size_t(W) * H * 4);
      draw(dc, W, H, kS);
      GdiFlush();
      const uint8_t *p = static_cast<const uint8_t *>(bits);

      for (uint32_t y = 0; y < h; ++y)
      {
        for (uint32_t x = 0; x < w; ++x)
        {
          uint32_t sum = 0;

          for (int sy = 0; sy < kS; ++sy)
            for (int sx = 0; sx < kS; ++sx)
              sum += p[(size_t(y * kS + sy) * W + (x * kS + sx)) * 4 + 1]; // green channel

          m.v[size_t(y) * w + x] = uint8_t(sum / (kS * kS));
        }
      }

      SelectObject(dc, old);
      DeleteObject(bmp);
      DeleteDC(dc);
      return m;
    }

    void WhiteRoundRect(HDC dc, int x0, int y0, int x1, int y1, int r)
    {
      HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
      HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
      RoundRect(dc, x0, y0, x1 + 1, y1 + 1, r, r);
      SelectObject(dc, ob);
      SelectObject(dc, op);
      DeleteObject(br);
    }

    // White text centred in the box, shrunk to fit
    void WhiteText(HDC dc, const std::wstring &text, int x0, int y0, int x1, int y1)
    {
      int size = int((y1 - y0) * (text.size() > 1 ? 0.62f : 0.72f));
      HFONT font = nullptr;

      for (int tries = 0; tries < 12; ++tries)
      {
        if (font)
          DeleteObject(font);

        font = CreateFontW(-size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Arial");
        HGDIOBJ of = SelectObject(dc, font);
        SIZE ext = {};
        GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &ext);
        SelectObject(dc, of);

        if (ext.cx <= (x1 - x0) * 86 / 100 || size < 8)
          break;

        size = size * 88 / 100;
      }

      HGDIOBJ of = SelectObject(dc, font);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(255, 255, 255));
      RECT r = {x0, y0, x1, y1};
      DrawTextW(dc, text.c_str(), int(text.size()), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(dc, of);
      DeleteObject(font);
    }

    void WhiteTriangle(HDC dc, int cx, int cy, int r, int dir) // 0 up, 1 down, 2 left, 3 right
    {
      POINT p[3];

      switch (dir)
      {
      case 0:
        p[0] = {cx, cy - r}, p[1] = {cx - r, cy + r * 2 / 3}, p[2] = {cx + r, cy + r * 2 / 3};
        break;

      case 1:
        p[0] = {cx, cy + r}, p[1] = {cx - r, cy - r * 2 / 3}, p[2] = {cx + r, cy - r * 2 / 3};
        break;

      case 2:
        p[0] = {cx - r, cy}, p[1] = {cx + r * 2 / 3, cy - r}, p[2] = {cx + r * 2 / 3, cy + r};
        break;

      default:
        p[0] = {cx + r, cy}, p[1] = {cx - r * 2 / 3, cy - r}, p[2] = {cx - r * 2 / 3, cy + r};
        break;
      }

      HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
      HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
      Polygon(dc, p, 3);
      SelectObject(dc, ob);
      SelectObject(dc, op);
      DeleteObject(br);
    }

#else
    // Without GDI: the same shapes rasterized here (an 8-bit coverage canvas at 4x), the legends with stb_truetype and a system font (Liberation Sans Bold first: Arial's metrics)
    struct Canvas
    {
      int w = 0, h = 0;
      std::vector<uint8_t> v;

      void Plot(int x, int y, uint8_t c)
      {
        if (x >= 0 && y >= 0 && x < w && y < h)
          v[size_t(y) * w + x] = std::max(v[size_t(y) * w + x], c);
      }
    };

    using HDC = Canvas *;

    template <typename DrawFn>
    Mask Coverage(uint32_t w, uint32_t h, DrawFn draw)
    {
      constexpr int kS = 4;
      Mask m;
      m.w = w, m.h = h;
      m.v.assign(size_t(w) * h, 0);
      Canvas c;
      c.w = int(w) * kS, c.h = int(h) * kS;
      c.v.assign(size_t(c.w) * c.h, 0);
      draw(&c, c.w, c.h, kS);

      for (uint32_t y = 0; y < h; ++y)
      {
        for (uint32_t x = 0; x < w; ++x)
        {
          uint32_t sum = 0;

          for (int sy = 0; sy < kS; ++sy)
          {
            for (int sx = 0; sx < kS; ++sx)
              sum += c.v[size_t(y * kS + sy) * c.w + (x * kS + sx)];
          }

          m.v[size_t(y) * w + x] = uint8_t(sum / (kS * kS));
        }
      }

      return m;
    }

    // As GDI's RoundRect(x0, y0, x1 + 1, y1 + 1, r, r): corner ellipses r wide
    void WhiteRoundRect(HDC dc, int x0, int y0, int x1, int y1, int r)
    {
      const float rr = std::max(0.0f, r * 0.5f);

      for (int y = std::max(y0, 0); y <= std::min(y1, dc->h - 1); ++y)
      {
        for (int x = std::max(x0, 0); x <= std::min(x1, dc->w - 1); ++x)
        {
          const float px = x + 0.5f, py = y + 0.5f;
          const float cx = std::clamp(px, x0 + rr, x1 + 1 - rr), cy = std::clamp(py, y0 + rr, y1 + 1 - rr);

          if ((px - cx) * (px - cx) + (py - cy) * (py - cy) <= rr * rr)
            dc->Plot(x, y, 255);
        }
      }
    }

    const stbtt_fontinfo *Font()
    {
      static std::once_flag once;
      static std::vector<unsigned char> data;
      static stbtt_fontinfo info;
      static bool ok = false;

      std::call_once(once, []
                     {
        static const char *const kFonts[] = {
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf",
            "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
            "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf",
            "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
        };

        for (const char *path : kFonts)
        {
          std::ifstream in(path, std::ios::binary);

          if (!in)
            continue;

          data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

          if (!data.empty() && stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0)))
          {
            ok = true;
            return;
          }
        } });

      return ok ? &info : nullptr;
    }

    // White text centred in the box, shrunk to fit (as the GDI version: em size = 62% / 72% of the box height, then 88% steps until
    // it fits in 86% of the width)
    void WhiteText(HDC dc, const std::wstring &text, int x0, int y0, int x1, int y1)
    {
      const stbtt_fontinfo *font = Font();

      if (!font || text.empty())
        return;

      int size = int((y1 - y0) * (text.size() > 1 ? 0.62f : 0.72f));
      float scale = 0;
      float width = 0;

      for (int tries = 0; tries < 12; ++tries)
      {
        scale = stbtt_ScaleForMappingEmToPixels(font, float(size));
        width = 0;

        for (size_t i = 0; i < text.size(); ++i)
        {
          int advance = 0, lsb = 0;
          stbtt_GetCodepointHMetrics(font, int(text[i]), &advance, &lsb);
          width += advance * scale;

          if (i + 1 < text.size())
            width += stbtt_GetCodepointKernAdvance(font, int(text[i]), int(text[i + 1])) * scale;
        }

        if (width <= (x1 - x0) * 86 / 100 || size < 8)
          break;

        size = size * 88 / 100;
      }

      int ascent = 0, descent = 0, gap = 0;
      stbtt_GetFontVMetrics(font, &ascent, &descent, &gap);
      const float line = (ascent - descent) * scale;
      const float baseline = y0 + ((y1 - y0) - line) * 0.5f + ascent * scale;
      float pen = x0 + ((x1 - x0) - width) * 0.5f;

      for (size_t i = 0; i < text.size(); ++i)
      {
        const int cp = int(text[i]);
        int gw = 0, gh = 0, gx = 0, gy = 0;
        unsigned char *bitmap = stbtt_GetCodepointBitmapSubpixel(font, scale, scale, pen - std::floor(pen), 0, cp, &gw, &gh, &gx, &gy);

        if (bitmap)
        {
          for (int y = 0; y < gh; ++y)
          {
            for (int x = 0; x < gw; ++x)
              dc->Plot(int(std::floor(pen)) + gx + x, int(std::lround(baseline)) + gy + y, bitmap[y * gw + x]);
          }

          stbtt_FreeBitmap(bitmap, nullptr);
        }

        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(font, cp, &advance, &lsb);
        pen += advance * scale;

        if (i + 1 < text.size())
          pen += stbtt_GetCodepointKernAdvance(font, cp, int(text[i + 1])) * scale;
      }
    }

    void WhiteTriangle(HDC dc, int cx, int cy, int r, int dir) // 0 up, 1 down, 2 left, 3 right
    {
      float p[3][2];

      switch (dir)
      {
      case 0:
        p[0][0] = cx, p[0][1] = cy - r, p[1][0] = cx - r, p[1][1] = cy + r * 2 / 3, p[2][0] = cx + r, p[2][1] = cy + r * 2 / 3;
        break;

      case 1:
        p[0][0] = cx, p[0][1] = cy + r, p[1][0] = cx - r, p[1][1] = cy - r * 2 / 3, p[2][0] = cx + r, p[2][1] = cy - r * 2 / 3;
        break;

      case 2:
        p[0][0] = cx - r, p[0][1] = cy, p[1][0] = cx + r * 2 / 3, p[1][1] = cy - r, p[2][0] = cx + r * 2 / 3, p[2][1] = cy + r;
        break;

      default:
        p[0][0] = cx + r, p[0][1] = cy, p[1][0] = cx - r * 2 / 3, p[1][1] = cy - r, p[2][0] = cx - r * 2 / 3, p[2][1] = cy + r;
        break;
      }

      auto edge = [](const float *a, const float *b, float x, float y)
      { return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]); };

      for (int y = std::max(cy - r, 0); y <= std::min(cy + r, dc->h - 1); ++y)
      {
        for (int x = std::max(cx - r, 0); x <= std::min(cx + r, dc->w - 1); ++x)
        {
          const float px = x + 0.5f, py = y + 0.5f;
          const float e0 = edge(p[0], p[1], px, py), e1 = edge(p[1], p[2], px, py), e2 = edge(p[2], p[0], px, py);

          if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))
            dc->Plot(x, y, 255);
        }
      }
    }

#endif

    struct KeyBox
    {
      int x0, y0, x1, y1; // in 1x pixels
      std::wstring label;
      int arrow = -1;
      bool filler = false; // unlabelled key (keyboard picture)
    };

    // Keys in the style of a keyboard: light key cap with a darker rim and bottom edge, dark legend. Filler keys (the keyboard picture's other keys) are greyer and blank
    Image DrawKeys(uint32_t w, uint32_t h, const std::vector<KeyBox> &keys)
    {
      auto radius = [](const KeyBox &k)
      { return std::max(3, std::min(k.x1 - k.x0, k.y1 - k.y0) / 4); };

      auto outer = Coverage(w, h, [&](HDC dc, int, int, int s)
                            { for (const KeyBox &k : keys) WhiteRoundRect(dc, k.x0 * s, k.y0 * s, k.x1 * s - 1, k.y1 * s - 1, radius(k) * s); });

      auto faces = [&](bool filler)
      {
        return Coverage(w, h, [&](HDC dc, int, int, int s)
                        {
          for (const KeyBox &k : keys)
          {
            if (k.filler != filler)
              continue;

            const int in = std::max(1, (k.y1 - k.y0) / 14) * s;

            // The face sits higher than the rim: a thicker edge at the bottom
            WhiteRoundRect(dc, k.x0 * s + in, k.y0 * s + in, k.x1 * s - 1 - in, k.y1 * s - 1 - in * 3, radius(k) * s);
          } });
      };

      auto face = faces(false), face_filler = faces(true);
      auto legend = Coverage(w, h, [&](HDC dc, int, int, int s)
                             {
        for (const KeyBox &k : keys)
        {
          if (k.filler)
            continue;

          const int in = std::max(1, (k.y1 - k.y0) / 14);
          const int x0 = (k.x0 + in) * s, y0 = (k.y0 + in) * s, x1 = (k.x1 - in) * s, y1 = (k.y1 - in * 3) * s;

          if (k.arrow >= 0)
            WhiteTriangle(dc, (x0 + x1) / 2, (y0 + y1) / 2, (y1 - y0) / 4, k.arrow);
          else
            WhiteText(dc, k.label, x0, y0, x1, y1);
        } });

      Image img;
      img.w = w, img.h = h;
      img.rgba.assign(size_t(w) * h * 4, 0);

      auto at = [](const Mask &m, size_t i)
      { return m.v.empty() ? 0.0f : m.v[i] / 255.0f; };

      for (uint32_t y = 0; y < h; ++y)
      {
        for (uint32_t x = 0; x < w; ++x)
        {
          const size_t i = size_t(y) * w + x;
          const float o = at(outer, i), f = at(face, i), ff = at(face_filler, i), l = at(legend, i);

          // rim grey -> face (slight top-to-bottom shading) -> legend
          const float t = float(y) / float(std::max(1u, h - 1));
          float c = 120.0f * std::max(0.0f, 1 - f - ff) + (248.0f - 22.0f * t) * f + (196.0f - 14.0f * t) * ff;
          c = c * (1 - l) + 28.0f * l;

          uint8_t *p = &img.rgba[i * 4];
          p[0] = p[1] = p[2] = uint8_t(std::clamp(c, 0.0f, 255.0f));
          p[3] = uint8_t(std::clamp(o * 255.0f, 0.0f, 255.0f));
        }
      }

      return img;
    }

    // The key bound to a pad button: the first key of the binding, named as on the keyboard
    std::wstring KeyLabel(const char *cvar)
    {
      std::string v = rex::cvar::Query<std::string>(cvar);
      v = v.substr(0, v.find(','));

      if (const size_t plus = v.rfind('+'); plus != std::string::npos && plus + 1 < v.size())
        v = v.substr(plus + 1);

      while (!v.empty() && v.front() == ' ')
        v.erase(v.begin());

      while (!v.empty() && v.back() == ' ')
        v.pop_back();

      static const std::map<std::string, std::string> kNames = {{"Return", "ENTER"}, {"Enter", "ENTER"}, {"Escape", "ESC"}, {"Space", "SPACE"}, {"Backspace", "BKSP"}, {"Tab", "TAB"}, {"Semicolon", ";"}, {"Quote", "'"}, {"Comma", ","}, {"Period", "."}, {"Slash", "/"}, {"Backslash", "\\"}, {"Minus", "-"}, {"Equals", "="}, {"LeftBracket", "["}, {"RightBracket", "]"}, {"Backtick", "`"}, {"Up", "UP"}, {"Down", "DOWN"}, {"Left", "LEFT"}, {"Right", "RIGHT"}, {"Shift", "SHIFT"}, {"Control", "CTRL"}, {"Ctrl", "CTRL"}, {"Alt", "ALT"}};

      if (auto it = kNames.find(v); it != kNames.end())
        v = it->second;

      std::wstring out;

      for (char c : v)
        out += wchar_t(std::toupper(static_cast<unsigned char>(c)));

      return out.empty() ? L"?" : out;
    }

    const char *BindingFor(Icon icon)
    {
      switch (icon)
      {
      case kA:
        return "keybind_a";

      case kB:
        return "keybind_b";

      case kX:
        return "keybind_x";

      case kY:
        return "keybind_y";

      case kStart:
        return "keybind_start";

      case kLB:
        return "keybind_left_shoulder";

      case kRB:
        return "keybind_right_shoulder";

      case kLT:
        return "keybind_left_trigger";

      case kRT:
        return "keybind_right_trigger";

      default:
        return nullptr;
      }
    }

    // The controller page's pad picture for the keyboard: a keyboard slab whose bound keys sit where the pad's controls are, so the page's lines and highlight rings still point at them (left stick: movement keys, D-pad: arrows, Back, Start, the four face buttons; positions from the Xbox 360 picture, 472 x 337), the rest of the slab filled with blank keys
    Image KeyboardPicture(uint32_t w, uint32_t h)
    {
      const float sx = float(w) / 472.0f, sy = float(h) / 337.0f;
      std::vector<KeyBox> keys;

      auto key = [&](float cx, float cy, float kw, float kh, std::wstring label, int arrow = -1)
      {
        keys.push_back({int((cx - kw / 2) * sx), int((cy - kh / 2) * sy), int((cx + kw / 2) * sx), int((cy + kh / 2) * sy), std::move(label), arrow});
      };

      constexpr float K = 34;
      key(90, 64, K, K, KeyLabel("keybind_lstick_up"));
      key(52, 100, K, K, KeyLabel("keybind_lstick_left"));
      key(90, 100, K, K, KeyLabel("keybind_lstick_down"));
      key(128, 100, K, K, KeyLabel("keybind_lstick_right"));
      key(160, 152, K, K, L"", 0);
      key(122, 188, K, K, L"", 2);
      key(160, 188, K, K, L"", 1);
      key(198, 188, K, K, L"", 3);
      key(180, 88, K, K, KeyLabel("keybind_back"));
      key(285, 88, 56, K, KeyLabel("keybind_start"));
      key(372, 50, K, K, KeyLabel("keybind_y"));
      key(333, 88, K, K, KeyLabel("keybind_x"));
      key(410, 88, K, K, KeyLabel("keybind_b"));
      key(372, 127, K, K, KeyLabel("keybind_a"));
      key(236, 270, 170, 30, KeyLabel("keybind_left_trigger"));

      // Blank keys in staggered rows wherever they fit
      const size_t bound = keys.size();
      const float slab_x0 = 8, slab_x1 = 464, slab_y0 = 18, slab_y1 = 300;

      for (int row = 0; row < 7; ++row)
      {
        const float cy = 50 + 38 * float(row);

        for (float cx = 30 + (row & 1) * 14.0f; cx < 460; cx += 38)
        {
          const float x0 = cx - K / 2, x1 = cx + K / 2, y0 = cy - K / 2, y1 = cy + K / 2;

          if (x0 < slab_x0 + 6 || x1 > slab_x1 - 6 || y0 < slab_y0 + 6 || y1 > slab_y1 - 6)
            continue;

          bool clear = true;

          for (size_t i = 0; i < bound && clear; ++i)
          {
            const KeyBox &b = keys[i];
            clear = x1 * sx + 3 <= b.x0 || x0 * sx - 3 >= b.x1 || y1 * sy + 3 <= b.y0 || y0 * sy - 3 >= b.y1;
          }

          if (clear)
          {
            key(cx, cy, K, K, L"");
            keys.back().filler = true;
          }
        }
      }

      const Image caps = DrawKeys(w, h, keys);

      // The slab under the keys: a light body with a darker rim
      const int r = int(30 * sx);

      auto slab_outer = Coverage(w, h, [&](HDC dc, int, int, int s)
                                 { WhiteRoundRect(dc, int(slab_x0 * sx) * s, int(slab_y0 * sy) * s, int(slab_x1 * sx) * s, int(slab_y1 * sy) * s, r * s); });

      auto slab_inner = Coverage(w, h, [&](HDC dc, int, int, int s)
                                 { WhiteRoundRect(dc, int((slab_x0 + 4) * sx) * s, int((slab_y0 + 3) * sy) * s, int((slab_x1 - 4) * sx) * s, int((slab_y1 - 7) * sy) * s, r * s); });
      Image img;
      img.w = w, img.h = h;
      img.rgba.assign(size_t(w) * h * 4, 0);

      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
          const size_t i = size_t(y) * w + x;
          const float o = slab_outer.v.empty() ? 0 : slab_outer.v[i] / 255.0f, in = slab_inner.v.empty() ? 0 : slab_inner.v[i] / 255.0f;
          const float t = float(y) / float(std::max(1u, h - 1));
          const float body = 120.0f * (1 - in) + (236.0f - 30.0f * t) * in;
          const uint8_t *k = &caps.rgba[i * 4];
          const float ka = k[3] / 255.0f;
          const float a = ka + o * (1 - ka);
          const float col = a > 1e-4f ? (k[0] * ka + body * o * (1 - ka)) / a : 0.0f;
          uint8_t *p = &img.rgba[i * 4];
          p[0] = p[1] = p[2] = uint8_t(std::clamp(col, 0.0f, 255.0f));
          p[3] = uint8_t(std::clamp(a * 255.0f, 0.0f, 255.0f));
        }

      return img;
    }
  }

  Image KeyboardIcon(Icon icon, uint32_t w, uint32_t h)
  {
    if (icon == kPadBody)
      return KeyboardPicture(w, h);

    if (icon == kPadStickA || icon == kPadStickB)
    {
      // The pad's stick overlays: hidden (blank)
      Image blank;
      blank.w = w, blank.h = h;
      blank.rgba.assign(size_t(w) * h * 4, 0);
      return blank;
    }

    if (icon == kDpad)
    {
      // The movement keys (W A S D by default; the arrows work too): up on top, left / down / right below
      const int kw = int(w) / 3, kh = int(h) / 2;
      const int top = (int(h) - 2 * kh) / 2, left = (int(w) - 3 * kw) / 2;
      return DrawKeys(w, h, {{left + kw, top, left + 2 * kw, top + kh, KeyLabel("keybind_lstick_up")}, {left, top + kh, left + kw, top + 2 * kh, KeyLabel("keybind_lstick_left")}, {left + kw, top + kh, left + 2 * kw, top + 2 * kh, KeyLabel("keybind_lstick_down")}, {left + 2 * kw, top + kh, left + 3 * kw, top + 2 * kh, KeyLabel("keybind_lstick_right")}});
    }

    const char *bind = BindingFor(icon);

    if (!bind)
      return {};

    const std::wstring label = KeyLabel(bind);

    // Wide keys for long names, square ones for single characters
    uint32_t kw = w, kh = h;

    if (label.size() <= 2)
      kw = kh = std::min(w, h);
    else
      kh = std::min(h, std::max<uint32_t>(w * 2 / 3, h * 3 / 4));

    const int x0 = int(w - kw) / 2, y0 = int(h - kh) / 2;
    return DrawKeys(w, h, {{x0, y0, x0 + int(kw), y0 + int(kh), label}});
  }

  uint64_t KeysHash()
  {
    uint64_t h = 0;

    for (int i = 0; i < kTargetCount; ++i)
      if (const char *b = BindingFor(Icon(i)))
      {
        const std::string v = rex::cvar::Query<std::string>(b);
        h = h * 1099511628211ull ^ XXH3_64bits(v.data(), v.size());
      }

    for (const char *b : {"keybind_lstick_up", "keybind_lstick_down", "keybind_lstick_left", "keybind_lstick_right", "keybind_back"})
    {
      const std::string v = rex::cvar::Query<std::string>(b);
      h = h * 1099511628211ull ^ XXH3_64bits(v.data(), v.size());
    }

    return h;
  }
}