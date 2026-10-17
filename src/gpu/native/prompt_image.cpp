// Native renderer: prompt pictures (BC2 blocks and image scaling)

#include "gpu/native/prompt_art.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sst::native::prompts
{
  namespace
  {
    void Color565(uint16_t c, int out[3])
    {
      out[0] = ((c >> 11) & 31) * 255 / 31;
      out[1] = ((c >> 5) & 63) * 255 / 63;
      out[2] = (c & 31) * 255 / 31;
    }

    uint16_t To565(const float c[3])
    {
      const int r = std::clamp(int(std::lround(c[0] * 31.0f / 255.0f)), 0, 31);
      const int g = std::clamp(int(std::lround(c[1] * 63.0f / 255.0f)), 0, 63);
      const int b = std::clamp(int(std::lround(c[2] * 31.0f / 255.0f)), 0, 31);
      return uint16_t((r << 11) | (g << 5) | b);
    }

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
    {
      if (px[4 * i + 3] >= 16)
      {
        for (int c = 0; c < 3; ++c)
          mean[c] += px[4 * i + c];

        n += 1;
      }
    }

    if (n == 0)
      return; // fully transparent: colour irrelevant

    for (float &m : mean)
      m /= n;

    // Main axis: the farthest pixel from the mean, refined once
    float axis[3] = {0, 0, 0}, best = -1;

    for (int i = 0; i < 16; ++i)
    {
      if (px[4 * i + 3] >= 16)
      {
        float d[3], l = 0;

        for (int c = 0; c < 3; ++c)
          d[c] = px[4 * i + c] - mean[c], l += d[c] * d[c];

        if (l > best)
          best = l, axis[0] = d[0], axis[1] = d[1], axis[2] = d[2];
      }
    }

    float lo = 0, hi = 0;
    const float al = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);

    if (al > 1e-3f)
    {
      for (float &v : axis)
        v /= al;

      lo = 1e9f, hi = -1e9f;

      for (int i = 0; i < 16; ++i)
      {
        if (px[4 * i + 3] >= 16)
        {
          const float t = (px[4 * i] - mean[0]) * axis[0] + (px[4 * i + 1] - mean[1]) * axis[1] + (px[4 * i + 2] - mean[2]) * axis[2];
          lo = std::min(lo, t), hi = std::max(hi, t);
        }
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
    {
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
    }

    return out;
  }
}