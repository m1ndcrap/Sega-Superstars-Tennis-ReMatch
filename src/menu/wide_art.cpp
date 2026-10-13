// sst_rematch - artwork for the ultrawide side areas

#include "menu/wide_art.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#ifndef SST_WIDE_ART_NO_REX
#include <rex/filesystem.h>
#include <rex/logging.h>
#endif

namespace sst::menu::art
{
  namespace
  {
    uint32_t Le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
    uint16_t Le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }

    // Xenos 2D tiled offset of block (x, y) in a page `pitch` blocks wide
    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bpb_log2)
    {
      pitch = (pitch + 31) & ~31u;
      const int32_t macro = int32_t(((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7));
      const int32_t micro = int32_t(((x & 7) + ((y & 0xE) << 2)) << bpb_log2);
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + int32_t((y & 1) << 4);
      return uint32_t(((offset & ~0x1FF) << 3) + int32_t((y & 16) << 7) + ((offset & 0x1C0) << 2) + int32_t((((((y & 8) >> 2) + (x >> 3)) & 3)) << 6) + (offset & 0x3F));
    }

    uint16_t To565(const float c[3])
    {
      auto q = [](float v, int bits)
      {
        const int m = (1 << bits) - 1;
        return std::clamp(int(v * m / 255.0f + 0.5f), 0, m);
      };

      return uint16_t((q(c[0], 5) << 11) | (q(c[1], 6) << 5) | q(c[2], 5));
    }

    void From565(uint16_t c, float out[3])
    {
      out[0] = float((c >> 11) & 31) * 255.0f / 31.0f;
      out[1] = float((c >> 5) & 63) * 255.0f / 63.0f;
      out[2] = float(c & 31) * 255.0f / 31.0f;
    }

    // One DXT3 block (standard little-endian layout) from 16 RGB pixels
    void EncodeBlock(const float px[16][3], uint8_t out[16])
    {
      std::memset(out, 0xFF, 8); // explicit alpha: opaque
      float lo[3], hi[3], mean[3] = {0, 0, 0};

      for (int c = 0; c < 3; ++c)
        lo[c] = 255.0f, hi[c] = 0.0f;

      for (int i = 0; i < 16; ++i)
      {
        for (int c = 0; c < 3; ++c)
        {
          lo[c] = std::min(lo[c], px[i][c]);
          hi[c] = std::max(hi[c], px[i][c]);
          mean[c] += px[i][c] / 16.0f;
        }
      }

      // Bounding box diagonal, flipped per channel to follow the colours' trend (green / blue against red), inset a little
      float cov_g = 0, cov_b = 0;

      for (int i = 0; i < 16; ++i)
      {
        const float r = px[i][0] - mean[0];
        cov_g += r * (px[i][1] - mean[1]);
        cov_b += r * (px[i][2] - mean[2]);
      }

      float a[3] = {hi[0], hi[1], hi[2]}, b[3] = {lo[0], lo[1], lo[2]};

      if (cov_g < 0)
        std::swap(a[1], b[1]);

      if (cov_b < 0)
        std::swap(a[2], b[2]);

      for (int c = 0; c < 3; ++c)
      {
        const float inset = (a[c] - b[c]) / 32.0f;
        a[c] -= inset;
        b[c] += inset;
      }

      uint16_t c0 = To565(a), c1 = To565(b);
      float p[4][3];
      From565(c0, p[0]);
      From565(c1, p[1]);

      for (int c = 0; c < 3; ++c)
      {
        p[2][c] = (2 * p[0][c] + p[1][c]) / 3.0f;
        p[3][c] = (p[0][c] + 2 * p[1][c]) / 3.0f;
      }

      uint32_t idx = 0;

      for (int i = 0; i < 16; ++i)
      {
        int best = 0;
        float best_d = 1e30f;

        for (int k = 0; k < 4; ++k)
        {
          float d = 0;

          for (int c = 0; c < 3; ++c)
            d += (px[i][c] - p[k][c]) * (px[i][c] - p[k][c]);

          if (d < best_d)
            best_d = d, best = k;
        }

        idx |= uint32_t(best) << (2 * i);
      }

      out[8] = uint8_t(c0), out[9] = uint8_t(c0 >> 8);
      out[10] = uint8_t(c1), out[11] = uint8_t(c1 >> 8);
      out[12] = uint8_t(idx), out[13] = uint8_t(idx >> 8), out[14] = uint8_t(idx >> 16),
      out[15] = uint8_t(idx >> 24);
    }

    void Log(const std::string &s, bool warn)
    {
#ifndef SST_WIDE_ART_NO_REX
      if (warn)
        REXLOG_WARN("[sst-menu] {}", s);
      else
        REXLOG_INFO("[sst-menu] {}", s);
#else
      (void)s, (void)warn;
#endif
    }

    std::filesystem::path ArtFolder()
    {
#ifndef SST_WIDE_ART_NO_REX
      return rex::filesystem::GetExecutableFolder() / "wide_backgrounds";
#else
      return "wide_backgrounds";
#endif
    }
  }

  bool LoadBmp(const std::string &path, Image &out, std::string &error)
  {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);

    if (!f)
      return error = "missing", false;

    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});

    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M')
      return error = "not a BMP", false;

    const uint32_t data = Le32(&d[10]), hdr = Le32(&d[14]);
    const int32_t w = int32_t(Le32(&d[18])), h_signed = int32_t(Le32(&d[22]));
    const uint16_t bpp = Le16(&d[28]);
    const uint32_t comp = Le32(&d[30]);

    if (hdr < 40 || (bpp != 24 && bpp != 32) || (comp != 0 && comp != 3) || w <= 0 || w > 4096 || h_signed == 0)
      return error = "unsupported BMP (24 or 32 bits, uncompressed)", false;

    const uint32_t h = uint32_t(std::abs(h_signed));
    const size_t row = ((size_t(w) * bpp / 8) + 3) & ~size_t(3);

    if (data + row * h > d.size())
      return error = "BMP truncated", false;

    out.w = uint32_t(w);
    out.h = h;
    out.rgb.resize(size_t(w) * h * 3);

    for (uint32_t y = 0; y < h; ++y)
    {
      const uint32_t src_y = h_signed > 0 ? h - 1 - y : y; // positive height: bottom-up
      const uint8_t *s = &d[data + row * src_y];
      uint8_t *o = &out.rgb[size_t(y) * w * 3];

      for (int32_t x = 0; x < w; ++x, s += bpp / 8, o += 3)
        o[0] = s[2], o[1] = s[1], o[2] = s[0];
    }

    return true;
  }

  Page EncodeDxt3(const Image &img)
  {
    Page p;
    p.w = (img.w + 127) & ~127u;
    p.h = (img.h + 127) & ~127u;
    p.data.assign(size_t(p.w) * p.h, 0); // DXT3: 1 byte per pixel
    const uint32_t bw = p.w / 4, bh = p.h / 4;

    for (uint32_t by = 0; by < bh; ++by)
    {
      for (uint32_t bx = 0; bx < bw; ++bx)
      {
        float px[16][3];

        for (int i = 0; i < 16; ++i)
        {
          // Outside the picture: its edge pixels (keeps filtering clean)
          const uint32_t x = std::min(bx * 4 + (i & 3), img.w - 1);
          const uint32_t y = std::min(by * 4 + (i >> 2), img.h - 1);
          const uint8_t *s = &img.rgb[(size_t(y) * img.w + x) * 3];
          px[i][0] = s[0], px[i][1] = s[1], px[i][2] = s[2];
        }

        uint8_t blk[16];
        EncodeBlock(px, blk);
        uint8_t *o = &p.data[TiledOffset(bx, by, bw, 4)];

        for (int i = 0; i < 16; i += 2)
          o[i] = blk[i + 1], o[i + 1] = blk[i]; // 8-in-16
      }
    }

    return p;
  }

  bool IsCourtBackground(const std::string &stem)
  {
    static const char *kCourts[] = {"cc", "hd", "js", "mb", "pu", "sh", "so", "vc"};

    for (const char *c : kCourts)
    {
      if (stem == std::string("bg_") + c + "loading")
        return true;
    }

    return false;
  }

  std::shared_ptr<const Sides> SidesFor(const std::string &stem)
  {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const Sides>> cache;
    std::lock_guard lock(mutex);

    if (auto it = cache.find(stem); it != cache.end())
      return it->second;

    std::shared_ptr<const Sides> result;

    // File names as the tool writes them: Bg_<court>loading_left.bmp
    std::string name = stem;

    if (name.size() > 1)
      name[0] = 'B';

    const std::filesystem::path dir = ArtFolder();
    Image l, r;
    std::string el, er;
    const bool have_l = LoadBmp((dir / (name + "_left.bmp")).string(), l, el);
    const bool have_r = LoadBmp((dir / (name + "_right.bmp")).string(), r, er);

    if (have_l && have_r)
    {
      if (l.w != r.w || l.h != r.h || l.h < 64 || l.w < 64)
      {
        Log(name + ": side pictures ignored (left and right must be the same size)", true);
      }
      else
      {
        auto s = std::make_shared<Sides>();
        s->left = EncodeDxt3(l);
        s->right = EncodeDxt3(r);
        s->art_w = l.w;
        s->art_h = l.h;
        Log(name + ": side pictures " + std::to_string(l.w) + "x" + std::to_string(l.h) + " from " + dir.string(), false);
        result = s;
      }
    }
    else if (have_l != have_r)
    {
      Log(name + ": side pictures ignored (" + (have_l ? "right: " + er : "left: " + el) + ")", true);
    }

    cache[stem] = result;
    return result;
  }
}