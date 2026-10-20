// Native renderer: button prompt icons for the device in use. The game draws Xbox icons from a few texture pages; when such a page is uploaded the icons are replaced block by block: PlayStation icons (taken from the PlayStation art the same pages hold) or keyboard keys (prompt_keys.cpp)

#include "gpu/native/prompts.h"
#include "gpu/native/prompt_art.h"

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

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(sst_native_prompts, true, "SST/Native", "Button prompt icons follow the device in use (PlayStation icons for PlayStation pads, keys for the keyboard)");

namespace sst::native::prompts
{
  namespace
  {
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

    std::array<Image, kIconCount> g_art;                    // captured PlayStation art (decoded)
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
    {
      if (pages[i].fingerprint == fp && pages[i].width == width && pages[i].height == height)
      {
        if (std::find(g_logged_pages.begin(), g_logged_pages.end(), uint32_t(i)) == g_logged_pages.end())
        {
          g_logged_pages.push_back(uint32_t(i));
          REXLOG_INFO("[sst-native] prompts: icon page {} found", pages[i].name);
        }

        return int(i);
      }
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
    {
      if (r.icon >= kTargetCount && g_art[r.icon].Empty())
        g_capture[r.icon].assign(size_t((r.x1 - r.x0 + 3) / 4) * ((r.y1 - r.y0 + 3) / 4) * 16, 0);
    }

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
      {
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
    {
      if (auto it = g_prepared.blocks.find((by << 16) | bx); it != g_prepared.blocks.end())
        std::memcpy(block, it->second.data(), 16);
    }
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
      {
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
          uint8_t px[64];
          DecodeBC2(&g_capture[r.icon][(by * bw + bx) * 16], px);

          for (uint32_t j = 0; j < 4; ++j)
          {
            for (uint32_t i = 0; i < 4; ++i)
            {
              const uint32_t x = bx * 4 + i, y = by * 4 + j;

              if (x < w && y < h)
                std::memcpy(&img.rgba[(size_t(y) * w + x) * 4], &px[(j * 4 + i) * 4], 4);
            }
          }
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