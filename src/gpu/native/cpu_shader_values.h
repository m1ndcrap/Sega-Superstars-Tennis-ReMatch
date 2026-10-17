// The value formats the CPU shader interpreter (cpu_shader.h) reads and writes: float bits with denormals flushed (as the Xenos ALU), Xbox 360 halves, the GPU's endian swaps, and the memory-export stream formats.
// Derived from Xenia's ShaderInterpreter and memexport translators (copyright 2022 Ben Vanik, all rights reserved, released under the BSD license (ReXGlue SDK adaptation by Tom Clay, 2026))

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace sst::native::cpu_shader
{
  inline float AsFloat(uint32_t v)
  {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }

  inline uint32_t AsUint(float f)
  {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
  }

  inline float FlushDenormal(float value)
  {
    uint32_t bits = AsUint(value);
    bits &= (bits & 0x7F800000u) ? ~0u : (1u << 31);
    return AsFloat(bits);
  }

  inline float Saturate(float v) { return std::min(1.0f, std::max(0.0f, v)); }

  // Every write into a temp register goes through here: the values are stored with denormals flushed, which is what every read of them wants (FlushDenormal on read, as D3D9 / the Xenos ALU does), so unmodified operands can be read in place.
  inline void StoreTemp(float *__restrict dst, const float *__restrict src, uint32_t n)
  {
    for (uint32_t l = 0; l < n; ++l)
    {
      const uint32_t bits = AsUint(src[l]);
      const uint32_t keep = (bits & 0x7F800000u) ? 0xFFFFFFFFu : 0x80000000u;
      dst[l] = AsFloat(bits & keep);
    }
  }

  // Xbox 360 half (no inf / NaN: exponent 31 is an ordinary exponent) -> float bits; the same values as vertex_format's HalfToFloat table, computed (a 256 KB table costs a cache miss per lookup)
  inline uint32_t HalfToFloatBits(uint32_t h)
  {
    const uint32_t sign = (h & 0x8000u) << 16, a = h & 0x7FFFu;
    const uint32_t normal = (a << 13) + (112u << 23);
    const uint32_t subnormal = AsUint(float(int32_t(a)) * 5.9604644775390625e-8f); // a * 2^-24, exact (also 0)
    const uint32_t is_normal = 0u - uint32_t(a >= 0x400u);                         // mask, not a branch (zero halves are common)
    return sign | (normal & is_normal) | (subnormal & ~is_normal);
  }

  // Float -> IEEE half (round to nearest even), as Xenia's memexport does (standard half, not the Xbox extended range).
  inline uint16_t FloatToHalf(float value)
  {
    const uint32_t f = AsUint(value);
    const uint32_t sign = (f >> 16) & 0x8000u;
    const uint32_t abs = f & 0x7FFFFFFFu;

    if (abs >= 0x7F800000u)
      return uint16_t(sign | (abs > 0x7F800000u ? 0x7E00u : 0x7C00u));

    if (abs >= 0x477FF000u)
      return uint16_t(sign | 0x7C00u); // overflow -> inf

    if (abs < 0x38800000u)
    {
      if (abs < 0x33000000u)
        return uint16_t(sign);

      const uint32_t mant = (abs & 0x7FFFFFu) | 0x800000u;
      const uint32_t shift = 126u - (abs >> 23);
      uint32_t h = mant >> (shift + 1);
      const uint32_t rem = mant & ((1u << (shift + 1)) - 1), half = 1u << shift;

      if (rem > half || (rem == half && (h & 1)))
        ++h;

      return uint16_t(sign | h);
    }

    uint32_t h = ((abs - 0x38000000u) >> 13);
    const uint32_t rem = abs & 0x1FFFu;

    if (rem > 0x1000u || (rem == 0x1000u && (h & 1)))
      ++h;

    return uint16_t(sign | h);
  }

  template <uint32_t kEndian>
  inline uint32_t Swap32T(uint32_t v)
  {
    if constexpr (kEndian == 1)
      return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    else if constexpr (kEndian == 2)
      return __builtin_bswap32(v);
    else if constexpr (kEndian == 3)
      return (v << 16) | (v >> 16);
    else
      return v;
  }

  inline uint32_t Swap32(uint32_t v, uint32_t endian)
  {
    switch (endian)
    {
    case 1: // 8in16
      return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);

    case 2: // 8in32
      return __builtin_bswap32(v);

    case 3: // 16in32
      return (v << 16) | (v >> 16);

    default:
      return v;
    }
  }

  // xenos::Endian128 on a 4-dword element.
  inline void Swap128(uint32_t v[4], uint32_t endian)
  {
    switch (endian)
    {
    case 1:
    case 2:
    case 3:
      for (int i = 0; i < 4; ++i)
        v[i] = Swap32(v[i], endian);

      break;

    case 4: // 8in64
      for (int i = 0; i < 4; i += 2)
      {
        const uint32_t a = __builtin_bswap32(v[i]), b = __builtin_bswap32(v[i + 1]);
        v[i] = b;
        v[i + 1] = a;
      }

      break;

    case 5: // 8in128
    {
      const uint32_t a = __builtin_bswap32(v[0]), b = __builtin_bswap32(v[1]), c = __builtin_bswap32(v[2]), d = __builtin_bswap32(v[3]);
      v[0] = d, v[1] = c, v[2] = b, v[3] = a;
      break;
    }

    default:
      break;
    }
  }

  // Packs one eM# value into the stream format. Returns the element size in bytes (0 = unsupported format).
  inline uint32_t PackElement(const float in[4], uint32_t format_info, uint32_t out[4])
  {
    float v[4] = {in[0], in[1], in[2], in[3]};

    if ((format_info >> 19) & 1)
      std::swap(v[0], v[2]); // red / blue swap

    const bool is_signed = (format_info >> 16) & 1;
    const bool is_norm = !((format_info >> 17) & 1);
    const uint32_t format = (format_info >> 8) & 0x3F;
    out[0] = out[1] = out[2] = out[3] = 0;

    // Integer formats: component widths (packed from bit 0 upwards).
    int widths[4] = {};
    int n = 0;

    switch (format)
    {
    case 2: // k_8
      widths[0] = 8, n = 1;
      break;

    case 3: // k_1_5_5_5
      widths[0] = 5, widths[1] = 5, widths[2] = 5, widths[3] = 1, n = 4;
      break;

    case 4: // k_5_6_5
      widths[0] = 5, widths[1] = 6, widths[2] = 5, n = 3;
      break;

    case 5: // k_6_5_5
      widths[0] = 6, widths[1] = 5, widths[2] = 5, n = 3;
      break;

    case 6:
    case 14:
    case 50: // k_8_8_8_8 (+_A, AS_16)
      widths[0] = widths[1] = widths[2] = widths[3] = 8, n = 4;
      break;

    case 7:
    case 54: // k_2_10_10_10
      widths[0] = widths[1] = widths[2] = 10, widths[3] = 2, n = 4;
      break;

    case 10: // k_8_8
      widths[0] = widths[1] = 8, n = 2;
      break;

    case 15: // k_4_4_4_4
      widths[0] = widths[1] = widths[2] = widths[3] = 4, n = 4;
      break;

    case 16:
    case 55: // k_10_11_11
      widths[0] = widths[1] = 11, widths[2] = 10, n = 3;
      break;

    case 17:
    case 56: // k_11_11_10
      widths[0] = 10, widths[1] = widths[2] = 11, n = 3;
      break;

    case 24: // k_16
      widths[0] = 16, n = 1;
      break;

    case 25: // k_16_16
      widths[0] = widths[1] = 16, n = 2;
      break;

    case 26: // k_16_16_16_16
    {
      for (int i = 0; i < 4; ++i)
      {
        float f = std::isnan(v[i]) ? 0.0f : v[i];
        uint32_t q;

        if (is_signed)
        {
          if (is_norm)
            f = std::clamp(f, -1.0f, 1.0f) * 32767.0f;
          else
            f = std::clamp(f, -32768.0f, 32767.0f);

          q = uint32_t(int32_t(f + (f < 0 ? -0.5f : 0.5f))) & 0xFFFFu;
        }
        else
        {
          if (is_norm)
            f = Saturate(f) * 65535.0f;
          else
            f = std::clamp(f, 0.0f, 65535.0f);

          q = uint32_t(f + 0.5f) & 0xFFFFu;
        }

        out[i >> 1] |= q << (16 * (i & 1));
      }

      return 8;
    }

    case 30: // k_16_FLOAT
      out[0] = FloatToHalf(v[0]);
      return 2;

    case 31: // k_16_16_FLOAT
      out[0] = FloatToHalf(v[0]) | (uint32_t(FloatToHalf(v[1])) << 16);
      return 4;

    case 32: // k_16_16_16_16_FLOAT
      out[0] = FloatToHalf(v[0]) | (uint32_t(FloatToHalf(v[1])) << 16);
      out[1] = FloatToHalf(v[2]) | (uint32_t(FloatToHalf(v[3])) << 16);
      return 8;

    case 36: // k_32_FLOAT
      out[0] = AsUint(v[0]);
      return 4;

    case 37: // k_32_32_FLOAT
      out[0] = AsUint(v[0]), out[1] = AsUint(v[1]);
      return 8;

    case 38: // k_32_32_32_32_FLOAT
      for (int i = 0; i < 4; ++i)
        out[i] = AsUint(v[i]);

      return 16;

    default:
      return 0;
    }

    uint32_t packed = 0;
    int shift = 0;

    for (int i = 0; i < n; ++i)
    {
      const int w = widths[i];
      float f = std::isnan(v[i]) ? 0.0f : v[i];
      uint32_t q;

      if (is_signed)
      {
        const float max = float((1 << (w - 1)) - 1);

        if (is_norm)
          f = std::clamp(f, -1.0f, 1.0f) * max;
        else
          f = std::clamp(f, -max - 1.0f, max);

        q = uint32_t(int32_t(f + (f < 0 ? -0.5f : 0.5f)));
      }
      else
      {
        const float max = float((1u << w) - 1);

        if (is_norm)
          f = Saturate(f) * max;
        else
          f = std::clamp(f, 0.0f, max);

        q = uint32_t(f + 0.5f);
      }

      packed |= (q & ((1u << w) - 1)) << shift;
      shift += w;
    }

    out[0] = packed;
    return uint32_t((shift + 7) / 8);
  }
}