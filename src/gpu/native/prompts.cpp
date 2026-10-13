// sst_rematch - native renderer: button prompt icons for the device in use

#include "gpu/native/prompts.h"

#include "input/controls.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <xxhash.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

REXCVAR_DEFINE_BOOL(sst_native_prompts, true, "SST/Native", "Button prompt icons follow the device in use (PlayStation icons for PlayStation pads, keys for the keyboard)");

namespace sst::native::prompts
{
  namespace
  {
    enum Icon : uint8_t
    {
      // Xbox icons drawn by the game (targets)
      kA,
      kB,
      kX,
      kY,
      kStart,
      kDpad,
      kLB,
      kRB,
      kLT,
      kRT,
      kPadBody,   // the controller page's Xbox 360 pad picture
      kPadStickA, // its stick overlays
      kPadStickB,
      kTargetCount,
      // PlayStation art in the same atlases (sources)
      kPsCross = kTargetCount,
      kPsCircle,
      kPsSquare,
      kPsTriangle,
      kPsL1,
      kPsR1,
      kPsL2,
      kPsR2,
      kIconCount
    };

    struct Rect
    {
      Icon icon;
      uint16_t x0, y0, x1, y1; // pixels in the page (x0 / y0 on block boundaries)
    };

    struct Page
    {
      const char *name;
      uint64_t fingerprint; // XXH3-64 of the first 4 KB of the page's tiled data
      uint16_t width, height;
      std::vector<Rect> rects;
    };

    // From the scenes' PTEX entries (texture hash -> page, UV rectangle)
    const std::vector<Page> &Pages()
    {
      static const std::vector<Page> pages = {
          {"Global (menu navigation bar)", 0x3bcc3ebc77d56b53ull, 128, 128, {
                                                                                {kB, 0, 0, 38, 38},
                                                                                {kX, 40, 0, 78, 39},
                                                                                {kY, 80, 0, 118, 39},
                                                                                {kA, 0, 40, 39, 78},
                                                                                {kStart, 76, 40, 113, 71},
                                                                                {kDpad, 40, 40, 76, 77},
                                                                            }},
          {"SSTMainMenu (controller page)", 0x8da6680334ffecb8ull, 2048, 1024, {
                                                                                   {kB, 1956, 80, 1994, 118},
                                                                                   {kRT, 1712, 96, 1764, 148},
                                                                                   {kX, 1660, 148, 1698, 187},
                                                                                   {kLT, 1764, 92, 1816, 144},
                                                                                   {kLB, 1816, 92, 1868, 144},
                                                                                   {kA, 1780, 144, 1819, 182},
                                                                                   {kRB, 1660, 96, 1712, 148},
                                                                                   {kPadBody, 716, 0, 1188, 337},
                                                                                   {kPadStickA, 1660, 0, 1756, 96},
                                                                                   {kPadStickB, 1756, 0, 1847, 91},
                                                                                   {kPsSquare, 1996, 80, 2034, 119},
                                                                                   {kPsCircle, 1916, 80, 1954, 119},
                                                                                   {kPsCross, 1740, 148, 1778, 186},
                                                                                   {kPsTriangle, 1700, 148, 1738, 186},
                                                                                   {kPsL2, 1908, 0, 1968, 48},
                                                                                   {kPsL1, 1952, 48, 2012, 77},
                                                                                   {kPsR2, 1848, 0, 1908, 48},
                                                                                   {kPsR1, 1892, 48, 1952, 77},
                                                                               }},
          {"SpecHud (spectator HUD)", 0x73dbeacb47c3c410ull, 256, 256, {
                                                                           {kB, 48, 156, 86, 194},
                                                                           {kX, 88, 156, 126, 195},
                                                                           {kY, 128, 156, 166, 195},
                                                                           {kLB, 136, 104, 188, 156},
                                                                           {kRB, 84, 104, 136, 156},
                                                                           {kPsSquare, 0, 200, 38, 239},
                                                                           {kPsCircle, 208, 148, 246, 187},
                                                                           {kPsCross, 168, 156, 206, 194},
                                                                           {kPsTriangle, 40, 200, 78, 238},
                                                                       }},
          {"Hud (match HUD)", 0xc662c41aefb0357bull, 512, 512, {
                                                                   {kX, 148, 412, 186, 451},
                                                                   {kY, 108, 412, 146, 451},
                                                                   {kPsSquare, 268, 380, 306, 419},
                                                                   {kPsCircle, 308, 380, 346, 419},
                                                                   {kPsCross, 188, 412, 226, 450},
                                                                   {kPsTriangle, 228, 412, 266, 450},
                                                               }},
      };
      return pages;
    }

    struct Image
    {
      uint32_t w = 0, h = 0;
      std::vector<uint8_t> rgba; // straight alpha
      bool Empty() const { return rgba.empty(); }
    };

    // BC2 (DXT3) blocks

    void Color565(uint16_t c, int out[3])
    {
      out[0] = ((c >> 11) & 31) * 255 / 31;
      out[1] = ((c >> 5) & 63) * 255 / 63;
      out[2] = (c & 31) * 255 / 31;
    }

    void DecodeBC2(const uint8_t *b, uint8_t out[64])
    {
      const uint16_t c0 = uint16_t(b[8] | (b[9] << 8)), c1 = uint16_t(b[10] | (b[11] << 8));
      int p[4][3];
      Color565(c0, p[0]);
      Color565(c1, p[1]);

      for (int i = 0; i < 3; ++i)
      {
        p[2][i] = (2 * p[0][i] + p[1][i]) / 3;
        p[3][i] = (p[0][i] + 2 * p[1][i]) / 3;
      }

      const uint32_t idx = uint32_t(b[12]) | (uint32_t(b[13]) << 8) | (uint32_t(b[14]) << 16) | (uint32_t(b[15]) << 24);

      for (int i = 0; i < 16; ++i)
      {
        const int k = (idx >> (2 * i)) & 3;
        const int a = (b[i / 2] >> (4 * (i & 1))) & 15;
        out[4 * i + 0] = uint8_t(p[k][0]);
        out[4 * i + 1] = uint8_t(p[k][1]);
        out[4 * i + 2] = uint8_t(p[k][2]);
        out[4 * i + 3] = uint8_t(a * 17);
      }
    }

    uint16_t To565(const float c[3])
    {
      const int r = std::clamp(int(std::lround(c[0] * 31.0f / 255.0f)), 0, 31);
      const int g = std::clamp(int(std::lround(c[1] * 63.0f / 255.0f)), 0, 63);
      const int b = std::clamp(int(std::lround(c[2] * 31.0f / 255.0f)), 0, 31);
      return uint16_t((r << 11) | (g << 5) | b);
    }

    // 16 RGBA pixels -> BC2: explicit 4-bit alpha; colours from the extremes along the block's main colour axis (opaque-ish pixels only)
    void EncodeBC2(const uint8_t px[64], uint8_t out[16])
    {
      std::memset(out, 0, 16);

      for (int i = 0; i < 16; ++i)
      {
        const int a = (px[4 * i + 3] + 8) / 17;
        out[i / 2] |= uint8_t(std::min(a, 15) << (4 * (i & 1)));
      }

      float mean[3] = {}, n = 0;

      for (int i = 0; i < 16; ++i)
        if (px[4 * i + 3] >= 16)
        {
          for (int c = 0; c < 3; ++c)
            mean[c] += px[4 * i + c];

          n += 1;
        }

      if (n == 0)
        return; // fully transparent: colour irrelevant

      for (float &m : mean)
        m /= n;

      // Main axis: the farthest pixel from the mean, refined once
      float axis[3] = {0, 0, 0}, best = -1;

      for (int i = 0; i < 16; ++i)
        if (px[4 * i + 3] >= 16)
        {
          float d[3], l = 0;

          for (int c = 0; c < 3; ++c)
            d[c] = px[4 * i + c] - mean[c], l += d[c] * d[c];

          if (l > best)
            best = l, axis[0] = d[0], axis[1] = d[1], axis[2] = d[2];
        }

      float lo = 0, hi = 0;
      const float al = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);

      if (al > 1e-3f)
      {
        for (float &v : axis)
          v /= al;

        lo = 1e9f, hi = -1e9f;

        for (int i = 0; i < 16; ++i)
          if (px[4 * i + 3] >= 16)
          {
            const float t = (px[4 * i] - mean[0]) * axis[0] + (px[4 * i + 1] - mean[1]) * axis[1] + (px[4 * i + 2] - mean[2]) * axis[2];
            lo = std::min(lo, t), hi = std::max(hi, t);
          }
      }

      float e0[3], e1[3];

      for (int c = 0; c < 3; ++c)
        e0[c] = mean[c] + axis[c] * hi, e1[c] = mean[c] + axis[c] * lo;

      const uint16_t c0 = To565(e0), c1 = To565(e1);
      out[8] = uint8_t(c0), out[9] = uint8_t(c0 >> 8), out[10] = uint8_t(c1), out[11] = uint8_t(c1 >> 8);
      int pal[4][3];
      Color565(c0, pal[0]);
      Color565(c1, pal[1]);

      for (int c = 0; c < 3; ++c)
      {
        pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
        pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
      }

      uint32_t idx = 0;

      for (int i = 0; i < 16; ++i)
      {
        int bk = 0, bd = INT32_MAX;

        for (int k = 0; k < 4; ++k)
        {
          int d = 0;

          for (int c = 0; c < 3; ++c)
            d += (px[4 * i + c] - pal[k][c]) * (px[4 * i + c] - pal[k][c]);

          if (d < bd)
            bd = d, bk = k;
        }

        idx |= uint32_t(bk) << (2 * i);
      }

      out[12] = uint8_t(idx), out[13] = uint8_t(idx >> 8), out[14] = uint8_t(idx >> 16), out[15] = uint8_t(idx >> 24);
    }

    // images

    // Bilinear sample with premultiplied alpha (no dark fringes)
    void Sample(const Image &src, float x, float y, float out[4])
    {
      x = std::clamp(x - 0.5f, 0.0f, float(src.w - 1));
      y = std::clamp(y - 0.5f, 0.0f, float(src.h - 1));
      const uint32_t x0 = uint32_t(x), y0 = uint32_t(y), x1 = std::min(x0 + 1, src.w - 1), y1 = std::min(y0 + 1, src.h - 1);
      const float fx = x - float(x0), fy = y - float(y0);
      const float wts[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
      const uint32_t xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
      float acc[4] = {};

      for (int k = 0; k < 4; ++k)
      {
        const uint8_t *p = &src.rgba[(size_t(ys[k]) * src.w + xs[k]) * 4];
        const float a = p[3] / 255.0f * wts[k];

        for (int c = 0; c < 3; ++c)
          acc[c] += p[c] * a;

        acc[3] += a;
      }

      for (int c = 0; c < 3; ++c)
        out[c] = acc[3] > 1e-5f ? acc[c] / acc[3] : 0.0f;

      out[3] = acc[3] * 255.0f;
    }

    // src fitted (aspect kept, centred) into a w x h box
    Image Fit(const Image &src, uint32_t w, uint32_t h)
    {
      Image out;
      out.w = w, out.h = h;
      out.rgba.assign(size_t(w) * h * 4, 0);

      if (src.Empty())
        return out;

      const float s = std::min(float(w) / float(src.w), float(h) / float(src.h));
      const float dw = float(src.w) * s, dh = float(src.h) * s;
      const float ox = (float(w) - dw) * 0.5f, oy = (float(h) - dh) * 0.5f;
      // Downscaling more than 2x: supersample
      const int ss = s < 0.5f ? 4 : 2;

      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
          float acc[4] = {}, wa = 0;

          for (int sy = 0; sy < ss; ++sy)
            for (int sx = 0; sx < ss; ++sx)
            {
              const float px = (float(x) + (sx + 0.5f) / ss - ox) / s, py = (float(y) + (sy + 0.5f) / ss - oy) / s;

              if (px < 0 || py < 0 || px >= float(src.w) || py >= float(src.h))
                continue;

              float v[4];
              Sample(src, px, py, v);
              const float a = v[3] / 255.0f;

              for (int c = 0; c < 3; ++c)
                acc[c] += v[c] * a;

              acc[3] += v[3];
              wa += a;
            }

          uint8_t *o = &out.rgba[(size_t(y) * w + x) * 4];

          for (int c = 0; c < 3; ++c)
            o[c] = uint8_t(std::clamp(wa > 1e-5f ? acc[c] / wa : 0.0f, 0.0f, 255.0f));

          o[3] = uint8_t(std::clamp(acc[3] / float(ss * ss), 0.0f, 255.0f));
        }

      return out;
    }

    // keyboard keys (GDI, drawn at 4x and filtered down)

    struct Mask
    {
      uint32_t w = 0, h = 0;
      std::vector<uint8_t> v; // coverage at 1x
    };

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
        for (uint32_t x = 0; x < w; ++x)
        {
          uint32_t sum = 0;

          for (int sy = 0; sy < kS; ++sy)
            for (int sx = 0; sx < kS; ++sx)
              sum += p[(size_t(y * kS + sy) * W + (x * kS + sx)) * 4 + 1]; // green channel

          m.v[size_t(y) * w + x] = uint8_t(sum / (kS * kS));
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
                            {
        for (const KeyBox& k : keys)
          WhiteRoundRect(dc, k.x0 * s, k.y0 * s, k.x1 * s - 1, k.y1 * s - 1, radius(k) * s); });
      auto faces = [&](bool filler)
      {
        return Coverage(w, h, [&](HDC dc, int, int, int s)
                        {
          for (const KeyBox& k : keys)
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
        for (const KeyBox& k : keys)
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

      static const std::map<std::string, std::string> kNames = {
          {"Return", "ENTER"}, {"Enter", "ENTER"}, {"Escape", "ESC"}, {"Space", "SPACE"}, {"Backspace", "BKSP"}, {"Tab", "TAB"}, {"Semicolon", ";"}, {"Quote", "'"}, {"Comma", ","}, {"Period", "."}, {"Slash", "/"}, {"Backslash", "\\"}, {"Minus", "-"}, {"Equals", "="}, {"LeftBracket", "["}, {"RightBracket", "]"}, {"Backtick", "`"}, {"Up", "UP"}, {"Down", "DOWN"}, {"Left", "LEFT"}, {"Right", "RIGHT"}, {"Shift", "SHIFT"}, {"Control", "CTRL"}, {"Ctrl", "CTRL"}, {"Alt", "ALT"}};

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

    // state

    std::array<Image, kIconCount> g_art; // captured PlayStation art (decoded)
    std::array<std::vector<uint8_t>, kIconCount> g_capture; // blocks being captured (host BC2)
    uint32_t g_art_generation = 0;

    // Replacement blocks for the page being uploaded: (bx, by) -> block
    struct Prepared
    {
      int page = -1;
      uint32_t generation = 0;
      std::unordered_map<uint32_t, std::array<uint8_t, 16>> blocks;
    };

    Prepared g_prepared;
    std::vector<uint32_t> g_logged_pages;

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

    Icon PlayStationFor(Icon icon)
    {
      switch (icon)
      {
      case kA:
        return kPsCross;
      case kB:
        return kPsCircle;
      case kX:
        return kPsSquare;
      case kY:
        return kPsTriangle;
      case kLB:
        return kPsL1;
      case kRB:
        return kPsR1;
      case kLT:
        return kPsL2;
      case kRT:
        return kPsR2;
      default:
        return kIconCount; // Start, D-pad: the same on both
      }
    }

    input::PromptStyle Style()
    {
      return REXCVAR_GET(sst_native_prompts) ? input::CurrentPromptStyle() : input::PromptStyle::kXbox;
    }
  }

  int MatchPage(const uint8_t *data, uint32_t width, uint32_t height)
  {
    const auto &pages = Pages();
    bool size_known = false;

    for (const Page &p : pages)
      size_known |= p.width == width && p.height == height;

    if (!size_known)
      return -1;

    const uint64_t fp = XXH3_64bits(data, 4096);

    for (size_t i = 0; i < pages.size(); ++i)
      if (pages[i].fingerprint == fp && pages[i].width == width && pages[i].height == height)
      {
        if (std::find(g_logged_pages.begin(), g_logged_pages.end(), uint32_t(i)) == g_logged_pages.end())
        {
          g_logged_pages.push_back(uint32_t(i));
          REXLOG_INFO("[sst-native] prompts: icon page {} found", pages[i].name);
        }

        return int(i);
      }

    return -1;
  }

  const char *PageName(int page)
  {
    return page >= 0 && size_t(page) < Pages().size() ? Pages()[page].name : "?";
  }

  uint32_t Generation()
  {
    const auto style = Style();
    uint64_t h = (uint64_t(style) << 32) ^ g_art_generation;

    if (style == input::PromptStyle::kKeyboard)
      h ^= KeysHash();

    return uint32_t(h ^ (h >> 32)) | 1u; // never 0 (0 = not uploaded yet)
  }

  void BeginPage(int page)
  {
    const auto &pages = Pages();

    if (page < 0 || size_t(page) >= pages.size())
      return;

    const Page &p = pages[page];

    // Sources: blocks collected while this upload runs
    for (const Rect &r : p.rects)
      if (r.icon >= kTargetCount && g_art[r.icon].Empty())
        g_capture[r.icon].assign(size_t((r.x1 - r.x0 + 3) / 4) * ((r.y1 - r.y0 + 3) / 4) * 16, 0);

    const uint32_t gen = Generation();

    if (g_prepared.page == page && g_prepared.generation == gen)
      return;

    g_prepared.page = page;
    g_prepared.generation = gen;
    g_prepared.blocks.clear();
    const auto style = Style();

    if (style == input::PromptStyle::kXbox)
      return;

    for (const Rect &r : p.rects)
    {
      if (r.icon >= kTargetCount)
        continue;

      const uint32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
      Image icon;

      if (style == input::PromptStyle::kPlayStation)
      {
        const Icon src = PlayStationFor(r.icon);

        if (src == kIconCount || g_art[src].Empty())
          continue;

        icon = Fit(g_art[src], w, h);
      }
      else
      {
        icon = KeyboardIcon(r.icon, w, h);
      }

      if (icon.Empty())
        continue;

      // The icon's cells, block-aligned (pixels past its rectangle stay transparent)
      const uint32_t bx0 = r.x0 / 4, by0 = r.y0 / 4, bx1 = (r.x1 + 3) / 4, by1 = (r.y1 + 3) / 4;

      for (uint32_t by = by0; by < by1; ++by)
        for (uint32_t bx = bx0; bx < bx1; ++bx)
        {
          uint8_t px[64] = {};

          for (uint32_t j = 0; j < 4; ++j)
            for (uint32_t i = 0; i < 4; ++i)
            {
              const uint32_t x = bx * 4 + i, y = by * 4 + j;

              if (x < r.x0 || y < r.y0 || x >= r.x0 + w || y >= r.y0 + h)
                continue;

              std::memcpy(&px[(j * 4 + i) * 4], &icon.rgba[(size_t(y - r.y0) * w + (x - r.x0)) * 4], 4);
            }

          std::array<uint8_t, 16> blk;
          EncodeBC2(px, blk.data());
          g_prepared.blocks[(by << 16) | bx] = blk;
        }
    }
  }

  void Block(int page, uint32_t bx, uint32_t by, uint8_t *block)
  {
    const auto &pages = Pages();

    if (page < 0 || size_t(page) >= pages.size())
      return;

    for (const Rect &r : pages[page].rects)
    {
      if (r.icon < kTargetCount || g_capture[r.icon].empty())
        continue;

      const uint32_t bx0 = r.x0 / 4, by0 = r.y0 / 4, bw = (r.x1 - r.x0 + 3) / 4, bh = (r.y1 - r.y0 + 3) / 4;

      if (bx >= bx0 && by >= by0 && bx < bx0 + bw && by < by0 + bh)
        std::memcpy(&g_capture[r.icon][((by - by0) * bw + (bx - bx0)) * 16], block, 16);
    }

    if (g_prepared.page == page)
      if (auto it = g_prepared.blocks.find((by << 16) | bx); it != g_prepared.blocks.end())
        std::memcpy(block, it->second.data(), 16);
  }

  void EndPage(int page)
  {
    const auto &pages = Pages();

    if (page < 0 || size_t(page) >= pages.size())
      return;

    bool added = false;

    for (const Rect &r : pages[page].rects)
    {
      if (r.icon < kTargetCount || g_capture[r.icon].empty())
        continue;

      const uint32_t w = r.x1 - r.x0, h = r.y1 - r.y0, bw = (w + 3) / 4;
      Image img;
      img.w = w, img.h = h;
      img.rgba.assign(size_t(w) * h * 4, 0);

      for (uint32_t by = 0; by < (h + 3) / 4; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
          uint8_t px[64];
          DecodeBC2(&g_capture[r.icon][(by * bw + bx) * 16], px);

          for (uint32_t j = 0; j < 4; ++j)
            for (uint32_t i = 0; i < 4; ++i)
            {
              const uint32_t x = bx * 4 + i, y = by * 4 + j;

              if (x < w && y < h)
                std::memcpy(&img.rgba[(size_t(y) * w + x) * 4], &px[(j * 4 + i) * 4], 4);
            }
        }

      g_capture[r.icon].clear();
      g_art[r.icon] = std::move(img);
      added = true;
    }

    if (added)
    {
      ++g_art_generation;
      REXLOG_INFO("[sst-native] prompts: PlayStation icons taken from {}", pages[page].name);
    }
  }
}