// sst_rematch - movie (CRI Movie / .usm) playback diagnostics

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(sst_movie_log, true, "SST", "Log movie playback steps (file opens, CRI errors) for diagnostics");

namespace
{
  inline uint8_t *GuestPtr(uint8_t *base, uint32_t addr)
  {
    return base + addr + rex::memory::detail::PhysicalHostOffset(addr);
  }

  inline uint32_t Load32(uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, GuestPtr(base, addr), 4);
    return std::byteswap(v);
  }

  bool Plausible(uint32_t addr)
  {
    return addr >= 0x10000000u && addr < 0xF0000000u;
  }

  std::string GuestString(uint8_t *base, uint32_t addr, size_t max = 260)
  {
    std::string s;

    if (!Plausible(addr))
      return "<null>";

    const char *p = reinterpret_cast<const char *>(GuestPtr(base, addr));

    for (size_t i = 0; i < max && p[i]; ++i)
      s += std::isprint(uint8_t(p[i])) ? p[i] : '?';

    return s;
  }

  // A minimal printf for CRI's messages: %s (guest string), %d/%i/%u/%x/%X/%p
  std::string Format(uint8_t *base, const std::string &fmt, const uint32_t *args, int nargs)
  {
    std::string out;
    int a = 0;

    for (size_t i = 0; i < fmt.size(); ++i)
    {
      if (fmt[i] != '%' || i + 1 >= fmt.size())
      {
        out += fmt[i];
        continue;
      }

      size_t j = i + 1;

      while (j < fmt.size() && std::strchr("0123456789.-+# l", fmt[j]))
        ++j;

      if (j >= fmt.size())
        break;

      const char c = fmt[j];

      if (c == '%')
      {
        out += '%';
      }
      else
      {
        const uint32_t v = a < nargs ? args[a++] : 0;
        char buf[32];

        switch (c)
        {
        case 's':
          out += GuestString(base, v, 128);
          break;

        case 'd':
        case 'i':
          std::snprintf(buf, sizeof(buf), "%d", int32_t(v));
          out += buf;
          break;

        case 'u':
          std::snprintf(buf, sizeof(buf), "%u", v);
          out += buf;
          break;

        case 'x':
          std::snprintf(buf, sizeof(buf), "%x", v);
          out += buf;
          break;

        case 'X':
          std::snprintf(buf, sizeof(buf), "%X", v);
          out += buf;
          break;

        default:
          std::snprintf(buf, sizeof(buf), "<%c:%08X>", c, v);
          out += buf;
          break;
        }
      }

      i = j;
    }

    return out;
  }

  bool IsMovieName(const std::string &path)
  {
    std::string l = path;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c)
                   { return char(std::tolower(c)); });

    return l.find(".usm") != std::string::npos || l.find("movie") != std::string::npos;
  }

  std::atomic<int> g_errors{0};
}

extern "C" void __imp__sub_8227C798(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_825B1E68(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_8255BAD8(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_8253DF70(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_822E5428(PPCContext &__restrict, uint8_t *);

// Script native "play movie" (r5 -> {name, ...}, r9 = start flag; called every frame with start 0 to poll): builds <name without its extension>.usm into the movie request at [0x82DB777C]
extern "C" REX_FUNC(sub_8227C798)
{
  const bool start = (ctx.r9.u32 & 0xFF) != 0;

  if (REXCVAR_GET(sst_movie_log) && start)
  {
    const uint32_t args = ctx.r5.u32;
    const std::string name = Plausible(args) ? GuestString(base, Load32(base, args)) : "<?>";
    const uint32_t second = Plausible(args) ? Load32(base, args + 4) : 0;
    REXLOG_INFO("[sst-movie] play movie '{}' (arg 2 {:08X}, start {}; r3 {:08X} r4 {:08X} r5 {:08X} "
                "r6 {:08X})",
                name, second, ctx.r9.u32 & 0xFF, ctx.r3.u32, ctx.r4.u32, args, ctx.r6.u32);
  }

  __imp__sub_8227C798(ctx, base);

  if (REXCVAR_GET(sst_movie_log) && start)
  {
    const uint32_t req = Load32(base, 0x82DB777C);

    if (Plausible(req))
      REXLOG_INFO("[sst-movie]   request {:08X}: path '{}', {}x{}, flags {:08X}", req, GuestString(base, req + 8), Load32(base, req + 0x24), Load32(base, req + 0x28), Load32(base, req));
    else
      REXLOG_INFO("[sst-movie]   no movie request object");
  }
}

// CreateFileA (path r3): .usm opens
extern "C" REX_FUNC(sub_825B1E68)
{
  const bool log = REXCVAR_GET(sst_movie_log);
  std::string path;

  if (log)
    path = GuestString(base, ctx.r3.u32);

  const uint32_t access = ctx.r4.u32, flags = ctx.r8.u32;
  __imp__sub_825B1E68(ctx, base);

  if (log && IsMovieName(path))
    REXLOG_INFO("[sst-movie] CreateFile '{}' (access {:08X}, flags {:08X}) -> {:08X}", path, access, flags, ctx.r3.u32);
}

// criErr_Notify(level, format, p1, p2, p3)
extern "C" REX_FUNC(sub_8255BAD8)
{
  if (REXCVAR_GET(sst_movie_log) && g_errors.fetch_add(1) < 200)
  {
    const uint32_t args[3] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
    REXLOG_WARN("[sst-movie] CRI error (level {}): {}", ctx.r3.u32, Format(base, GuestString(base, ctx.r4.u32, 256), args, 3));
  }

  __imp__sub_8255BAD8(ctx, base);
}

// CriMvEasy: error (handle r3, message r4)
extern "C" REX_FUNC(sub_8253DF70)
{
  if (REXCVAR_GET(sst_movie_log) && g_errors.fetch_add(1) < 200)
    REXLOG_WARN("[sst-movie] CriMvEasy error: {}", GuestString(base, ctx.r4.u32, 256));

  __imp__sub_8253DF70(ctx, base);
}

// CriMovieControl::SetupTexture(this, width, height): the Y / U / V textures
extern "C" REX_FUNC(sub_822E5428)
{
  if (REXCVAR_GET(sst_movie_log))
    REXLOG_INFO("[sst-movie] SetupTexture {}x{} (control {:08X})", int32_t(ctx.r4.u32), int32_t(ctx.r5.u32), ctx.r3.u32);

  __imp__sub_822E5428(ctx, base);
}