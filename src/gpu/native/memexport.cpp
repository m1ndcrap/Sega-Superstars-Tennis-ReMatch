// sst_rematch - native renderer: CPU execution of memory-export vertex shaders
// (see memexport.h).
//
// Derived from Xenia's ShaderInterpreter and memexport translators:
// Copyright 2022 Ben Vanik. All rights reserved. Released under the BSD license (ReXGlue SDK adaptation by Tom Clay, 2026).

#include "gpu/native/memexport.h"

#include "gpu/native/vertex_format.h"

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/xenos.h>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace sst::native
{

  namespace ucode = rex::graphics::ucode;
  namespace xenos = rex::graphics::xenos;

  namespace
  {

    constexpr uint32_t kTempCount = xenos::kMaxShaderTempRegisters; // 64
    constexpr uint32_t kMaxCfSteps = 4096;                          // control flow instructions per invocation
    constexpr uint32_t kGuestPhysicalSize = 0x20000000u;

    float AsFloat(uint32_t v)
    {
      float f;
      std::memcpy(&f, &v, 4);
      return f;
    }
    uint32_t AsUint(float f)
    {
      uint32_t v;
      std::memcpy(&v, &f, 4);
      return v;
    }
    float FlushDenormal(float value)
    {
      uint32_t bits = AsUint(value);
      bits &= (bits & 0x7F800000u) ? ~0u : (1u << 31);
      return AsFloat(bits);
    }
    float Saturate(float v) { return std::min(1.0f, std::max(0.0f, v)); }

    // Float -> IEEE half (round to nearest even), as Xenia's memexport does (standard half, not the Xbox extended range)
    uint16_t FloatToHalf(float value)
    {
      const uint32_t f = AsUint(value);
      const uint32_t sign = (f >> 16) & 0x8000u;
      const uint32_t abs = f & 0x7FFFFFFFu;
      if (abs >= 0x7F800000u)
        return uint16_t(sign | (abs > 0x7F800000u ? 0x7E00u : 0x7C00u));
      if (abs >= 0x477FF000u)
        return uint16_t(sign | 0x7C00u); // overflow -> inf
      if (abs < 0x38800000u)
      { // subnormal / zero
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

    uint32_t Swap32(uint32_t v, uint32_t endian)
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
    // xenos::Endian128 on a 4-dword element
    void Swap128(uint32_t v[4], uint32_t endian)
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
      case 5:
      { // 8in128
        const uint32_t a = __builtin_bswap32(v[0]), b = __builtin_bswap32(v[1]),
                       c = __builtin_bswap32(v[2]), d = __builtin_bswap32(v[3]);
        v[0] = d, v[1] = c, v[2] = b, v[3] = a;
      }
      break;
      default:
        break;
      }
    }

    // Packs one eM# value into the stream format. Returns the element size in bytes (0 = unsupported format)
    uint32_t PackElement(const float in[4], uint32_t format_info, uint32_t out[4])
    {
      float v[4] = {in[0], in[1], in[2], in[3]};
      if ((format_info >> 19) & 1)
        std::swap(v[0], v[2]); // red / blue swap
      const bool is_signed = (format_info >> 16) & 1;
      const bool is_norm = !((format_info >> 17) & 1);
      const uint32_t format = (format_info >> 8) & 0x3F;
      out[0] = out[1] = out[2] = out[3] = 0;

      // Integer formats: component widths (packed from bit 0 upwards)
      int widths[4] = {};
      int n = 0;
      switch (format)
      {
      case 2:
        widths[0] = 8, n = 1;
        break; // k_8
      case 3:
        widths[0] = 5, widths[1] = 5, widths[2] = 5, widths[3] = 1, n = 4;
        break; // k_1_5_5_5
      case 4:
        widths[0] = 5, widths[1] = 6, widths[2] = 5, n = 3;
        break; // k_5_6_5
      case 5:
        widths[0] = 6, widths[1] = 5, widths[2] = 5, n = 3;
        break; // k_6_5_5
      case 6:
      case 14:
      case 50: // k_8_8_8_8 (+_A, AS_16)
        widths[0] = widths[1] = widths[2] = widths[3] = 8, n = 4;
        break;
      case 7:
      case 54: // k_2_10_10_10
        widths[0] = widths[1] = widths[2] = 10, widths[3] = 2, n = 4;
        break;
      case 10:
        widths[0] = widths[1] = 8, n = 2;
        break; // k_8_8
      case 15:
        widths[0] = widths[1] = widths[2] = widths[3] = 4, n = 4;
        break; // k_4_4_4_4
      case 16:
      case 55:
        widths[0] = widths[1] = 11, widths[2] = 10, n = 3;
        break; // k_10_11_11
      case 17:
      case 56:
        widths[0] = 10, widths[1] = widths[2] = 11, n = 3;
        break; // k_11_11_10
      case 24:
        widths[0] = 16, n = 1;
        break; // k_16
      case 25:
        widths[0] = widths[1] = 16, n = 2;
        break; // k_16_16
      case 26:
      { // k_16_16_16_16
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

    // interpreter
    constexpr uint32_t kLanes = 64;

    using V = ucode::AluVectorOpcode;
    using S = ucode::AluScalarOpcode;

    inline float Mul(float a, float b) { return (a && b) ? a * b : 0.0f; } // D3D9: 0 * x = +0

    // One lane of a vector operation. Returns true if the result is replicated from x.
    inline bool VectorOp(V op, const float (&o)[3][4], float r[4], bool &pred, int32_t &a0)
    {
      switch (op)
      {
      case V::kAdd:
        for (int i = 0; i < 4; ++i)
          r[i] = o[0][i] + o[1][i];
        return false;
      case V::kMul:
        for (int i = 0; i < 4; ++i)
          r[i] = Mul(o[0][i], o[1][i]);
        return false;
      case V::kMax:
        for (int i = 0; i < 4; ++i)
          r[i] = std::isgreaterequal(o[0][i], o[1][i]) ? o[0][i] : o[1][i];
        return false;
      case V::kMin:
        for (int i = 0; i < 4; ++i)
          r[i] = std::isless(o[0][i], o[1][i]) ? o[0][i] : o[1][i];
        return false;
      case V::kSeq:
        for (int i = 0; i < 4; ++i)
          r[i] = float(o[0][i] == o[1][i]);
        return false;
      case V::kSgt:
        for (int i = 0; i < 4; ++i)
          r[i] = float(std::isgreater(o[0][i], o[1][i]));
        return false;
      case V::kSge:
        for (int i = 0; i < 4; ++i)
          r[i] = float(std::isgreaterequal(o[0][i], o[1][i]));
        return false;
      case V::kSne:
        for (int i = 0; i < 4; ++i)
          r[i] = float(o[0][i] != o[1][i]);
        return false;
      case V::kFrc:
        for (int i = 0; i < 4; ++i)
          r[i] = o[0][i] - std::floor(o[0][i]);
        return false;
      case V::kTrunc:
        for (int i = 0; i < 4; ++i)
          r[i] = std::trunc(o[0][i]);
        return false;
      case V::kFloor:
        for (int i = 0; i < 4; ++i)
          r[i] = std::floor(o[0][i]);
        return false;
      case V::kMad:
        for (int i = 0; i < 4; ++i)
          r[i] = Mul(o[0][i], o[1][i]) + o[2][i];
        return false;
      case V::kCndEq:
        for (int i = 0; i < 4; ++i)
          r[i] = o[0][i] == 0.0f ? o[1][i] : o[2][i];
        return false;
      case V::kCndGe:
        for (int i = 0; i < 4; ++i)
          r[i] = std::isgreaterequal(o[0][i], 0.0f) ? o[1][i] : o[2][i];
        return false;
      case V::kCndGt:
        for (int i = 0; i < 4; ++i)
          r[i] = std::isgreater(o[0][i], 0.0f) ? o[1][i] : o[2][i];
        return false;
      case V::kDp4:
        r[0] = 0.0f;
        for (int i = 0; i < 4; ++i)
          r[0] += Mul(o[0][i], o[1][i]);
        return true;
      case V::kDp3:
        r[0] = 0.0f;
        for (int i = 0; i < 3; ++i)
          r[0] += Mul(o[0][i], o[1][i]);
        return true;
      case V::kDp2Add:
        r[0] = Mul(o[0][0], o[1][0]) + Mul(o[0][1], o[1][1]) + o[2][0];
        return true;
      case V::kCube:
      {
        const float x = o[0][2], y = o[0][3], z = o[0][0];
        const float ax = std::abs(x), ay = std::abs(y), az = std::abs(z);
        if (az >= ax && az >= ay)
        {
          const bool n = std::isless(z, 0.0f);
          r[0] = -y, r[1] = n ? -x : x, r[2] = z, r[3] = n ? 5.0f : 4.0f;
        }
        else if (ay >= ax)
        {
          const bool n = std::isless(y, 0.0f);
          r[0] = n ? -z : z, r[1] = x, r[2] = y, r[3] = n ? 3.0f : 2.0f;
        }
        else
        {
          const bool n = std::isless(x, 0.0f);
          r[0] = -y, r[1] = n ? z : -z, r[2] = x, r[3] = n ? 1.0f : 0.0f;
        }
        r[2] *= 2.0f;
        return false;
      }
      case V::kMax4:
      {
        const float *v = o[0];
        if (std::isgreaterequal(v[0], v[1]) && std::isgreaterequal(v[0], v[2]) &&
            std::isgreaterequal(v[0], v[3]))
          r[0] = v[0];
        else if (std::isgreaterequal(v[1], v[2]) && std::isgreaterequal(v[1], v[3]))
          r[0] = v[1];
        else
          r[0] = std::isgreaterequal(v[2], v[3]) ? v[2] : v[3];
        return true;
      }
      case V::kSetpEqPush:
        pred = o[0][3] == 0.0f && o[1][3] == 0.0f;
        r[0] = (o[0][0] == 0.0f && o[1][0] == 0.0f) ? 0.0f : o[0][0] + 1.0f;
        return true;
      case V::kSetpNePush:
        pred = o[0][3] == 0.0f && o[1][3] != 0.0f;
        r[0] = (o[0][0] == 0.0f && o[1][0] != 0.0f) ? 0.0f : o[0][0] + 1.0f;
        return true;
      case V::kSetpGtPush:
        pred = o[0][3] == 0.0f && std::isgreater(o[1][3], 0.0f);
        r[0] = (o[0][0] == 0.0f && std::isgreater(o[1][0], 0.0f)) ? 0.0f : o[0][0] + 1.0f;
        return true;
      case V::kSetpGePush:
        pred = o[0][3] == 0.0f && std::isgreaterequal(o[1][3], 0.0f);
        r[0] = (o[0][0] == 0.0f && std::isgreaterequal(o[1][0], 0.0f)) ? 0.0f : o[0][0] + 1.0f;
        return true;
      case V::kKillEq:
        r[0] = float(o[0][0] == o[1][0] || o[0][1] == o[1][1] || o[0][2] == o[1][2] ||
                     o[0][3] == o[1][3]);
        return true;
      case V::kKillGt:
        r[0] = float(std::isgreater(o[0][0], o[1][0]) || std::isgreater(o[0][1], o[1][1]) ||
                     std::isgreater(o[0][2], o[1][2]) || std::isgreater(o[0][3], o[1][3]));
        return true;
      case V::kKillGe:
        r[0] = float(std::isgreaterequal(o[0][0], o[1][0]) || std::isgreaterequal(o[0][1], o[1][1]) ||
                     std::isgreaterequal(o[0][2], o[1][2]) || std::isgreaterequal(o[0][3], o[1][3]));
        return true;
      case V::kKillNe:
        r[0] = float(o[0][0] != o[1][0] || o[0][1] != o[1][1] || o[0][2] != o[1][2] ||
                     o[0][3] != o[1][3]);
        return true;
      case V::kDst:
        r[0] = 1.0f, r[1] = Mul(o[0][1], o[1][1]), r[2] = o[0][2], r[3] = o[1][3];
        return false;
      case V::kMaxA:
        a0 = int32_t(std::floor(std::clamp(o[0][3], -256.0f, 255.0f) + 0.5f));
        for (int i = 0; i < 4; ++i)
          r[i] = std::isgreaterequal(o[0][i], o[1][i]) ? o[0][i] : o[1][i];
        return false;
      default:
        r[0] = r[1] = r[2] = r[3] = 0.0f;
        return false;
      }
    }

    // One lane of a scalar operation: returns the new previous-scalar value.
    inline float ScalarOp(S op, const float so[2], float ps, bool &pred, int32_t &a0)
    {
      switch (op)
      {
      case S::kAdds:
      case S::kAddsc0:
      case S::kAddsc1:
        return so[0] + so[1];
      case S::kAddsPrev:
        return so[0] + ps;
      case S::kMuls:
      case S::kMulsc0:
      case S::kMulsc1:
        return Mul(so[0], so[1]);
      case S::kMulsPrev:
        return Mul(so[0], ps);
      case S::kMulsPrev2:
        if (ps == -FLT_MAX || !std::isfinite(ps) || !std::isfinite(so[1]) ||
            std::islessequal(so[1], 0.0f))
          return -FLT_MAX;
        return Mul(so[0], ps);
      case S::kMaxs:
        return std::isgreaterequal(so[0], so[1]) ? so[0] : so[1];
      case S::kMins:
        return std::isless(so[0], so[1]) ? so[0] : so[1];
      case S::kSeqs:
        return float(so[0] == 0.0f);
      case S::kSgts:
        return float(std::isgreater(so[0], 0.0f));
      case S::kSges:
        return float(std::isgreaterequal(so[0], 0.0f));
      case S::kSnes:
        return float(so[0] != 0.0f);
      case S::kFrcs:
        return so[0] - std::floor(so[0]);
      case S::kTruncs:
        return std::trunc(so[0]);
      case S::kFloors:
        return std::floor(so[0]);
      case S::kExp:
        return std::exp2(so[0]);
      case S::kLogc:
      {
        const float r = std::log2(so[0]);
        return r == -INFINITY ? -FLT_MAX : r;
      }
      case S::kLog:
        return std::log2(so[0]);
      case S::kRcpc:
      {
        const float r = 1.0f / so[0];
        return r == -INFINITY ? -FLT_MAX : r == INFINITY ? FLT_MAX
                                                         : r;
      }
      case S::kRcpf:
      {
        const float r = 1.0f / so[0];
        return r == -INFINITY ? -0.0f : r == INFINITY ? 0.0f
                                                      : r;
      }
      case S::kRcp:
        return 1.0f / so[0];
      case S::kRsqc:
      {
        const float r = 1.0f / std::sqrt(so[0]);
        return r == -INFINITY ? -FLT_MAX : r == INFINITY ? FLT_MAX
                                                         : r;
      }
      case S::kRsqf:
      {
        const float r = 1.0f / std::sqrt(so[0]);
        return r == -INFINITY ? -0.0f : r == INFINITY ? 0.0f
                                                      : r;
      }
      case S::kRsq:
        return 1.0f / std::sqrt(so[0]);
      case S::kMaxAs:
        a0 = int32_t(std::floor(std::clamp(so[0], -256.0f, 255.0f) + 0.5f));
        return std::isgreaterequal(so[0], so[1]) ? so[0] : so[1];
      case S::kMaxAsf:
        a0 = int32_t(std::floor(std::clamp(so[0], -256.0f, 255.0f)));
        return std::isgreaterequal(so[0], so[1]) ? so[0] : so[1];
      case S::kSubs:
      case S::kSubsc0:
      case S::kSubsc1:
        return so[0] - so[1];
      case S::kSubsPrev:
        return so[0] - ps;
      case S::kSetpEq:
        pred = so[0] == 0.0f;
        return float(!pred);
      case S::kSetpNe:
        pred = so[0] != 0.0f;
        return float(!pred);
      case S::kSetpGt:
        pred = std::isgreater(so[0], 0.0f);
        return float(!pred);
      case S::kSetpGe:
        pred = std::isgreaterequal(so[0], 0.0f);
        return float(!pred);
      case S::kSetpInv:
        pred = so[0] == 1.0f;
        return pred ? 0.0f : (so[0] == 0.0f ? 1.0f : so[0]);
      case S::kSetpPop:
      {
        const float c = so[0] - 1.0f;
        pred = std::islessequal(c, 0.0f);
        return pred ? 0.0f : c;
      }
      case S::kSetpClr:
        pred = false;
        return FLT_MAX;
      case S::kSetpRstr:
        pred = so[0] == 0.0f;
        return pred ? 0.0f : so[0];
      case S::kKillsEq:
        return float(so[0] == 0.0f);
      case S::kKillsGt:
        return float(std::isgreater(so[0], 0.0f));
      case S::kKillsGe:
        return float(std::isgreaterequal(so[0], 0.0f));
      case S::kKillsNe:
        return float(so[0] != 0.0f);
      case S::kKillsOne:
        return float(so[0] == 1.0f);
      case S::kSqrt:
        return std::sqrt(so[0]);
      case S::kSin:
        return std::sin(so[0]);
      case S::kCos:
        return std::cos(so[0]);
      case S::kRetainPrev:
      default:
        return ps;
      }
    }

    // CF instruction `cf` of the microcode (false if outside it)
    bool ReadCf(const uint32_t *ucode, uint32_t dwords, uint32_t cf, ucode::ControlFlowInstruction *ci)
    {
      if (3 * size_t(cf >> 1) + 3 > dwords)
        return false;
      const uint32_t *pair = &ucode[3 * (cf >> 1)];
      if (cf & 1)
      {
        ci->dword_0 = (pair[1] >> 16) | (pair[2] << 16);
        ci->dword_1 = pair[2] >> 16;
      }
      else
      {
        ci->dword_0 = pair[0];
        ci->dword_1 = pair[1] & 0xFFFF;
      }
      return true;
    }

    // Whether the shader's control flow depends on per-vertex state (the predicate), which the lanes can't share; also the number of temp registers it can touch (all with relative addressing)
    bool NeedsPerVertexFlow(const uint32_t *ucode, uint32_t dwords, uint32_t *temp_count)
    {
      using Op = ucode::ControlFlowOpcode;
      uint32_t temps = 1;
      auto use = [&](uint32_t reg, bool relative)
      {
        temps = relative ? kTempCount : std::max(temps, (reg & (kTempCount - 1)) + 1);
      };
      *temp_count = kTempCount;
      uint32_t cf_end = 2 * (dwords / 3); // shrinks to the first exec's instructions
      for (uint32_t cf = 0; cf < cf_end; ++cf)
      {
        ucode::ControlFlowInstruction ci;
        if (!ReadCf(ucode, dwords, cf, &ci))
          break;
        const Op op = ci.opcode();
        switch (op)
        {
        case Op::kCondExecPred:
        case Op::kCondExecPredEnd:
          return true;
        case Op::kCondCall:
        {
          const auto &c = *reinterpret_cast<const ucode::ControlFlowCondCallInstruction *>(&ci);
          if (!c.is_unconditional() && c.is_predicated())
            return true;
        }
        break;
        case Op::kCondJmp:
        {
          const auto &c = *reinterpret_cast<const ucode::ControlFlowCondJmpInstruction *>(&ci);
          if (!c.is_unconditional() && c.is_predicated())
            return true;
        }
        break;
        case Op::kLoopEnd:
        {
          const auto &l = *reinterpret_cast<const ucode::ControlFlowLoopEndInstruction *>(&ci);
          if (l.is_predicated_break())
            return true;
        }
        break;
        case Op::kExec:
        case Op::kExecEnd:
        case Op::kCondExec:
        case Op::kCondExecEnd:
        case Op::kCondExecPredClean:
        case Op::kCondExecPredCleanEnd:
        {
          const auto &e = *reinterpret_cast<const ucode::ControlFlowExecInstruction *>(&ci);
          cf_end = std::min(cf_end, 2 * e.address());
          for (uint32_t i = 0; i < e.count(); ++i)
          {
            if (3 * size_t(e.address() + i) + 3 > dwords)
              return true;
            const uint32_t *ins = &ucode[3 * (e.address() + i)];
            if ((e.sequence() >> (2 * i)) & 1)
            {
              const auto &f = *reinterpret_cast<const ucode::FetchInstruction *>(ins);
              if (f.is_predicated())
                return true;
              use(f.dest(), f.is_dest_relative());
              const auto &v = *reinterpret_cast<const ucode::VertexFetchInstruction *>(ins);
              use(v.src(), v.is_src_relative());
            }
            else
            {
              const auto &a = *reinterpret_cast<const ucode::AluInstruction *>(ins);
              if (a.is_predicated())
                return true;
              // (a0-relative constants are fine: operands are gathered per lane.)
              if (!a.is_export())
                use(a.vector_dest(), a.is_vector_dest_relative());
              use(a.scalar_dest(), a.is_scalar_dest_relative());
              for (uint32_t s = 1; s <= 3; ++s)
                if (a.src_is_temp(s))
                  use(ucode::AluInstruction::src_temp_reg(a.src_reg(s)),
                      ucode::AluInstruction::is_src_temp_relative(a.src_reg(s)));
              if (ucode::GetAluScalarOpcodeInfo(a.scalar_opcode()).operand_count == 2)
                use(a.scalar_const_reg_op_src_temp_reg(), false);
            }
          }
        }
        break;
        default:
          break;
        }
      }
      *temp_count = temps;
      return false;
    }

    class Interpreter
    {
    public:
      Interpreter(const uint32_t *ucode, uint32_t dwords, const VsConstants &c, uint8_t *membase,
                  MemexportStats *stats, uint32_t temp_count)
          : ucode_(ucode), dwords_(dwords), c_(c), membase_(membase), stats_(stats),
            temps_(size_t(kTempCount) * 4 * kLanes), clear_floats_(size_t(temp_count) * 4 * kLanes) {}

      // Runs 'lanes' (<= kLanes) vertices sharing control flow
      // positions: if set, receives the position export (xyzw) of each lane
      void Run(const uint32_t *vertex_indices, uint32_t lanes, float *positions = nullptr)
      {
        lanes_ = lanes;
        positions_ = positions;
        if (positions)
          for (uint32_t i = 0; i < 4 * lanes; ++i)
            positions[i] = std::nanf("");
        std::fill(temps_.begin(), temps_.begin() + clear_floats_, 0.0f);
        for (uint32_t l = 0; l < lanes; ++l)
          T(0, 0)
          [l] = float(vertex_indices[l]);
        std::memset(ps_, 0, sizeof(ps_));
        std::memset(a0_, 0, sizeof(a0_));
        std::memset(pred_, 0, sizeof(pred_));
        state_ = {};
        written_ = 0;
        address_valid_ = false;
        Execute();
        Flush();
      }

    private:
      struct State
      {
        uint32_t vfetch_full[3] = {};
        uint32_t call_stack_depth = 0;
        uint32_t call_return[4] = {};
        uint32_t loop_depth = 0;
        xenos::LoopConstant loops[4] = {};
        uint32_t loop_iterators[4] = {};

        int32_t LoopAddress() const
        {
          if (!loop_depth)
            return 0;
          const xenos::LoopConstant &l = loops[loop_depth - 1];
          return std::min(256, std::max(-256, int32_t(loop_iterators[loop_depth - 1]) * l.step +
                                                  int32_t(l.start)));
        }
      };

      float *T(uint32_t reg, uint32_t comp) { return &temps_[(size_t(reg) * 4 + comp) * kLanes]; }
      uint32_t TempIndex(uint32_t address, bool relative) const
      {
        return uint32_t(int32_t(address) + (relative ? state_.LoopAddress() : 0)) & (kTempCount - 1);
      }
      bool BoolConstant(uint32_t address) const
      {
        return (c_.bool_constants[(address >> 5) & 7] >> (address & 31)) & 1;
      }
      const float *FloatConstant(int32_t index) const
      {
        static const float kZero[4] = {};
        return (index < 0 || index >= 256) ? kZero : c_.float_constants + 4 * index;
      }

      void Execute();
      void ExecuteAlu(const ucode::AluInstruction &instr);
      void ExecuteVfetch(const uint32_t *dwords, uint32_t address);
      void StoreFetch(uint32_t dest, bool relative, uint32_t swizzle, const float (*value)[kLanes]);
      void Flush();

      const uint32_t *ucode_;
      uint32_t dwords_;
      const VsConstants &c_;
      uint8_t *membase_;
      MemexportStats *stats_;

      uint32_t lanes_ = 0;
      float *positions_ = nullptr;

    public:
      // Fetch-index capture: per full vfetch instruction address, the element index each vertex fetched (lane l of the current batch -> base + l)
      std::unordered_map<uint32_t, std::vector<int32_t>> *fetch_records_ = nullptr;
      uint32_t fetch_lane_base_ = 0;

    private:
      std::vector<float> temps_; // [reg][component][lane]
      size_t clear_floats_;      // zeroed per batch: the registers the shader uses
      float ps_[kLanes];         // previous scalar
      int32_t a0_[kLanes];
      bool pred_[kLanes]; // lane 0 steers control flow (one lane if it matters)
      uint32_t vfetch_address_[kLanes];
      State state_;

      // Memory export: eA (raw bits) and eM0-4 per lane; which eM# were written
      uint32_t address_[4][kLanes];
      bool address_valid_ = false;
      float data_[5][4][kLanes];
      uint32_t written_ = 0;
    };

    void Interpreter::Execute()
    {
      using Op = ucode::ControlFlowOpcode;
      uint32_t cf_next = 1;
      uint32_t steps = 0;
      for (uint32_t cf = 0;; cf = cf_next)
      {
        cf_next = cf + 1;
        ucode::ControlFlowInstruction ci;
        if (++steps > kMaxCfSteps || !ReadCf(ucode_, dwords_, cf, &ci))
        {
          ++stats_->malformed;
          return;
        }
        const Op op = ci.opcode();
        switch (op)
        {
        case Op::kNop:
        case Op::kMarkVsFetchDone:
          break;
        case Op::kExec:
        case Op::kExecEnd:
        case Op::kCondExec:
        case Op::kCondExecEnd:
        case Op::kCondExecPred:
        case Op::kCondExecPredEnd:
        case Op::kCondExecPredClean:
        case Op::kCondExecPredCleanEnd:
        {
          const auto &exec = *reinterpret_cast<const ucode::ControlFlowExecInstruction *>(&ci);
          bool run = true;
          if (op == Op::kCondExec || op == Op::kCondExecEnd || op == Op::kCondExecPredClean ||
              op == Op::kCondExecPredCleanEnd)
          {
            const auto &cond = *reinterpret_cast<const ucode::ControlFlowCondExecInstruction *>(&ci);
            run = cond.condition() == BoolConstant(cond.bool_address());
          }
          else if (op == Op::kCondExecPred || op == Op::kCondExecPredEnd)
          {
            const auto &cond =
                *reinterpret_cast<const ucode::ControlFlowCondExecPredInstruction *>(&ci);
            run = cond.condition() == pred_[0];
          }
          if (run)
          {
            for (uint32_t i = 0; i < exec.count(); ++i)
            {
              if (3 * size_t(exec.address() + i) + 3 > dwords_)
              {
                ++stats_->malformed;
                return;
              }
              const uint32_t *ins = &ucode_[3 * (exec.address() + i)];
              if ((exec.sequence() >> (2 * i)) & 1)
              {
                const auto &fetch = *reinterpret_cast<const ucode::FetchInstruction *>(ins);
                if (fetch.is_predicated() && fetch.predicate_condition() != pred_[0])
                  continue;
                if (fetch.opcode() == ucode::FetchOpcode::kVertexFetch)
                {
                  ExecuteVfetch(ins, exec.address() + i);
                }
                else
                {
                  float zero[4][kLanes] = {}; // texture fetches: not supported (zero)
                  StoreFetch(fetch.dest(), fetch.is_dest_relative(), fetch.dest_swizzle(), zero);
                }
              }
              else
              {
                const auto &alu = *reinterpret_cast<const ucode::AluInstruction *>(ins);
                if (alu.is_predicated() && alu.predicate_condition() != pred_[0])
                  continue;
                ExecuteAlu(alu);
              }
            }
          }

          // An *End exec ends the shader even when its condition skipped it
          if (ucode::DoesControlFlowOpcodeEndShader(op))
            return;
        }
        break;
        case Op::kLoopStart:
        {
          const auto &ls = *reinterpret_cast<const ucode::ControlFlowLoopStartInstruction *>(&ci);

          if (state_.loop_depth >= 4)
          {
            cf_next = ls.address();
            break;
          }

          xenos::LoopConstant lc;
          lc.value = c_.loop_constants ? c_.loop_constants[ls.loop_id() & 31] : 0;
          const uint32_t d = state_.loop_depth;
          state_.loops[d] = lc;
          if (!ls.is_repeat())
            state_.loop_iterators[d] = 0;
          if (state_.loop_iterators[d] >= lc.count)
          {
            cf_next = ls.address(); // skip the loop
            break;
          }
          ++state_.loop_depth;
        }
        break;
        case Op::kLoopEnd:
        {
          if (!state_.loop_depth)
            break;
          const auto &le = *reinterpret_cast<const ucode::ControlFlowLoopEndInstruction *>(&ci);
          const uint32_t d = state_.loop_depth - 1;
          const uint32_t it = ++state_.loop_iterators[d];
          if (it < state_.loops[d].count &&
              (!le.is_predicated_break() || le.condition() != pred_[0]))
          {
            cf_next = le.address();
            break;
          }
          --state_.loop_depth;
        }
        break;
        case Op::kCondCall:
        {
          if (state_.call_stack_depth >= 4)
            break;
          const auto &cc = *reinterpret_cast<const ucode::ControlFlowCondCallInstruction *>(&ci);
          if (!cc.is_unconditional())
          {
            const bool v = cc.is_predicated() ? pred_[0] : BoolConstant(cc.bool_address());
            if (cc.condition() != v)
              break;
          }
          state_.call_return[state_.call_stack_depth++] = cf + 1;
          cf_next = cc.address();
        }
        break;
        case Op::kReturn:
          if (state_.call_stack_depth)
            cf_next = state_.call_return[--state_.call_stack_depth];
          break;
        case Op::kCondJmp:
        {
          const auto &cj = *reinterpret_cast<const ucode::ControlFlowCondJmpInstruction *>(&ci);
          if (!cj.is_unconditional())
          {
            const bool v = cj.is_predicated() ? pred_[0] : BoolConstant(cj.bool_address());
            if (cj.condition() != v)
              break;
          }
          cf_next = cj.address();
        }
        break;
        case Op::kAlloc:
          Flush(); // a new export allocation ends the previous memory export
          break;
        default:
          ++stats_->malformed;
          return;
        }
      }
    }

    void Interpreter::ExecuteAlu(const ucode::AluInstruction &instr)
    {
      const uint32_t L = lanes_;
      float vr[4][kLanes];
      float sr[kLanes];

      // Gathers source operand `src` (1-3) of every lane into out[comp][lane], components in the order given by `comps` (already swizzled by the caller when it needs a subset).
      auto gather = [&](uint32_t src, bool scalar_const_op, float (*out)[kLanes], const uint32_t *comps, uint32_t ncomps, uint32_t need = 0xF)
      {
        const uint32_t reg = instr.src_reg(src);
        const uint32_t neg = uint32_t(instr.src_negate(src)) << 31;

        // Two-operand scalar ops (c#.w with r#.x) always read a constant here: src3_sel is then part of the temp register number
        if (!scalar_const_op && instr.src_is_temp(src))
        {
          const uint32_t t = TempIndex(ucode::AluInstruction::src_temp_reg(reg),
                                       ucode::AluInstruction::is_src_temp_relative(reg));
          const bool absolute = ucode::AluInstruction::is_src_temp_value_absolute(reg);
          const uint32_t abs_mask = ~(uint32_t(absolute) << 31);
          for (uint32_t j = 0; j < ncomps; ++j)
          {
            if (!((need >> j) & 1))
              continue;
            const float *p = T(t, comps[j]);
            for (uint32_t l = 0; l < L; ++l)
              out[j][l] = AsFloat((AsUint(FlushDenormal(p[l])) & abs_mask) ^ neg);
          }
          return;
        }
        const bool addressed = instr.src_const_is_addressed(src);
        if (addressed && instr.is_const_address_register_relative())
        {
          for (uint32_t l = 0; l < L; ++l)
          {
            const float *c = FloatConstant(int32_t(reg) + a0_[l]);
            for (uint32_t j = 0; j < ncomps; ++j)
              out[j][l] = AsFloat(AsUint(FlushDenormal(c[comps[j]])) ^ neg);
          }
          return;
        }
        const float *c = FloatConstant(int32_t(reg) + (addressed ? state_.LoopAddress() : 0));
        for (uint32_t j = 0; j < ncomps; ++j)
        {
          const float v = AsFloat(AsUint(FlushDenormal(c[comps[j]])) ^ neg);
          for (uint32_t l = 0; l < L; ++l)
            out[j][l] = v;
        }
      };

      // Vector operation
      const V vop = instr.vector_opcode();
      const ucode::AluVectorOpcodeInfo &vinfo = ucode::GetAluVectorOpcodeInfo(vop);
      const uint32_t vmask = instr.GetVectorOpResultWriteMask();

      if (vmask || vinfo.changed_state)
      {
        // Component-wise operations only need the written components
        const bool componentwise = !vinfo.changed_state && (vop == V::kAdd || vop == V::kMul || vop == V::kMad || vop == V::kMax || vop == V::kMin);
        const uint32_t need = componentwise ? vmask : 0xF;
        float ops[3][4][kLanes];

        for (uint32_t i = 0; i < 3; ++i)
        {
          if (!vinfo.operand_components_used[i])
            continue;
          const uint32_t swz = instr.src_swizzle(1 + i);
          uint32_t comps[4];
          for (uint32_t j = 0; j < 4; ++j)
            comps[j] = ucode::AluInstruction::GetSwizzledComponentIndex(swz, j);
          gather(1 + i, false, ops[i], comps, 4, need);
        }
        if (componentwise)
        {
          for (uint32_t j = 0; j < 4; ++j)
          {
            if (!((vmask >> j) & 1))
              continue;
            const float *a = ops[0][j], *b = ops[1][j], *c = ops[2][j];
            float *r = vr[j];
            switch (vop)
            {
            case V::kAdd:
              for (uint32_t l = 0; l < L; ++l)
                r[l] = a[l] + b[l];
              break;
            case V::kMul:
              for (uint32_t l = 0; l < L; ++l)
                r[l] = Mul(a[l], b[l]);
              break;
            case V::kMad:
              for (uint32_t l = 0; l < L; ++l)
                r[l] = Mul(a[l], b[l]) + c[l];
              break;
            case V::kMax:
              for (uint32_t l = 0; l < L; ++l)
                r[l] = std::isgreaterequal(a[l], b[l]) ? a[l] : b[l];
              break;
            default: // kMin
              for (uint32_t l = 0; l < L; ++l)
                r[l] = std::isless(a[l], b[l]) ? a[l] : b[l];
              break;
            }
          }
        }
        else
          for (uint32_t l = 0; l < L; ++l)
          {
            float o[3][4], r[4];
            for (uint32_t i = 0; i < 3; ++i)
              for (uint32_t j = 0; j < 4; ++j)
                o[i][j] = vinfo.operand_components_used[i] ? ops[i][j][l] : 0.0f;
            if (VectorOp(vop, o, r, pred_[l], a0_[l]))
              r[1] = r[2] = r[3] = r[0];
            for (uint32_t j = 0; j < 4; ++j)
              vr[j][l] = r[j];
          }
      }
      else
      {
        std::memset(vr, 0, sizeof(vr));
      }

      // Scalar operation
      const S sop = instr.scalar_opcode();
      const ucode::AluScalarOpcodeInfo &sinfo = ucode::GetAluScalarOpcodeInfo(sop);
      float so[2][kLanes];
      const uint32_t swz = instr.src_swizzle(3);
      if (sop == S::kRetainPrev)
      {
        // no operands
      }
      else if (sinfo.operand_count == 1)
      {
        const uint32_t n = sinfo.single_operand_is_two_component ? 2 : 1;
        const uint32_t comps[2] = {ucode::AluInstruction::GetSwizzledComponentIndex(swz, 3), ucode::AluInstruction::GetSwizzledComponentIndex(swz, 0)};
        gather(3, false, so, comps, n);
      }
      else if (sinfo.operand_count == 2)
      {
        // c#.w and r#.x (the temp is never relative)
        const uint32_t c_comp = ucode::AluInstruction::GetSwizzledComponentIndex(swz, 3);
        gather(3, true, so, &c_comp, 1);
        const uint32_t neg = uint32_t(instr.src_negate(3)) << 31;
        const float *p = T(instr.scalar_const_reg_op_src_temp_reg() & (kTempCount - 1), ucode::AluInstruction::GetSwizzledComponentIndex(swz, 0));
        for (uint32_t l = 0; l < L; ++l)
          so[1][l] = AsFloat(AsUint(FlushDenormal(p[l])) ^ neg);
      }

      if (sop != S::kRetainPrev)
      {
        for (uint32_t l = 0; l < L; ++l)
        {
          const float s[2] = {so[0][l], so[1][l]};
          ps_[l] = ScalarOp(sop, s, ps_[l], pred_[l], a0_[l]);
        }
      }

      for (uint32_t l = 0; l < L; ++l)
        sr[l] = instr.scalar_clamp() ? Saturate(ps_[l]) : ps_[l];
      if (instr.vector_clamp())
        for (uint32_t j = 0; j < 4; ++j)
          for (uint32_t l = 0; l < L; ++l)
            vr[j][l] = Saturate(vr[j][l]);

      const uint32_t smask = instr.GetScalarOpResultWriteMask();
      if (instr.is_export())
      {
        const uint32_t reg = instr.vector_dest();
        const uint32_t c0 = instr.GetConstant0WriteMask(), c1 = instr.GetConstant1WriteMask();
        const uint32_t mask = vmask | smask | c0 | c1;
        auto value = [&](uint32_t j, uint32_t l)
        {
          const uint32_t bit = 1u << j;
          return (vmask & bit) ? vr[j][l] : (smask & bit) ? sr[l]
                                        : (c1 & bit)      ? 1.0f
                                                          : 0.0f;
        };
        if (reg == uint32_t(ucode::ExportRegister::kExportAddress))
        {
          Flush(); // a new address ends the previous export
          for (uint32_t j = 0; j < 4; ++j)
            for (uint32_t l = 0; l < L; ++l)
              address_[j][l] = AsUint(value(j, l));
          address_valid_ = true;
        }
        else if (reg >= uint32_t(ucode::ExportRegister::kExportData0) &&
                 reg <= uint32_t(ucode::ExportRegister::kExportData4))
        {
          const uint32_t k = reg - uint32_t(ucode::ExportRegister::kExportData0);
          for (uint32_t j = 0; j < 4; ++j)
            if (mask & (1u << j))
              for (uint32_t l = 0; l < L; ++l)
                data_[k][j][l] = value(j, l);
          written_ |= 1u << k;
        }
        if (reg == uint32_t(ucode::ExportRegister::kVSPosition) && positions_)
        {
          for (uint32_t j = 0; j < 4; ++j)
            if (mask & (1u << j))
              for (uint32_t l = 0; l < L; ++l)
                positions_[4 * l + j] = value(j, l);
        }
        // Interpolators / point size: not needed
        return;
      }
      if (vmask)
      {
        const uint32_t t = TempIndex(instr.vector_dest(), instr.is_vector_dest_relative());
        for (uint32_t j = 0; j < 4; ++j)
          if (vmask & (1u << j))
            std::memcpy(T(t, j), vr[j], L * sizeof(float));
      }
      if (smask)
      {
        const uint32_t t = TempIndex(instr.scalar_dest(), instr.is_scalar_dest_relative());
        for (uint32_t j = 0; j < 4; ++j)
          if (smask & (1u << j))
            std::memcpy(T(t, j), sr, L * sizeof(float));
      }
    }

    void Interpreter::StoreFetch(uint32_t dest, bool relative, uint32_t swizzle,
                                 const float (*value)[kLanes])
    {
      const uint32_t t = TempIndex(dest, relative);
      for (uint32_t j = 0; j < 4; ++j)
      {
        float *d = T(t, j);
        const uint32_t s = (swizzle >> (3 * j)) & 7;
        if (s <= 3)
          std::memcpy(d, value[s], lanes_ * sizeof(float));
        else if (s == 5)
          std::fill(d, d + lanes_, 1.0f);
        else if (s != 7) // 7 = keep; 4 (and the invalid 6) = 0
          std::fill(d, d + lanes_, 0.0f);
      }
    }

    void Interpreter::ExecuteVfetch(const uint32_t *dw, uint32_t address)
    {
      const auto &instr = *reinterpret_cast<const ucode::VertexFetchInstruction *>(dw);
      if (!instr.is_mini_fetch())
        std::memcpy(state_.vfetch_full, dw, 12);
      const auto &full = *reinterpret_cast<const ucode::VertexFetchInstruction *>(state_.vfetch_full);
      const uint32_t slot = full.fetch_constant_index();
      const uint32_t fc0 = c_.fetch_constants[2 * slot], fc1 = c_.fetch_constants[2 * slot + 1];
      const uint64_t buffer_begin = uint64_t(fc0 & 0x1FFFFFFCu); // bytes
      const uint64_t buffer_end = buffer_begin + (fc1 & 0x03FFFFFCu);
      const uint32_t endian = fc1 & 3;
      const uint32_t L = lanes_;

      if (!instr.is_mini_fetch())
      {
        const float *src = T(TempIndex(instr.src(), instr.is_src_relative()), instr.src_swizzle());
        const float round = instr.is_index_rounded() ? 0.5f : 0.0f;
        std::vector<int32_t> *record = nullptr;
        if (fetch_records_)
        {
          record = &(*fetch_records_)[address];
          if (record->size() < fetch_lane_base_ + L)
            record->resize(fetch_lane_base_ + L, -1);
        }
        for (uint32_t l = 0; l < L; ++l)
        {
          const int32_t index = int32_t(std::floor(src[l] + round));
          vfetch_address_[l] = instr.stride() * uint32_t(index) + uint32_t(buffer_begin >> 2);
          if (record)
            (*record)[fetch_lane_base_ + l] = index;
        }
      }

      FetchElement e;
      e.format = uint32_t(instr.data_format());
      e.is_signed = instr.is_signed();
      e.normalized = instr.is_normalized();
      e.no_zero = instr.signed_rf_mode() == xenos::SignedRepeatingFractionMode::kNoZero;
      e.exp_adjust = instr.exp_adjust();
      e.out = FetchElement::Out::kFloat;
      const uint32_t bytes = DeclFormatSize(e.format);

      // 32-bit float formats: plain swapped dwords (the common case here)
      uint32_t float_words = 0;

      if (!e.exp_adjust)
      {
        switch (e.format)
        {
        case 36:
          float_words = 1;
          break; // k_32_FLOAT
        case 37:
          float_words = 2;
          break; // k_32_32_FLOAT
        case 57:
          float_words = 3;
          break; // k_32_32_32_FLOAT
        case 38:
          float_words = 4;
          break; // k_32_32_32_32_FLOAT
        default:
          break;
        }
      }

      const bool valid = bytes && (fc0 & 3) == 3;
      float result[4][kLanes];

      for (uint32_t l = 0; l < L; ++l)
      {
        const uint64_t first = uint64_t(uint32_t(int32_t(vfetch_address_[l]) + instr.offset())) * 4;
        uint32_t out[4] = {};
        if (valid && first >= buffer_begin && first + bytes <= buffer_end &&
            first + bytes <= kGuestPhysicalSize)
        {
          const uint8_t *p = membase_ + first;
          if (float_words)
          {
            for (uint32_t w = 0; w < float_words; ++w)
            {
              uint32_t v;
              std::memcpy(&v, p + 4 * w, 4);
              out[w] = Swap32(v, endian);
            }
          }
          else
          {
            DecodeElement(e, endian, p, out);
          }
        }
        else if (bytes)
        {
          ++stats_->fetch_oob;
        }
        for (uint32_t j = 0; j < 4; ++j)
          result[j][l] = AsFloat(out[j]);
      }
      StoreFetch(instr.dest(), instr.is_dest_relative(), instr.dest_swizzle(), result);
    }

    void Interpreter::Flush()
    {
      if (!written_)
        return;
      const uint32_t written = written_;
      written_ = 0;
      if (!address_valid_)
      {
        stats_->bad_address += lanes_;
        return;
      }
      for (uint32_t l = 0; l < lanes_; ++l)
      {
        const uint32_t eA[4] = {address_[0][l], address_[1][l], address_[2][l], address_[3][l]};
        // eA = stream constant + index * (0, 1, 0, 0): marker bits as Xenia checks.
        if ((eA[0] >> 30) != 1 || (eA[1] >> 23) != 0x96 || (eA[2] >> 23) != 0x96 ||
            (eA[3] >> 23) != 0x96)
        {
          ++stats_->bad_address;
          continue;
        }
        const uint32_t format_info = eA[2];
        const uint32_t index = eA[1] & 0x7FFFFF;
        const uint32_t count = eA[3] & 0x7FFFFF;
        const uint32_t base = eA[0] << 2; // dwords -> bytes, drops the marker bits
        const uint32_t endian = format_info & 7;
        stats_->last_format = (format_info >> 8) & 0x3F;
        stats_->last_base = base;
        stats_->last_count = count;
        const uint32_t format = (format_info >> 8) & 0x3F;
        const bool swap_rb = (format_info >> 19) & 1;
        if ((format == 36 || format == 37 || format == 38) && endian <= 3 && !swap_rb)
        {
          // 32-bit float formats (what this game uses): no packing.
          const uint32_t n = format == 36 ? 1 : format == 37 ? 2
                                                             : 4;
          for (uint32_t k = 0; k < 5; ++k)
          {
            if (!((written >> k) & 1) || index + k >= count)
              continue;
            const uint64_t address = uint64_t(base) + uint64_t(index + k) * 4 * n;
            if (address + 4 * n > kGuestPhysicalSize)
            {
              ++stats_->out_of_range;
              continue;
            }
            uint8_t *dst = membase_ + address;
            for (uint32_t j = 0; j < n; ++j)
            {
              const uint32_t v = Swap32(AsUint(data_[k][j][l]), endian);
              std::memcpy(dst + 4 * j, &v, 4);
            }
            ++stats_->elements;
          }
          continue;
        }
        for (uint32_t k = 0; k < 5; ++k)
        {
          if (!((written >> k) & 1) || index + k >= count)
            continue;
          const float value[4] = {data_[k][0][l], data_[k][1][l], data_[k][2][l], data_[k][3][l]};
          uint32_t packed[4];
          const uint32_t size = PackElement(value, format_info, packed);
          if (!size)
          {
            ++stats_->bad_format;
            continue;
          }
          const uint64_t address = uint64_t(base) + uint64_t(index + k) * size;
          if (address + size > kGuestPhysicalSize)
          {
            ++stats_->out_of_range;
            continue;
          }
          Swap128(packed, endian);

          // Little-endian dwords = guest byte order; 8 / 16-bit elements take the low bytes of the swapped dword (as Xenia replaces bytes in place)
          std::memcpy(membase_ + address, packed, size);
          ++stats_->elements;
        }
      }
    }
  }

  // Persistent worker threads: a job is split into parts that the workers and the calling thread take in turn
  struct MemexportRunner::Pool
  {
    explicit Pool(uint32_t n)
    {
      for (uint32_t i = 0; i < n; ++i)
        threads.emplace_back([this]
                             { Worker(); });
    }
    ~Pool()
    {
      {
        std::lock_guard lock(mutex);
        quit = true;
      }
      wake.notify_all();
      for (auto &t : threads)
        t.join();
    }
    // Runs fn(0..parts-1) on the workers and the caller; returns when all are done
    void Run(uint32_t parts, const std::function<void(uint32_t)> &fn)
    {
      {
        std::lock_guard lock(mutex);
        job = &fn;
        job_parts = parts;
        next.store(0);
        remaining = parts;
        ++generation;
      }
      wake.notify_all();
      Work(fn, parts);
      std::unique_lock lock(mutex);

      // Also wait for workers to leave the job, so none can pick up a part index of the next job with this one's function
      done.wait(lock, [&]
                { return remaining == 0 && active == 0; });
      job = nullptr;
    }

  private:
    void Work(const std::function<void(uint32_t)> &fn, uint32_t parts)
    {
      for (uint32_t part; (part = next.fetch_add(1)) < parts;)
      {
        fn(part);
        std::lock_guard lock(mutex);
        if (--remaining == 0)
          done.notify_all();
      }
    }
    void Worker()
    {
      uint64_t seen = 0;
      for (;;)
      {
        const std::function<void(uint32_t)> *fn;
        uint32_t parts;
        {
          std::unique_lock lock(mutex);
          wake.wait(lock, [&]
                    { return quit || (generation != seen && job); });
          if (quit)
            return;
          seen = generation;
          fn = job;
          parts = job_parts;
          ++active;
        }
        Work(*fn, parts);
        std::lock_guard lock(mutex);
        if (--active == 0)
          done.notify_all();
      }
    }

  public:
    std::vector<std::thread> threads;

  private:
    std::mutex mutex;
    std::condition_variable wake, done;
    const std::function<void(uint32_t)> *job = nullptr;
    uint32_t job_parts = 0, remaining = 0, active = 0;
    std::atomic<uint32_t> next{0};
    uint64_t generation = 0;
    bool quit = false;
  };

  MemexportRunner::MemexportRunner(uint8_t *physical_membase, uint32_t worker_threads)
      : membase_(physical_membase)
  {
    if (worker_threads)
      pool_ = std::make_unique<Pool>(worker_threads);
  }
  MemexportRunner::~MemexportRunner() = default;
  uint32_t MemexportRunner::worker_threads() const
  {
    return pool_ ? uint32_t(pool_->threads.size()) : 0;
  }

  bool MemexportRunner::Run(const uint32_t *ucode, uint32_t dwords, const VsConstants &constants,
                            const uint32_t *vertex_indices, uint32_t count,
                            MemexportStats *stats, bool force_per_vertex)
  {
    if (!ucode || dwords < 3 || !constants.float_constants || !constants.bool_constants ||
        !constants.fetch_constants || !membase_)
      return false;
    uint32_t temp_count;
    const bool per_vertex = NeedsPerVertexFlow(ucode, dwords, &temp_count) || force_per_vertex;
    const uint32_t lanes = per_vertex ? 1 : kLanes;
    if (per_vertex)
      ++stats->per_vertex_draws;

    // Vertices are independent and write disjoint elements: large draws are split across the worker threads (parts of 8 batches)
    constexpr uint32_t kPartVertices = 8 * kLanes;
    if (pool_ && !per_vertex && count >= 2 * kPartVertices)
    {
      const uint32_t parts = (count + kPartVertices - 1) / kPartVertices;
      std::mutex stats_mutex;
      const std::function<void(uint32_t)> fn = [&](uint32_t part)
      {
        MemexportStats local;
        Interpreter interpreter(ucode, dwords, constants, membase_, &local, temp_count);
        const uint32_t begin = part * kPartVertices;
        const uint32_t end = std::min(count, begin + kPartVertices);
        for (uint32_t i = begin; i < end; i += lanes)
          interpreter.Run(vertex_indices + i, std::min(lanes, end - i));
        std::lock_guard lock(stats_mutex);
        stats->elements += local.elements;
        stats->bad_address += local.bad_address;
        stats->bad_format += local.bad_format;
        stats->out_of_range += local.out_of_range;
        stats->fetch_oob += local.fetch_oob;
        stats->malformed += local.malformed;
        if (local.elements)
        {
          stats->last_format = local.last_format;
          stats->last_base = local.last_base;
          stats->last_count = local.last_count;
        }
      };
      pool_->Run(parts, fn);
    }
    else
    {
      Interpreter interpreter(ucode, dwords, constants, membase_, stats, temp_count);
      for (uint32_t i = 0; i < count; i += lanes)
        interpreter.Run(vertex_indices + i, std::min(lanes, count - i));
    }
    stats->vertices += count;
    return true;
  }

  bool MemexportRunner::RunFetchIndices(
      const uint32_t *ucode, uint32_t dwords, const VsConstants &constants,
      const uint32_t *vertex_indices, uint32_t count,
      std::unordered_map<uint32_t, std::vector<int32_t>> *records)
  {
    if (!ucode || dwords < 3 || !constants.float_constants || !constants.bool_constants ||
        !constants.fetch_constants || !membase_ || !records)
      return false;
    records->clear();
    uint32_t temp_count;
    const bool per_vertex = NeedsPerVertexFlow(ucode, dwords, &temp_count);
    const uint32_t lanes = per_vertex ? 1 : kLanes;
    MemexportStats ignored;
    Interpreter interpreter(ucode, dwords, constants, membase_, &ignored, temp_count);
    interpreter.fetch_records_ = records;
    for (uint32_t i = 0; i < count; i += lanes)
    {
      interpreter.fetch_lane_base_ = i;
      interpreter.Run(vertex_indices + i, std::min(lanes, count - i));
    }
    return true;
  }

  bool MemexportRunner::RunPositions(const uint32_t *ucode, uint32_t dwords,
                                     const VsConstants &constants, const uint32_t *vertex_indices,
                                     uint32_t count, float *positions, MemexportStats *stats)
  {
    if (!ucode || dwords < 3 || !constants.float_constants || !constants.bool_constants ||
        !constants.fetch_constants || !membase_)
      return false;
    uint32_t temp_count;
    const bool per_vertex = NeedsPerVertexFlow(ucode, dwords, &temp_count);
    const uint32_t lanes = per_vertex ? 1 : kLanes;
    Interpreter interpreter(ucode, dwords, constants, membase_, stats, temp_count);
    for (uint32_t i = 0; i < count; i += lanes)
      interpreter.Run(vertex_indices + i, std::min(lanes, count - i), positions + 4 * size_t(i));
    return true;
  }

  bool FloatConstantsRead(const uint32_t *ucode, uint32_t dwords, uint64_t used[4])
  {
    using Op = ucode::ControlFlowOpcode;
    used[0] = used[1] = used[2] = used[3] = 0;
    uint32_t cf_end = 2 * (dwords / 3);
    for (uint32_t cf = 0; cf < cf_end; ++cf)
    {
      ucode::ControlFlowInstruction ci;
      if (!ReadCf(ucode, dwords, cf, &ci))
        break;
      const Op op = ci.opcode();
      if (op != Op::kExec && op != Op::kExecEnd && op != Op::kCondExec && op != Op::kCondExecEnd &&
          op != Op::kCondExecPred && op != Op::kCondExecPredEnd && op != Op::kCondExecPredClean &&
          op != Op::kCondExecPredCleanEnd)
        continue;
      const auto &e = *reinterpret_cast<const ucode::ControlFlowExecInstruction *>(&ci);
      cf_end = std::min(cf_end, 2 * e.address());
      for (uint32_t i = 0; i < e.count() && 3 * size_t(e.address() + i) + 3 <= dwords; ++i)
      {
        if ((e.sequence() >> (2 * i)) & 1)
          continue; // fetch
        const auto &a = *reinterpret_cast<const ucode::AluInstruction *>(&ucode[3 * (e.address() + i)]);
        const bool scalar_const_op =
            ucode::GetAluScalarOpcodeInfo(a.scalar_opcode()).operand_count == 2;
        for (uint32_t s = 1; s <= 3; ++s)
        {
          if (a.src_is_temp(s) && !(s == 3 && scalar_const_op))
            continue;
          if (a.src_const_is_addressed(s))
            return false;
          const uint32_t reg = a.src_reg(s) & 0xFF;
          used[reg >> 6] |= uint64_t(1) << (reg & 63);
        }
      }
    }
    return true;
  }

  uint32_t VertexFetchSlots(const uint32_t *ucode, uint32_t dwords, uint32_t *slots, uint32_t max)
  {
    using Op = ucode::ControlFlowOpcode;
    uint32_t n = 0;
    uint32_t cf_end = 2 * (dwords / 3);
    for (uint32_t cf = 0; cf < cf_end; ++cf)
    {
      ucode::ControlFlowInstruction ci;
      if (!ReadCf(ucode, dwords, cf, &ci))
        break;
      const Op op = ci.opcode();
      if (op != Op::kExec && op != Op::kExecEnd && op != Op::kCondExec && op != Op::kCondExecEnd &&
          op != Op::kCondExecPred && op != Op::kCondExecPredEnd && op != Op::kCondExecPredClean &&
          op != Op::kCondExecPredCleanEnd)
        continue;
      const auto &e = *reinterpret_cast<const ucode::ControlFlowExecInstruction *>(&ci);
      cf_end = std::min(cf_end, 2 * e.address());
      for (uint32_t i = 0; i < e.count() && 3 * size_t(e.address() + i) + 3 <= dwords; ++i)
      {
        if (!((e.sequence() >> (2 * i)) & 1))
          continue;
        const uint32_t *ins = &ucode[3 * (e.address() + i)];
        const auto &f = *reinterpret_cast<const ucode::FetchInstruction *>(ins);
        if (f.opcode() != ucode::FetchOpcode::kVertexFetch)
          continue;
        const auto &v = *reinterpret_cast<const ucode::VertexFetchInstruction *>(ins);
        if (v.is_mini_fetch())
          continue;
        const uint32_t slot = v.fetch_constant_index();
        if (std::find(slots, slots + n, slot) == slots + n && n < max)
          slots[n++] = slot;
      }
    }
    return n;
  }

  uint32_t TextureFetchUsage(const uint32_t *ucode, uint32_t dwords, TextureFetchUse uses[32])
  {
    using Op = ucode::ControlFlowOpcode;
    using FOp = ucode::FetchOpcode;
    for (uint32_t i = 0; i < 32; ++i)
      uses[i] = TextureFetchUse{};
    uint32_t mask = 0;
    uint32_t cf_end = 2 * (dwords / 3);
    for (uint32_t cf = 0; cf < cf_end; ++cf)
    {
      ucode::ControlFlowInstruction ci;
      if (!ReadCf(ucode, dwords, cf, &ci))
        break;
      const Op op = ci.opcode();
      if (op != Op::kExec && op != Op::kExecEnd && op != Op::kCondExec && op != Op::kCondExecEnd &&
          op != Op::kCondExecPred && op != Op::kCondExecPredEnd && op != Op::kCondExecPredClean &&
          op != Op::kCondExecPredCleanEnd)
        continue;
      const auto &e = *reinterpret_cast<const ucode::ControlFlowExecInstruction *>(&ci);
      cf_end = std::min(cf_end, 2 * e.address());
      for (uint32_t i = 0; i < e.count() && 3 * size_t(e.address() + i) + 3 <= dwords; ++i)
      {
        if (!((e.sequence() >> (2 * i)) & 1))
          continue;
        const uint32_t *ins = &ucode[3 * (e.address() + i)];
        const auto &t = *reinterpret_cast<const ucode::TextureFetchInstruction *>(ins);
        const FOp fop = t.opcode();
        if (fop != FOp::kTextureFetch && fop != FOp::kGetTextureWeights &&
            fop != FOp::kGetTextureComputedLod && fop != FOp::kGetTextureGradients)
          continue;
        const uint32_t index = t.fetch_constant_index() & 31;
        TextureFetchUse &u = uses[index];
        const uint8_t dim = uint8_t(uint32_t(t.dimension()) + 1);
        if (!u.dimension)
        {
          u.dimension = dim;
          u.mag_filter = uint8_t(t.mag_filter());
          u.min_filter = uint8_t(t.min_filter());
          u.mip_filter = uint8_t(t.mip_filter());
          u.aniso_filter = uint8_t(t.aniso_filter());
          u.lod_bias = t.lod_bias();
          u.unnormalized = t.unnormalized_coordinates();
        }
        else if (u.dimension != dim)
        {
          u.mixed_dimensions = true;
        }
        ++u.count;
        mask |= 1u << index;
      }
    }
    return mask;
  }

  uint32_t MemexportElementSize(uint32_t format)
  {
    const float zero[4] = {};
    uint32_t packed[4];
    return PackElement(zero, format << 8, packed);
  }

  std::string DescribeMicrocode(const uint32_t *ucode, uint32_t dwords)
  {
    std::string s;
    for (uint32_t cf = 0; cf < 64 && 3 * size_t(cf >> 1) + 3 <= dwords; ++cf)
    {
      const uint32_t *pair = &ucode[3 * (cf >> 1)];
      ucode::ControlFlowInstruction ci;
      if (cf & 1)
      {
        ci.dword_0 = (pair[1] >> 16) | (pair[2] << 16);
        ci.dword_1 = pair[2] >> 16;
      }
      else
      {
        ci.dword_0 = pair[0];
        ci.dword_1 = pair[1] & 0xFFFF;
      }
      const auto op = ci.opcode();
      using Op = ucode::ControlFlowOpcode;
      if (op == Op::kAlloc)
      {
        const auto &a = *reinterpret_cast<const ucode::ControlFlowAllocInstruction *>(&ci);
        s += fmt::format(" | alloc {}", uint32_t(a.alloc_type()));
        continue;
      }
      if (op != Op::kExec && op != Op::kExecEnd)
      {
        s += fmt::format(" | cf op {}", uint32_t(op));
        if (op == Op::kNop)
          continue;
        break;
      }
      const auto &e = *reinterpret_cast<const ucode::ControlFlowExecInstruction *>(&ci);
      s += " | exec";
      for (uint32_t i = 0; i < e.count() && 3 * size_t(e.address() + i) + 3 <= dwords; ++i)
      {
        const uint32_t *ins = &ucode[3 * (e.address() + i)];
        if ((e.sequence() >> (2 * i)) & 1)
        {
          const auto &v = *reinterpret_cast<const ucode::VertexFetchInstruction *>(ins);
          s += fmt::format(" vfetch r{} f{} c{} +{} /{} swz{:03o};", v.dest(),
                           uint32_t(v.data_format()), v.fetch_constant_index(), v.offset(),
                           v.stride(), v.dest_swizzle());
        }
        else
        {
          const auto &a = *reinterpret_cast<const ucode::AluInstruction *>(ins);
          s += fmt::format(" {}/{} {}{}.{:X};", ucode::GetAluVectorOpcodeInfo(a.vector_opcode()).name,
                           ucode::GetAluScalarOpcodeInfo(a.scalar_opcode()).name,
                           a.is_export() ? "e" : "r", a.vector_dest(), a.vector_write_mask());
        }
      }
      if (op == Op::kExecEnd)
        break;
    }
    return s;
  }
}