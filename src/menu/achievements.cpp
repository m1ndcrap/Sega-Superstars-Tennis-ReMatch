// In-game achievements

#include "menu/achievements.h"
#include "menu/guest.h"
#include "menu/scene_patches.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/ui/image_decode.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(sst_achievements_ui, true, "SST", "Achievements in the game's own style: Options > ACHIEVEMENTS list and unlock popups (instead of the SDK's overlay and toasts)");
REXCVAR_DEFINE_INT32(sst_achievement_popup_ms, 4000, "SST", "How long an achievement popup stays on screen (milliseconds)");
REXCVAR_DEFINE_STRING(sst_achievement_popup_sound, "Start", "SST", "Sound the achievement popup plays: one of the menu sounds (Start, Select, Move, Back), a sound hash (#1234ABCD), or none");
REXCVAR_DEFINE_BOOL(sst_achievement_popup_test, false, "SST", "Test: show the first achievement's popup once the game is up (nothing is unlocked)");

// Scene natives (thin wrappers around the scene manager 0x82DB4370): r5 values, r6 types, r7 count, r3 result
extern "C" void sub_82282830(PPCContext &__restrict, uint8_t *); // 4:2 enter page (page)
extern "C" void sub_82282888(PPCContext &__restrict, uint8_t *); // 4:3 leave page (page)
extern "C" void sub_82282BB8(PPCContext &__restrict, uint8_t *); // 4:4 show (object)
extern "C" void sub_82282C28(PPCContext &__restrict, uint8_t *); // 4:5 hide (object)
extern "C" void sub_82282AA0(PPCContext &__restrict, uint8_t *); // 4:10 play keys (object, from, to, loop)
extern "C" void sub_82283210(PPCContext &__restrict, uint8_t *); // 4:15 set text (object, text id)
extern "C" void sub_82283318(PPCContext &__restrict, uint8_t *); // 4:18 alpha (object, 0-255)
extern "C" void sub_82287EB8(PPCContext &__restrict, uint8_t *); // 7:1 play sound (name hash)

namespace sst::ach
{
  namespace
  {
#include "menu/achievements_script.inc"

    // object hashes (the scene patches add them; checked unused there)
    constexpr uint32_t kList = 0x53410001, kSummary = 0x53410002, kListPage = 0x53410000;
    constexpr uint32_t kRowGroup = 0x53411000, kRowPanel = 0x53412000, kRowIcon = 0x53413000, kRowIconLocked = 0x53414000;
    constexpr uint32_t kRowName = 0x53415000, kRowDesc = 0x53416000, kRowScore = 0x53417000;
    constexpr uint32_t kTexture = 0x53418000; // + slot
    constexpr uint32_t kPopupPage = 0x5341E000, kPopupGroup = 0x5341E001, kPopupBar = 0x5341E003;
    constexpr uint32_t kPopupHeader = 0x5341E004, kPopupName = 0x5341E005;
    constexpr uint32_t kPopupPanels[3] = {0x5341E002, 0x5341E00A, 0x5341E00B}; // for 1 / 2 / 3 description lines
    constexpr uint32_t kPopupDesc[3] = {0x5341E007, 0x5341E008, 0x5341E009};   // description lines
    constexpr uint32_t kPopupIcon = 0x5341E010;                                // + slot
    constexpr uint32_t kPopupHide = 0x5341E100, kPopupShow = 0x5341E101;

    // text ids
    constexpr uint32_t kTextBlank = 0x5341F000;
    constexpr uint32_t kTextName = 0x5341A000, kTextDesc = 0x5341B000;    // + row
    constexpr uint32_t kTextDate = 0x5341C000, kTextDateAlt = 0x5341D000; // + row: the unlock date (alternating with each page opening, so changed texts redraw)
    constexpr uint32_t kTextSummary = 0x5341F010;                         // + 0 / 1 (alternating: a new id makes the text redraw)
    constexpr uint32_t kTextPopupHeader = 0x5341F020;
    constexpr uint32_t kTextPopupName = 0x5341F021; // + 0 / 1
    constexpr uint32_t kTextPopupDesc = 0x5341F030; // + line x 2 + 0 / 1
    constexpr uint32_t kPopupFont = 0x69995503;     // MsgBox body font (its advances wrap the description)

    // scripts (lookup2)
    constexpr uint32_t kScriptAchievements = 0xDB57BE7A, kScriptRecords = 0x92CC7DA7, kScriptMainMenu = 0x9E754704;

    // icons: slots of 64 x 64 in one 512 x 64 page; slots 0-3 the pictures, 4-7 the same greyed (locked)
    constexpr uint32_t kIcon = 64, kSlots = 8, kLockedOffset = 4;
    constexpr uint32_t kPageW = kIcon * kSlots, kPageH = kIcon, kPageBytes = kPageW * kPageH * 4;
    constexpr uint32_t kPoolSize = 0x10000, kPopupText = 0x100;
    constexpr int kVisibleRows = 5;

    struct Entry
    {
      uint32_t id = 0;
      std::string name, desc, score;
      uint32_t gamerscore = 0;
      int slot = 0;
      float desc_scale = 0.85f;
    };

    struct Gpu
    {
      uint32_t offset = 0;  // pixels' place in the file's buffer
      bool written = false; // the last read of the file got them
    };

    struct State
    {
      std::recursive_mutex mutex; // (scene natives called under it resolve texts through ResolveText)
      rex::system::AchievementManager *manager = nullptr;
      bool loaded = false;
      std::vector<Entry> list;
      std::vector<uint8_t> page; // ARGB, kPageW x kPageH

      // guest memory: strings, popup text buffers, native call scratch
      uint32_t pool = 0, pool_used = 0, scratch = 0;
      std::unordered_map<uint32_t, uint32_t> texts; // text id -> guest string
      std::unordered_map<std::string, Gpu> gpu;     // ".gpu" file name -> icon page
      bool list_ready = false, popup_ready = false;
      bool summary_alt = false;

      // popups
      std::deque<uint32_t> queue;

      enum class Popup
      {
        kIdle,
        kShown,
        kLeaving
      } popup = Popup::kIdle;

      std::chrono::steady_clock::time_point since;
      bool popup_alt = false;

      // the page script in guest memory; the game's module it replaces
      uint32_t script = 0, original = 0;
      bool tested = false;  // sst_achievement_popup_test done
      bool menu_up = false; // the main menu has started once
      bool frames_seen = false;
      uint32_t front_end = 0; // the script front end (its directory holds the modules)
      uint32_t font_file = 0; // msgbox_en.dat in guest memory (the popup's font)
      std::vector<uint16_t> advances;
      uint32_t menu_module = 0, menu_site = 0; // the main menu module, its ACHIEVEMENTS action's 15:82 call
    };

    State g;

    // Bob Jenkins' lookup2 (initval 0x04C11DB7), the game's name hash
    uint32_t Lookup2(const std::string &key)
    {
      auto mix = [](uint32_t &a, uint32_t &b, uint32_t &c)
      {
        a -= b, a -= c, a ^= c >> 13;
        b -= c, b -= a, b ^= a << 8;
        c -= a, c -= b, c ^= b >> 13;
        a -= b, a -= c, a ^= c >> 12;
        b -= c, b -= a, b ^= a << 16;
        c -= a, c -= b, c ^= b >> 5;
        a -= b, a -= c, a ^= c >> 3;
        b -= c, b -= a, b ^= a << 10;
        c -= a, c -= b, c ^= b >> 15;
      };

      const uint8_t *k = reinterpret_cast<const uint8_t *>(key.data());
      uint32_t len = uint32_t(key.size()), a = 0x9E3779B9u, b = 0x9E3779B9u, c = 0x04C11DB7u;

      auto le = [](const uint8_t *p)
      { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); };

      while (len >= 12)
      {
        a += le(k), b += le(k + 4), c += le(k + 8);
        mix(a, b, c);
        k += 12, len -= 12;
      }

      c += uint32_t(key.size());

      switch (len)
      {
      case 11:
        c += uint32_t(k[10]) << 24;
        [[fallthrough]];

      case 10:
        c += uint32_t(k[9]) << 16;
        [[fallthrough]];

      case 9:
        c += uint32_t(k[8]) << 8;
        [[fallthrough]];

      case 8:
        b += uint32_t(k[7]) << 24;
        [[fallthrough]];

      case 7:
        b += uint32_t(k[6]) << 16;
        [[fallthrough]];

      case 6:
        b += uint32_t(k[5]) << 8;
        [[fallthrough]];

      case 5:
        b += k[4];
        [[fallthrough]];

      case 4:
        a += uint32_t(k[3]) << 24;
        [[fallthrough]];

      case 3:
        a += uint32_t(k[2]) << 16;
        [[fallthrough]];

      case 2:
        a += uint32_t(k[1]) << 8;
        [[fallthrough]];

      case 1:
        a += k[0];
        [[fallthrough]];

      default:
        break;
      }

      mix(a, b, c);
      return c;
    }

    using sst::menu::FindScriptNode;
    using sst::menu::GuestFileName;
    using sst::menu::GuestPtr;
    using sst::menu::Load32;
    using sst::menu::Store32;

    bool Enabled()
    {
      return REXCVAR_GET(sst_achievements_ui);
    }

    // ASCII only (the game's fonts): other characters become '?'; line breaks become spaces; trimmed
    std::string Clean(const std::string &s)
    {
      std::string out;

      for (size_t i = 0; i < s.size(); ++i)
      {
        const uint8_t c = uint8_t(s[i]);

        if (c == '\n' || c == '\r' || c == '\t')
          out += ' ';
        else if (c < 0x80)
          out += char(c);
        else if ((c & 0xC0) != 0x80) // first byte of a UTF-8 sequence
          out += '?';
      }

      const size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
      return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
    }

    // Guest memory for strings (once). Under g.mutex
    bool SetupGuest(uint8_t *base)
    {
      if (g.pool)
        return true;

      auto *memory = rex::system::kernel_memory();
      const uint32_t block = memory ? memory->SystemHeapAlloc(kPoolSize + 64) : 0;

      if (!block)
        return false;

      g.scratch = block;
      g.pool = block + 64;
      g.pool_used = 0;
      (void)base;
      return true;
    }

    // A text in the pool (fixed: `room` bytes, rewritable). Under g.mutex
    uint32_t PutText(uint8_t *base, uint32_t id, const std::string &text, uint32_t room = 0)
    {
      auto it = g.texts.find(id);
      const uint32_t n = uint32_t(text.size()) + 1;

      if (it == g.texts.end())
      {
        const uint32_t size = std::max(room, n);

        if (!g.pool || g.pool_used + size > kPoolSize)
          return 0;

        it = g.texts.emplace(id, g.pool + g.pool_used).first;
        g.pool_used += (size + 3) & ~3u;
      }
      else if (room && n > room)
      {
        return it->second;
      }

      std::memcpy(GuestPtr(base, it->second), text.c_str(), std::min(n, room ? room : n));
      return it->second;
    }

    // The catalogue (names, descriptions, gamerscore) and the icon page. Under g.mutex
    void Load()
    {
      if (g.loaded)
        return;

      auto *ks = rex::system::kernel_state();

      if (!ks)
        return;

      auto infos = (g.manager ? *g.manager : ks->achievements()).ListAchievements();

      if (infos.empty())
        return;

      g.loaded = true;

      std::sort(infos.begin(), infos.end(), [](const auto &a, const auto &b)
                { return a.id < b.id; });

      g.page.assign(kPageBytes, 0);
      std::map<uint32_t, int> slots; // XDBF image id -> slot
      const auto xdbf = ks->title_xdbf();

      for (const auto &a : infos)
      {
        Entry e;
        e.id = a.id;
        e.name = Clean(a.label);
        e.desc = Clean(a.description.empty() ? a.unachieved_description : a.description);
        e.gamerscore = a.gamerscore;
        e.score = fmt::format("{}G", a.gamerscore);

        // Descriptions in the records' font (about 12.3 px a character): at most 0.85, smaller when long
        const float est = float(e.desc.size()) * 12.3f;
        e.desc_scale = est > 0.0f ? std::min(0.85f, 900.0f / est) : 0.85f;

        auto it = slots.find(a.image_id);

        if (it == slots.end() && slots.size() < kLockedOffset && xdbf.is_valid())
        {
          const auto block = xdbf.GetEntry(rex::system::util::XdbfSection::kImage, a.image_id);
          int w = 0, h = 0;
          std::vector<uint8_t> rgba = block ? rex::ui::DecodeImageRGBA(block.buffer, block.size, w, h) : std::vector<uint8_t>();

          if (!rgba.empty() && w > 0 && h > 0)
          {
            const int slot = int(slots.size());
            it = slots.emplace(a.image_id, slot).first;

            // Scaled (nearest) into the slot; the locked copy greyed and darker. Memory order A, R, G, B (k_8_8_8_8, 8-in-32)
            for (uint32_t y = 0; y < kIcon; ++y)
            {
              for (uint32_t x = 0; x < kIcon; ++x)
              {
                const uint8_t *s = &rgba[(size_t(y * uint32_t(h) / kIcon) * uint32_t(w) + x * uint32_t(w) / kIcon) * 4];
                uint8_t *d = &g.page[(size_t(y) * kPageW + slot * kIcon + x) * 4];
                d[0] = s[3], d[1] = s[0], d[2] = s[1], d[3] = s[2];
                const uint8_t grey = uint8_t((s[0] * 77 + s[1] * 150 + s[2] * 29) / 256 * 55 / 100);
                uint8_t *l = &g.page[(size_t(y) * kPageW + (slot + kLockedOffset) * kIcon + x) * 4];
                l[0] = s[3], l[1] = grey, l[2] = grey, l[3] = grey;
              }
            }
          }
        }

        e.slot = it == slots.end() ? 0 : it->second;
        g.list.push_back(std::move(e));
      }

      uint32_t total = 0;

      for (const Entry &e : g.list)
        total += e.gamerscore;

      REXLOG_INFO("[sst-ach] {} achievements ({} G), {} icons", g.list.size(), total, slots.size());
    }

    // Row texts and the fixed ones (once the guest pool exists). Under g.mutex
    void PutTexts(uint8_t *base)
    {
      if (!SetupGuest(base) || g.texts.count(kTextBlank))
        return;

      PutText(base, kTextBlank, " ");
      PutText(base, kTextPopupHeader, "ACHIEVEMENT UNLOCKED");

      for (size_t i = 0; i < g.list.size(); ++i)
      {
        PutText(base, kTextName + uint32_t(i), g.list[i].name.empty() ? " " : g.list[i].name);
        PutText(base, kTextDesc + uint32_t(i), g.list[i].desc.empty() ? " " : g.list[i].desc);
      }
    }

    // Calls a scene native directly, as the script dispatcher would
    void Native(PPCContext &ctx, uint8_t *base, void (*fn)(PPCContext &__restrict, uint8_t *), std::initializer_list<std::pair<uint32_t, uint8_t>> args)
    {
      if (!g.scratch)
        return;

      // scratch: +0 result (8), +8 values (8 x 4), +40 types (8)
      std::memset(GuestPtr(base, g.scratch), 0, 48);
      uint32_t i = 0;

      for (auto [value, type] : args)
      {
        Store32(base, g.scratch + 8 + 4 * i, value);
        *GuestPtr(base, g.scratch + 40 + i) = type;
        ++i;
      }

      PPCContext c = ctx;
      c.r3.u64 = g.scratch;
      c.r5.u64 = g.scratch + 8;
      c.r6.u64 = g.scratch + 40;
      c.r7.u64 = i;
      c.r8.u64 = 0;
      c.r9.u64 = 1; // (4:10 plays its keys only when this is set, as in script calls)
      fn(c, base);
    }

    constexpr uint8_t kHash = 3, kInt = 2;

    void Show(PPCContext &ctx, uint8_t *base, uint32_t obj, bool on)
    {
      Native(ctx, base, on ? sub_82282BB8 : sub_82282C28, {{obj, kHash}});
    }

    void SetText(PPCContext &ctx, uint8_t *base, uint32_t obj, uint32_t text)
    {
      Native(ctx, base, sub_82283210, {{obj, kHash}, {text, kHash}});
    }

    void Alpha(PPCContext &ctx, uint8_t *base, uint32_t obj, uint32_t alpha)
    {
      Native(ctx, base, sub_82283318, {{obj, kHash}, {alpha, kInt}});
    }

    void SetResult(uint8_t *base, uint32_t out, uint32_t value, uint32_t type)
    {
      Store32(base, out, value);
      Store32(base, out + 4, type);
    }

    bool Unlocked(uint32_t id)
    {
      return g.manager ? g.manager->IsUnlocked(id) : false;
    }

    // "10/14/2026  12:49 PM" (local time) from the unlock's FILETIME; "UNLOCKED" when the time is unknown
    std::string UnlockDate(uint32_t id)
    {
      const uint64_t ft = g.manager ? g.manager->GetUnlockTime(id) : 0;
      constexpr uint64_t kUnixEpoch = 116444736000000000ull; // 1970-01-01 in FILETIME units (100 ns since 1601)

      if (ft <= kUnixEpoch)
        return "UNLOCKED";

      const std::time_t t = std::time_t((ft - kUnixEpoch) / 10000000ull);
      std::tm tm{};
#if defined(_WIN32)
      if (localtime_s(&tm, &t) != 0)
        return "UNLOCKED";
#else
      if (!localtime_r(&t, &tm))
        return "UNLOCKED";
#endif
      const int hour = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
      return fmt::format("{:02}/{:02}/{}  {}:{:02} {}", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900, hour, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    }

    // The MsgBox body font's advances (from the language file's FONT chunk: 0x20-byte fonts {hash, glyph count, 0, glyphs offset, ...}, 0x18-byte glyphs {texture, char, x, y, w, h, x offset, advance, ...}). Under g.mutex
    void ReadAdvances(uint8_t *base)
    {
      if (!g.advances.empty() || !g.font_file)
        return;

      // chunks {tag, chunk size, data size, 0x44332211, data}
      uint32_t at = g.font_file;

      for (int n = 0; n < 64; ++n)
      {
        const uint32_t tag = Load32(base, at), size = Load32(base, at + 4), data = Load32(base, at + 8);

        if (!tag || size < 16 || size > 0x400000 || Load32(base, at + 12) != 0x44332211)
          return;

        if (tag == 0x464F4E54) // "FONT"
        {
          const uint32_t d = at + 16, fonts = Load32(base, d + 8);

          for (uint32_t f = 0; f < fonts && f < 64; ++f)
          {
            const uint32_t e = d + 12 + 0x20 * f;

            if (Load32(base, e) != kPopupFont)
              continue;

            const uint32_t glyphs = Load32(base, e + 4) >> 16, go = Load32(base, e + 8);

            // (the glyph offset is chunk-relative in the file; the loader may have made it an address)
            const uint32_t glyph_base = go >= g.font_file ? go : d + go;

            if (glyphs > 0x400 || (go < g.font_file && go + 0x18 * glyphs > data))
              return;

            g.advances.assign(256, 0);

            for (uint32_t i = 0; i < glyphs; ++i)
            {
              const uint32_t gl = glyph_base + 0x18 * i;
              const uint32_t ch = Load32(base, gl + 4) >> 16;
              const uint32_t adv = Load32(base, gl + 16) >> 16;

              if (ch < 256)
                g.advances[ch] = uint16_t(adv);
            }

            REXLOG_INFO("[sst-ach] popup font: {} glyphs", glyphs);
            return;
          }

          return;
        }

        at += size;
      }
    }

    float TextWidth(const std::string &s)
    {
      float w = 0;

      for (const char c : s)
      {
        const uint8_t u = uint8_t(c);
        w += (u < g.advances.size() && g.advances[u]) ? float(g.advances[u]) : 16.0f;
      }

      return w;
    }

    // Word wrap to `width` (font units), at most `max_lines` (the last one cut with "..." when the text goes on). Under g.mutex
    std::vector<std::string> Wrap(uint8_t *base, const std::string &text, float width, size_t max_lines)
    {
      ReadAdvances(base);
      std::vector<std::string> words, lines;
      size_t p = 0;

      while (p < text.size())
      {
        const size_t q = text.find(' ', p);
        const std::string w = text.substr(p, q == std::string::npos ? std::string::npos : q - p);

        if (!w.empty())
          words.push_back(w);

        if (q == std::string::npos)
          break;

        p = q + 1;
      }

      std::string line;

      for (size_t i = 0; i < words.size(); ++i)
      {
        const std::string candidate = line.empty() ? words[i] : line + " " + words[i];

        if (line.empty() || TextWidth(candidate) <= width)
        {
          line = candidate;
          continue;
        }

        lines.push_back(line);
        line = words[i];

        if (lines.size() == max_lines)
        {
          // no room for the rest: the last line ends with "..."
          std::string last = lines.back();

          while (!last.empty() && TextWidth(last + "...") > width)
            last.pop_back();

          lines.back() = last + "...";
          return lines;
        }
      }

      if (!line.empty())
        lines.push_back(line);

      return lines;
    }

    // sst_achievement_popup_sound -> sound hash (lookup2 of the name, or #hex), 0 for none
    uint32_t PopupSound()
    {
      std::string name = REXCVAR_GET(sst_achievement_popup_sound);

      if (name.empty() || name == "none" || name == "off")
        return 0;

      if (name[0] == '#')
        return uint32_t(std::strtoul(name.c_str() + 1, nullptr, 16));

      return Lookup2(name);
    }
  }

  void OnSceneFile(uint8_t *base, const std::string &name, uint32_t buffer)
  {
    if (name != "msgbox_en.dat" || !buffer)
      return;

    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    g.font_file = buffer;
    g.advances.clear(); // read when first needed (the read may still be under way)
  }

  bool UseGameUi()
  {
    return Enabled();
  }

  void Initialize(rex::system::AchievementManager &manager)
  {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    g.manager = &manager;
    REXLOG_INFO("[sst-ach] game-styled achievements {}{}", Enabled() ? "on" : "off (SDK overlay and toasts)", REXCVAR_GET(sst_achievement_popup_test) ? ", popup test at the main menu" : "");

    manager.RegisterNotificationCallback([](const rex::system::AchievementEvent &event)
                                         {
      if (!Enabled())
        return;

      std::lock_guard<std::recursive_mutex> lock(g.mutex);
      g.queue.push_back(event.achievement.id);
      REXLOG_INFO("[sst-ach] unlocked {} \"{}\"", event.achievement.id, event.achievement.label); });
  }

  uint32_t ExtraRoom(const std::string &name)
  {
    if (!Enabled())
      return 0;

    if (name == "sstmainmenu.dat")
      return 0x30000;

    if (name == "sstmainmenu.rel")
      return 0x4000;

    if (name == "msgbox.dat")
      return 0x8000;

    if (name == "msgbox.rel")
      return 0x1000;

    return 0;
  }

  bool BuildScene(const std::string &dat_name, const uint8_t *dat, size_t dat_size, const uint8_t *rel, size_t rel_size, std::vector<uint8_t> &out_dat, std::vector<uint8_t> &out_rel)
  {
    if (!Enabled() || (dat_name != "sstmainmenu.dat" && dat_name != "msgbox.dat"))
      return false;

    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    Load();

    if (g.list.empty())
      return false;

    const std::string gpu_name = dat_name.substr(0, dat_name.size() - 4) + ".gpu";
    const auto gpu = g.gpu.find(gpu_name);

    if (gpu == g.gpu.end() || !gpu->second.written)
    {
      REXLOG_WARN("[sst-ach] {}: icons not loaded with {}; not patched", dat_name, gpu_name);
      return false;
    }

    sst::menu::scenes::IconPage page;
    page.data_offset = gpu->second.offset;
    page.width = kPageW;
    page.height = kPageH;

    for (uint32_t k = 0; k < kSlots; ++k)
      page.slots.push_back(kTexture + k);

    std::string error;

    if (dat_name == "sstmainmenu.dat")
    {
      sst::menu::scenes::AchievementList list;
      list.page_hash = kListPage;
      list.list = kList;
      list.summary = kSummary;
      list.row_group = kRowGroup;
      list.row_panel = kRowPanel;
      list.row_icon = kRowIcon;
      list.row_icon_locked = kRowIconLocked;
      list.row_name = kRowName;
      list.row_desc = kRowDesc;
      list.row_score = kRowScore;
      list.text_blank = kTextBlank;
      list.page = page;
      list.locked_offset = int(kLockedOffset);
      list.visible_rows = kVisibleRows;

      for (const Entry &e : g.list)
      {
        list.icon_slot.push_back(e.slot);
        list.desc_scale.push_back(e.desc_scale);
      }

      if (!sst::menu::scenes::AddAchievementList(dat, dat_size, rel, rel_size, list, out_dat, out_rel, error))
      {
        REXLOG_ERROR("[sst-ach] ACHIEVEMENTS page not added: {}", error);
        return false;
      }

      REXLOG_INFO("[sst-ach] sstmainmenu.dat: ACHIEVEMENTS list ({} rows, row pitch {:.4f}; DAT {} -> {}, REL {} -> {})", g.list.size(), list.pitch, dat_size, out_dat.size(), rel_size, out_rel.size());
      return true;
    }

    sst::menu::scenes::AchievementPopup popup;
    popup.page_hash = kPopupPage;
    popup.group = kPopupGroup;
    popup.bar = kPopupBar;
    popup.header = kPopupHeader;
    popup.name = kPopupName;

    for (int k = 0; k < 3; ++k)
      popup.panel[k] = kPopupPanels[k], popup.desc[k] = kPopupDesc[k];

    popup.icon_base = kPopupIcon;
    popup.hide_label = kPopupHide;
    popup.show_label = kPopupShow;
    popup.text_blank = kTextBlank;
    popup.page = page;

    if (!sst::menu::scenes::AddAchievementPopup(dat, dat_size, rel, rel_size, popup, out_dat, out_rel, error))
    {
      REXLOG_ERROR("[sst-ach] unlock popup not added: {}", error);
      return false;
    }

    REXLOG_INFO("[sst-ach] msgbox.dat: unlock popup (DAT {} -> {}, REL {} -> {})", dat_size, out_dat.size(), rel_size, out_rel.size());
    return true;
  }

  void SceneReady(const std::string &dat_name, bool ok)
  {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);

    if (dat_name == "sstmainmenu.dat")
      g.list_ready = ok;
    else if (dat_name == "msgbox.dat")
      g.popup_ready = ok;
  }

  bool ResolveText(uint8_t *base, uint32_t id, uint32_t &out)
  {
    if ((id & 0xFFFF0000u) != 0x53410000u)
      return false;

    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    Load();
    PutTexts(base);
    const auto it = g.texts.find(id);

    if (it == g.texts.end())
      return false;

    out = it->second;
    return true;
  }

  void OnScriptStart(PPCContext &ctx, uint8_t *base, uint32_t front_end, uint32_t script)
  {
    {
      std::lock_guard<std::recursive_mutex> lock(g.mutex);
      g.front_end = front_end;

      // The popup test waits for the main menu (so the popup is not lost behind the boot movies)
      if (script == kScriptMainMenu)
      {
        g.menu_up = true;

        // ACHIEVEMENTS leaves the menu like the other items: after choosing an item the menu stays for actions 12 and 14 (the Xbox 360 overlays: downloads, achievements) and leaves for the rest; the "14" of that test becomes 999 in the running module, so action 14 leaves and starts "options_achievements" (see OverrideNative)
        if (Enabled())
          if (const uint32_t node = FindScriptNode(base, front_end, kScriptMainMenu))
          {
            const uint32_t module = Load32(base, node + 0x18);

            // pushabs 11; pushi 12; ne; pushabs 11; pushi 14; ne; and
            static const uint8_t kTest[] = {0x29, 0x00, 0x0B, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x0D, 0x15, 0x29, 0x00, 0x0B, 0x01, 0x00, 0x00, 0x00, 0x0E, 0x0D, 0x15, 0x0D, 0x19};
            int patched = 0;

            for (uint32_t at = 0x20; module && at + sizeof(kTest) < 0x10000; ++at)
            {
              uint8_t *p = GuestPtr(base, module + at);

              if (std::memcmp(p, kTest, sizeof(kTest)) == 0)
              {
                p[16] = 0xE7, p[15] = 0x03; // 14 -> 999
                ++patched;
              }
            }

            if (patched)
              REXLOG_INFO("[sst-ach] main menu: ACHIEVEMENTS leaves the menu ({} test{} patched)", patched, patched == 1 ? "" : "s");
          }
      }
    }

    if (script != kScriptRecords && script != kScriptAchievements)
      return;

    std::lock_guard<std::recursive_mutex> lock(g.mutex);

    if (script == kScriptRecords)
      return;

    // ACHIEVEMENTS: the port's page instead of the Xbox 360 system UI (when the list is in the scene)
    const uint32_t node = FindScriptNode(base, front_end, kScriptAchievements);

    if (!node)
    {
      REXLOG_WARN("[sst-ach] \"options_achievements\" not found in the script directory");
      return;
    }

    const uint32_t current = Load32(base, node + 0x18);
    const bool want = Enabled() && g.list_ready && SetupGuest(base);

    if (want && !g.script)
    {
      auto *memory = rex::system::kernel_memory();
      g.script = memory ? memory->SystemHeapAlloc((uint32_t(sizeof(kAchievementsScript)) + 31) & ~31u) : 0;

      if (g.script)
        std::memcpy(GuestPtr(base, g.script), kAchievementsScript, sizeof(kAchievementsScript));
    }

    if (want && g.script)
    {
      if (current != g.script)
      {
        g.original = current;
        Store32(base, node + 0x18, g.script);
        REXLOG_INFO("[sst-ach] options_achievements -> the port's ACHIEVEMENTS page");
      }
    }
    else if (g.script && current == g.script && g.original)
    {
      Store32(base, node + 0x18, g.original); // the list is not in this scene: the game's own script
    }
  }

  // The main menu's ACHIEVEMENTS item (action 14): with a signed-in profile (native 15:82 true) it opens the Xbox 360 system UI (10:226, a stub here) and starts no script; the action starts "options_achievements" only for a guest or no profile (15:42 || !15:82). The item's greying and the cursor also ask 15:82, so only that one call (found next to the action's 'options_achievements' string in the running module) answers false while the port's page is available
  bool OverrideNative(PPCContext &ctx, uint8_t *base, uint32_t id)
  {
    if (id != 0x0F52 || !Enabled())
      return false;

    std::lock_guard<std::recursive_mutex> lock(g.mutex);

    if (!g.list_ready || !g.front_end || !ctx.r4.u32)
      return false;

    const uint32_t node = FindScriptNode(base, g.front_end, kScriptMainMenu);
    const uint32_t module = node ? Load32(base, node + 0x18) : 0;

    if (!module || Load32(base, ctx.r4.u32 + 8) != module)
      return false;

    // The call site: the 15:82 native (0x12 / 0x32 with debug info) shortly before the push of 'options_achievements' (once per module)
    if (g.menu_module != module)
    {
      g.menu_module = module;
      g.menu_site = 0;
      static const char kName[] = "options_achievements";
      const uint32_t limit = 0x10000;

      for (uint32_t at = 0x20; at + sizeof(kName) + 1 < limit && !g.menu_site; ++at)
      {
        const uint8_t *p = GuestPtr(base, module + at);

        if (p[0] != 0x04 || std::memcmp(p + 1, kName, sizeof(kName)) != 0)
          continue;

        for (uint32_t back = 4; back <= 0x40 && back <= at; ++back)
        {
          const uint8_t *q = GuestPtr(base, module + at - back);

          if ((q[0] == 0x12 || q[0] == 0x32) && q[1] == 0x00 && q[2] == 0x0F && q[3] == 0x52)
          {
            g.menu_site = module + at - back;
            break;
          }
        }
      }

      REXLOG_INFO("[sst-ach] main menu ACHIEVEMENTS check {}", g.menu_site ? fmt::format("at {:08X}", g.menu_site) : std::string("not found"));
    }

    uint32_t pc = Load32(base, ctx.r4.u32 + 4);

    if (pc < 0x100000)
      pc += module; // (an offset in the module)

    if (!g.menu_site || pc < g.menu_site || pc > g.menu_site + 8)
      return false;

    SetResult(base, ctx.r3.u32, 0, 2);
    REXLOG_INFO("[sst-ach] ACHIEVEMENTS chosen: the port's page instead of the system UI");
    return true;
  }

  // The page's natives: 0 begin (-> scroll steps), 1 finish
  void HandleNative(PPCContext &ctx, uint8_t *base, uint32_t id)
  {
    const uint32_t out = ctx.r3.u32;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    Load();
    PutTexts(base);

    switch (id & 0xFF)
    {
    case 0:
    {
      uint32_t unlocked = 0, score = 0, total = 0;
      g.summary_alt = !g.summary_alt;

      // Shown first: showing a group shows its children (both icons of every row)
      Show(ctx, base, kList, true);
      Show(ctx, base, kSummary, true);

      for (size_t i = 0; i < g.list.size(); ++i)
      {
        const Entry &e = g.list[i];
        const bool on = Unlocked(e.id);
        const uint32_t r = uint32_t(i);
        unlocked += on ? 1 : 0;
        score += on ? e.gamerscore : 0;
        total += e.gamerscore;
        SetText(ctx, base, kRowName + r, kTextName + r);
        SetText(ctx, base, kRowDesc + r, kTextDesc + r);

        // Unlock date and time in the gamerscore's place (local time; blank while locked)
        const uint32_t date = (g.summary_alt ? kTextDateAlt : kTextDate) + r;
        PutText(base, date, on ? UnlockDate(e.id) : std::string(" "), 32);
        SetText(ctx, base, kRowScore + r, date);
        Show(ctx, base, kRowIcon + r, on);
        Show(ctx, base, kRowIconLocked + r, !on);

        for (uint32_t obj : {kRowName + r, kRowDesc + r, kRowScore + r})
          Alpha(ctx, base, obj, on ? 0xFF : 0x78);
      }

      const uint32_t text = kTextSummary + (g.summary_alt ? 1 : 0);
      PutText(base, text, fmt::format("{} / {} UNLOCKED", unlocked, g.list.size()), 64);
      SetText(ctx, base, kSummary, text);
      const int steps = std::max(0, int(g.list.size()) - kVisibleRows);
      SetResult(base, out, uint32_t(steps), 2);
      REXLOG_INFO("[sst-ach] ACHIEVEMENTS page: {} / {} unlocked", unlocked, g.list.size());
      (void)score;
      (void)total;
      break;
    }

    case 1:
      Show(ctx, base, kList, false);
      Show(ctx, base, kSummary, false);
      SetResult(base, out, 0, 2);
      break;

    default:
      SetResult(base, out, 0, 0);
      break;
    }
  }

  void Frame(PPCContext &ctx, uint8_t *base)
  {
    if (!Enabled())
      return;

    std::lock_guard<std::recursive_mutex> lock(g.mutex);

    if (!g.frames_seen)
    {
      g.frames_seen = true;
      REXLOG_INFO("[sst-ach] scene updates running (popups {})", g.popup_ready ? "ready" : "not ready yet");
    }

    if (g.popup_ready && g.menu_up && REXCVAR_GET(sst_achievement_popup_test) && !g.tested)
    {
      Load();
      g.tested = true;
      REXLOG_INFO("[sst-ach] popup test");

      if (!g.list.empty())
        g.queue.push_back(g.list.front().id);
    }

    if (!g.popup_ready || (g.popup == State::Popup::kIdle && g.queue.empty()))
      return;

    using clock = std::chrono::steady_clock;
    const auto now = clock::now();

    const auto ms = [&]
    { return std::chrono::duration_cast<std::chrono::milliseconds>(now - g.since).count(); };

    switch (g.popup)
    {
    case State::Popup::kIdle:
    {
      Load();
      PutTexts(base);
      const uint32_t id = g.queue.front();
      g.queue.pop_front();

      const auto it = std::find_if(g.list.begin(), g.list.end(), [&](const Entry &e)
                                   { return e.id == id; });

      if (it == g.list.end())
        break;

      g.popup_alt = !g.popup_alt;
      const uint32_t alt = g.popup_alt ? 1 : 0;
      PutText(base, kTextPopupName + alt, it->name, kPopupText);

      // The description wrapped to the popup's width (up to three lines; the panel picture of that height)
      const std::vector<std::string> lines = Wrap(base, it->desc, sst::menu::scenes::kPopupDescWidth, 3);

      for (uint32_t k = 0; k < 3; ++k)
        PutText(base, kTextPopupDesc + k * 2 + alt, k < lines.size() ? lines[k] : std::string(" "), kPopupText);

      Native(ctx, base, sub_82282830, {{kPopupPage, kHash}});
      SetText(ctx, base, kPopupHeader, kTextPopupHeader);
      SetText(ctx, base, kPopupName, kTextPopupName + alt);

      for (uint32_t k = 0; k < 3; ++k)
      {
        SetText(ctx, base, kPopupDesc[k], kTextPopupDesc + k * 2 + alt);
        Show(ctx, base, kPopupPanels[k], k + 1 == std::max<size_t>(lines.size(), 1));
      }

      for (uint32_t k = 0; k < kSlots; ++k)
        Show(ctx, base, kPopupIcon + k, k == uint32_t(it->slot));

      Native(ctx, base, sub_82282AA0, {{kPopupGroup, kHash}, {kPopupHide, kHash}, {kPopupShow, kHash}, {0, kInt}});

      if (const uint32_t sound = PopupSound())
        Native(ctx, base, sub_82287EB8, {{sound, kHash}});

      g.popup = State::Popup::kShown;
      REXLOG_INFO("[sst-ach] popup: {} \"{}\" (icon slot {})", it->id, it->name, it->slot);
      g.since = now;
      break;
    }

    case State::Popup::kShown:
      if (ms() >= std::max(1000, int(REXCVAR_GET(sst_achievement_popup_ms))))
      {
        Native(ctx, base, sub_82282AA0, {{kPopupGroup, kHash}, {kPopupShow, kHash}, {kPopupHide, kHash}, {0, kInt}});
        g.popup = State::Popup::kLeaving;
        g.since = now;
      }

      break;

    case State::Popup::kLeaving:
      if (ms() >= 400)
      {
        Native(ctx, base, sub_82282888, {{kPopupPage, kHash}});
        REXLOG_INFO("[sst-ach] popup closed");
        g.popup = State::Popup::kIdle;
        g.since = now;
      }

      break;
    }
  }
}

// File size (r3 path -> r3 size, <= 0 if missing): the two scenes' .GPU files report room for the page
extern "C" void __imp__sub_82594820(PPCContext &__restrict, uint8_t *);
extern "C" REX_FUNC(sub_82594820)
{
  const uint32_t path = ctx.r3.u32;
  __imp__sub_82594820(ctx, base);

  if (int32_t(ctx.r3.u32) <= 0 || !sst::ach::Enabled())
    return;

  const std::string name = sst::menu::GuestFileName(base, path);

  if (name != "sstmainmenu.gpu" && name != "msgbox.gpu")
    return;

  const uint32_t offset = (ctx.r3.u32 + 0xFFF) & ~0xFFFu;

  {
    std::lock_guard<std::recursive_mutex> lock(sst::ach::g.mutex);
    auto &gpu = sst::ach::g.gpu[name];

    if (gpu.offset != offset)
      gpu = {offset, false};
  }

  ctx.r3.u32 = offset + sst::ach::kPageBytes;
}

namespace sst::ach
{
  namespace
  {
    // .GPU reads (r3 path, r4 buffer): the page goes after the file (which fills only its own bytes)
    void WritePage(uint8_t *base, uint32_t path, uint32_t buffer)
    {
      if (!buffer || !Enabled())
        return;

      const std::string name = GuestFileName(base, path);

      if (name != "sstmainmenu.gpu" && name != "msgbox.gpu")
        return;

      std::lock_guard<std::recursive_mutex> lock(g.mutex);
      auto it = g.gpu.find(name);

      if (it == g.gpu.end())
        return;

      Load();

      if (g.page.size() != kPageBytes)
        return;

      std::memcpy(GuestPtr(base, buffer + it->second.offset), g.page.data(), kPageBytes);
      it->second.written = true;
    }
  }
}

extern "C" void __imp__sub_825947C8(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_82594758(PPCContext &__restrict, uint8_t *);

extern "C" REX_FUNC(sub_825947C8)
{
  sst::ach::WritePage(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_825947C8(ctx, base);
}

extern "C" REX_FUNC(sub_82594758)
{
  sst::ach::WritePage(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_82594758(ctx, base);
}