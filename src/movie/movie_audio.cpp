// Movie (CRI Movie / .usm) audio: play each movie's sound at its own sampling rate. The game's movie sound (CRI's sample code, CriSmpMvSound) outputs through an XAudio source voice created at a fixed 48000 Hz, 6 channels (sub_822E6FE8), and feeds it the movie's decoded PCM 128 samples at a time
// without resampling. The movie's sampling rate reaches the sound engine (CriSmpMvSoundLoc::Create, sub_822E6410, r6) but the engine's method for it is an empty stub (vtable 0x8201238C slot 5 = sub_8235DAC8, blr). Movies at 48 kHz play right; Sega.sfd (the boot movie) is 44100 Hz stereo ADX, so
// it played around 10% faster and had a higher audio pitch than normal. This code has a fix that sets the audio's frequency ratio (sub_82517A30, the call the game's own sound system uses for pitch) to rate / 48000.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

REXCVAR_DECLARE(bool, sst_movie_log);
REXCVAR_DEFINE_BOOL(sst_movie_audio_rate, true, "SST", "Play movie sound at the movie's own sampling rate (the boot SEGA movie is 44.1 kHz; the game played it at 48 kHz, too high)");

namespace
{
  constexpr uint32_t kVoiceRate = 48000; // the movie voice's fixed rate (sub_822E6FE8)

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
    return addr >= 0x10000000u && addr <= 0xFFFFF000u; // (physical allocations live at 0xE0000000-0xFFFFFFFF)
  }

  std::mutex g_mutex;
  std::unordered_map<uint32_t, double> g_ratio; // sound engine -> frequency ratio
}

extern "C" void __imp__sub_822E6410(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_822E6C68(PPCContext &__restrict, uint8_t *);
extern "C" void __imp__sub_822E6BA0(PPCContext &__restrict, uint8_t *);
extern "C" void sub_82517A30(PPCContext &__restrict, uint8_t *); // XAudio source voice: set frequency ratio (r3 voice, f1 ratio)

namespace
{
  // engine +28: its XAudio source voice
  void ApplyRatio(PPCContext &ctx, uint8_t *base, uint32_t engine, double ratio)
  {
    const uint32_t voice = Load32(base, engine + 28);

    if (!Plausible(voice))
      return;

    PPCContext c = ctx;
    c.r3.u64 = voice;
    c.f1.f64 = ratio;
    sub_82517A30(c, base);

    if (int32_t(c.r3.u32) < 0)
      REXLOG_WARN("[sst-movie] audio: setting frequency ratio {:.5f} failed ({:08X})", ratio, c.r3.u32);
  }
}

// CriSmpMvSoundLoc::Create(this, r4, channels r5, sampling rate r6; seen: 2, 44100 for Sega.usm): creates the sound engine (this +8)
extern "C" REX_FUNC(sub_822E6410)
{
  const uint32_t self = ctx.r3.u32, channels = ctx.r5.u32, rate = ctx.r6.u32;
  __imp__sub_822E6410(ctx, base);

  const uint32_t engine = Plausible(self) ? Load32(base, self + 8) : 0;

  if (!Plausible(engine))
    return;

  // 48 kHz movies (and anything implausible) are left as the game plays them
  if (!REXCVAR_GET(sst_movie_audio_rate) || rate < 8000 || rate > 96000 || rate == kVoiceRate)
  {
    if (REXCVAR_GET(sst_movie_log))
      REXLOG_INFO("[sst-movie] audio: {} ch, {} Hz", channels, rate);

    return;
  }

  const double ratio = double(rate) / kVoiceRate;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ratio[engine] = ratio;
  }

  REXLOG_INFO("[sst-movie] audio: {} ch, {} Hz -> frequency ratio {:.5f} on the 48000 Hz voice", channels, rate, ratio);
  PPCContext c = ctx;
  ApplyRatio(c, base, engine, ratio);
}

// Sound engine: start (r3 engine); sets the voice's volume and starts it
extern "C" REX_FUNC(sub_822E6C68)
{
  const uint32_t engine = ctx.r3.u32;

  double ratio = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_ratio.find(engine);

    if (it != g_ratio.end())
      ratio = it->second;
  }

  // Before the start, so the first samples play at the right rate
  if (ratio > 0)
  {
    PPCContext c = ctx;
    ApplyRatio(c, base, engine, ratio);
  }

  __imp__sub_822E6C68(ctx, base);

  // and after it, in case starting resets the voice's frequency
  if (ratio > 0)
  {
    PPCContext c = ctx;
    ApplyRatio(c, base, engine, ratio);
  }
}

// Sound engine: destroy (r3 engine)
extern "C" REX_FUNC(sub_822E6BA0)
{
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ratio.erase(ctx.r3.u32);
  }

  __imp__sub_822E6BA0(ctx, base);
}