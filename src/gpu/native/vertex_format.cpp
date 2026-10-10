// sst_rematch - native renderer: vertex format mapping (see vertex_format.h).

#include "gpu/native/vertex_format.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace sst::native
{

  namespace
  {

    // xenos::VertexFormat
    enum : uint32_t
    {
      k_8_8_8_8 = 6,
      k_2_10_10_10 = 7,
      k_10_11_11 = 16,
      k_11_11_10 = 17,
      k_16_16 = 25,
      k_16_16_16_16 = 26,
      k_16_16_FLOAT = 31,
      k_16_16_16_16_FLOAT = 32,
      k_32 = 33,
      k_32_32 = 34,
      k_32_32_32_32 = 35,
      k_32_FLOAT = 36,
      k_32_32_FLOAT = 37,
      k_32_32_32_32_FLOAT = 38,
      k_32_32_32_FLOAT = 57,
    };

    bool IsSigned(uint32_t type) { return (type >> 8) & 1; }
    bool IsInteger(uint32_t type) { return (type >> 9) & 1; }
    uint32_t Swizzle(uint32_t type, int component) { return (type >> (10 + 3 * component)) & 7; }

    // Components the DXGI format provides; missing ones read as 0,0,0,1.
    uint32_t FormatComponents(uint32_t format)
    {
      switch (format)
      {
      case k_32:
      case k_32_FLOAT:
      case k_2_10_10_10: // fetched as one packed value when mapped to R32_UINT
      case k_10_11_11:
      case k_11_11_10:
        return 1;
      case k_16_16:
      case k_16_16_FLOAT:
      case k_32_32:
      case k_32_32_FLOAT:
        return 2;
      case k_32_32_32_FLOAT:
        return 3;
      default:
        return 4;
      }
    }

    // True if the declaration's swizzle is what the DXGI format gives by itself: xyzw in order, then default fills.
    bool SwizzleIsNatural(uint32_t type, uint32_t components)
    {
      for (uint32_t i = 0; i < 4; ++i)
      {
        const uint32_t s = Swizzle(type, int(i));
        if (i < components)
        {
          if (s != i)
            return false;
        }
        else if (s != (i == 3 ? 5u : 4u) && s != i)
        {
          return false;
        }
      }
      return true;
    }

    bool SwizzleIsZYXW(uint32_t type)
    {
      return Swizzle(type, 0) == 2 && Swizzle(type, 1) == 1 && Swizzle(type, 2) == 0 &&
             Swizzle(type, 3) == 3;
    }

  }

  uint32_t DeclFormat(uint32_t type) { return type & 0x3F; }
  uint32_t DeclEndian(uint32_t type) { return (type >> 6) & 3; }

  uint32_t DeclFormatSize(uint32_t format)
  {
    switch (format)
    {
    case k_8_8_8_8:
    case k_2_10_10_10:
    case k_10_11_11:
    case k_11_11_10:
    case k_16_16:
    case k_16_16_FLOAT:
    case k_32:
    case k_32_FLOAT:
      return 4;
    case k_16_16_16_16:
    case k_16_16_16_16_FLOAT:
    case k_32_32:
    case k_32_32_FLOAT:
      return 8;
    case k_32_32_32_FLOAT:
      return 12;
    case k_32_32_32_32:
    case k_32_32_32_32_FLOAT:
      return 16;
    default:
      return 0;
    }
  }

  bool UsageIsUint(uint8_t usage)
  {
    // D3DDECLUSAGE_BLENDINDICES 2, NORMAL 3, TANGENT 6, BINORMAL 7
    return usage == 2 || usage == 3 || usage == 6 || usage == 7;
  }

  const char *UsageSemantic(uint8_t usage)
  {
    static const char *const kNames[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL",
                                         "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
                                         "TESSFACTOR", "POSITIONT", "COLOR", "FOG",
                                         "DEPTH", "SAMPLE"};
    return usage < std::size(kNames) ? kNames[usage] : nullptr;
  }

  std::string DescribeType(uint32_t type)
  {
    static const char *const kSw = "xyzw01??";
    std::string s = "fmt " + std::to_string(DeclFormat(type)) + " endian " +
                    std::to_string(DeclEndian(type)) + (IsSigned(type) ? " signed" : " unsigned") +
                    (IsInteger(type) ? " int" : " norm") + " swizzle ";
    for (int i = 0; i < 4; ++i)
      s += kSw[Swizzle(type, i)];
    return s;
  }

  ElementMapping MapElement(uint32_t type, uint8_t usage)
  {
    ElementMapping m;
    const uint32_t format = DeclFormat(type);
    const bool sgn = IsSigned(type), integer = IsInteger(type);
    const bool want_uint = UsageIsUint(usage);
    bool natural = SwizzleIsNatural(type, FormatComponents(format));

    switch (format)
    {
    case k_8_8_8_8:
      if (want_uint)
      {
        m.format = sgn ? DXGI_FORMAT_R8G8B8A8_SINT : DXGI_FORMAT_R8G8B8A8_UINT;
        if (!integer)
          m.note = "normalized 8888 read by a uint input (raw 0-255)";
      }
      else if (integer)
      {
        m.format = sgn ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        m.note = "integer 8888 read as normalized";
      }
      else if (!sgn && SwizzleIsZYXW(type))
      {
        m.format = DXGI_FORMAT_B8G8R8A8_UNORM; // D3DCOLOR
        natural = true;
      }
      else
      {
        m.format = sgn ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
      }
      break;
    case k_2_10_10_10:
      if (want_uint)
      {
        m.format = DXGI_FORMAT_R32_UINT;
        m.note = "2_10_10_10 into a uint input (raw bits)";
      }
      else if (!sgn)
      {
        m.format = integer ? DXGI_FORMAT_R10G10B10A2_UINT : DXGI_FORMAT_R10G10B10A2_UNORM;
        if (integer)
          m.note = "integer 2_10_10_10 into a float input";
      }
      else
      {
        m.format = DXGI_FORMAT_R10G10B10A2_UNORM;
        m.note = "signed 2_10_10_10 has no DXGI format";
      }
      break;
    case k_10_11_11:
    case k_11_11_10:
      m.format = DXGI_FORMAT_R32_UINT;
      if (want_uint && usage != 2)
      {
        m.r11g11b10 = true;
        if (format == k_10_11_11)
          m.note = "10_11_11 decoded as 11_11_10";
      }
      else
      {
        m.note = "packed 11/10-bit format outside NORMAL/TANGENT/BINORMAL";
      }
      natural = true;
      break;
    case k_16_16:
      if (want_uint || integer)
      {
        m.format = sgn ? DXGI_FORMAT_R16G16_SINT : DXGI_FORMAT_R16G16_UINT;
        if (!want_uint)
        {
          m.format = sgn ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_UNORM;
          m.note = "integer 16_16 into a float input (scaled as normalized)";
        }
      }
      else
      {
        m.format = sgn ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_UNORM;
      }
      break;
    case k_16_16_16_16:
      if (want_uint)
      {
        m.format = sgn ? DXGI_FORMAT_R16G16B16A16_SINT : DXGI_FORMAT_R16G16B16A16_UINT;
      }
      else
      {
        m.format = sgn ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R16G16B16A16_UNORM;
        if (integer)
          m.note = "integer 16_16_16_16 into a float input (scaled as normalized)";
      }
      break;
    case k_16_16_FLOAT:
      m.format = want_uint ? DXGI_FORMAT_R16G16_UINT : DXGI_FORMAT_R16G16_FLOAT;
      if (want_uint)
        m.note = "half floats into a uint input";
      break;
    case k_16_16_16_16_FLOAT:
      m.format = want_uint ? DXGI_FORMAT_R16G16B16A16_UINT : DXGI_FORMAT_R16G16B16A16_FLOAT;
      if (want_uint)
        m.note = "half floats into a uint input";
      break;
    case k_32:
      m.format = want_uint ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT;
      if (!want_uint)
        m.note = "32-bit integer into a float input (raw bits)";
      break;
    case k_32_32:
      m.format = want_uint ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32_FLOAT;
      if (!want_uint)
        m.note = "32-bit integers into a float input (raw bits)";
      break;
    case k_32_32_32_32:
      m.format = want_uint ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
      if (!want_uint)
        m.note = "32-bit integers into a float input (raw bits)";
      break;
    // Floats into uint inputs keep their bits: the shader does asfloat().
    case k_32_FLOAT:
      m.format = want_uint ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT;
      break;
    case k_32_32_FLOAT:
      m.format = want_uint ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32_FLOAT;
      break;
    case k_32_32_32_FLOAT:
      m.format = want_uint ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32B32_FLOAT;
      break;
    case k_32_32_32_32_FLOAT:
      m.format = want_uint ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
      break;
    default:
      m.note = "unknown vertex format";
      return m;
    }
    if (!natural && !m.note)
      m.note = "swizzle not applied";
    return m;
  }

  void SwapVertexData(uint32_t endian, const uint8_t *src, uint8_t *dst, size_t bytes)
  {
    const size_t words = bytes / 4;
    switch (endian)
    {
    case 1: // 8in16
      for (size_t i = 0; i < words; ++i)
      {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
        std::memcpy(dst + i * 4, &v, 4);
      }
      break;
    case 2: // 8in32
      for (size_t i = 0; i < words; ++i)
      {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = __builtin_bswap32(v);
        std::memcpy(dst + i * 4, &v, 4);
      }
      break;
    case 3: // 16in32
      for (size_t i = 0; i < words; ++i)
      {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = (v << 16) | (v >> 16);
        std::memcpy(dst + i * 4, &v, 4);
      }
      break;
    default:
      std::memcpy(dst, src, words * 4);
      break;
    }
  }

  namespace
  {

    float HalfToFloat(uint16_t h)
    {
      const uint32_t sign = uint32_t(h & 0x8000) << 16;
      uint32_t exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
      uint32_t bits;
      if (exp == 0)
      {
        if (mant == 0)
        {
          bits = sign;
        }
        else
        { // subnormal
          exp = 1;
          while (!(mant & 0x400))
          {
            mant <<= 1;
            --exp;
          }
          mant &= 0x3FF;
          bits = sign | ((exp + 112) << 23) | (mant << 13);
        }
      }
      else if (exp == 31)
      { // Xbox 360 half has no inf/NaN: extended range
        bits = sign | ((exp + 112) << 23) | (mant << 13);
      }
      else
      {
        bits = sign | ((exp + 112) << 23) | (mant << 13);
      }
      float f;
      std::memcpy(&f, &bits, 4);
      return f;
    }

  } // namespace

  void DecodeElement(const FetchElement &e, uint32_t endian, const uint8_t *vertex, uint32_t out[4])
  {
    const uint32_t size = DeclFormatSize(e.format);
    uint32_t words[4] = {};
    if (size)
    {
      uint8_t tmp[16];
      SwapVertexData(endian, vertex + e.offset, tmp, size);
      std::memcpy(words, tmp, size);
    }

    float c[6] = {0, 0, 0, 0, 0, 1};
    int widths[4] = {}, offsets[4] = {}, word_of[4] = {};
    int packed = 0; // component count of a packed integer format
    switch (e.format)
    {
    case k_8_8_8_8:
      packed = 4;
      widths[0] = widths[1] = widths[2] = widths[3] = 8;
      offsets[1] = 8, offsets[2] = 16, offsets[3] = 24;
      break;
    case k_2_10_10_10:
      packed = 4;
      widths[0] = widths[1] = widths[2] = 10, widths[3] = 2;
      offsets[1] = 10, offsets[2] = 20, offsets[3] = 30;
      break;
    case k_10_11_11:
      packed = 3;
      widths[0] = widths[1] = 11, widths[2] = 10;
      offsets[1] = 11, offsets[2] = 22;
      break;
    case k_11_11_10:
      packed = 3;
      widths[0] = 10, widths[1] = widths[2] = 11;
      offsets[1] = 10, offsets[2] = 21;
      break;
    case k_16_16:
      packed = 2;
      widths[0] = widths[1] = 16;
      offsets[1] = 16;
      break;
    case k_16_16_16_16:
      packed = 4;
      widths[0] = widths[1] = widths[2] = widths[3] = 16;
      offsets[1] = offsets[3] = 16;
      word_of[2] = word_of[3] = 1;
      break;
    case k_16_16_FLOAT:
    case k_16_16_16_16_FLOAT:
      for (int i = 0; i < (e.format == k_16_16_FLOAT ? 2 : 4); ++i)
        c[i] = HalfToFloat(uint16_t(words[i >> 1] >> ((i & 1) * 16)));
      break;
    case k_32:
    case k_32_32:
    case k_32_32_32_32:
    {
      const int n = e.format == k_32 ? 1 : e.format == k_32_32 ? 2
                                                               : 4;
      for (int i = 0; i < n; ++i)
      {
        double v = e.is_signed ? double(int32_t(words[i])) : double(words[i]);
        if (e.normalized)
        {
          if (e.is_signed)
            v = e.no_zero ? (v + 0.5) / 2147483647.5 : std::max(v / 2147483647.0, -1.0);
          else
            v /= 4294967295.0;
        }
        c[i] = float(v);
      }
      break;
    }
    case k_32_FLOAT:
    case k_32_32_FLOAT:
    case k_32_32_32_FLOAT:
    case k_32_32_32_32_FLOAT:
      std::memcpy(c, words, size);
      break;
    default:
      break;
    }
    for (int i = 0; i < packed; ++i)
    {
      const uint32_t w = words[word_of[i]];
      const int width = widths[i];
      uint32_t bits = (w >> offsets[i]) & ((1u << width) - 1);
      double v;
      if (e.is_signed)
      {
        int32_t sv = int32_t(bits << (32 - width)) >> (32 - width);
        v = sv;
        if (e.normalized)
        {
          const double scale = double((1u << (width - 1)) - 1) + (e.no_zero ? 0.5 : 0.0);
          v = e.no_zero ? (v + 0.5) / scale : std::max(v / scale, -1.0);
        }
      }
      else
      {
        v = bits;
        if (e.normalized)
          v /= double((1u << width) - 1);
      }
      c[i] = float(v);
    }
    if (e.exp_adjust)
    {
      const float scale = std::ldexp(1.0f, e.exp_adjust);
      for (int i = 0; i < 4; ++i)
        c[i] *= scale;
    }

    for (int i = 0; i < 4; ++i)
    {
      const float v = c[e.sel[i] < 6 ? e.sel[i] : 4];
      switch (e.out)
      {
      case FetchElement::Out::kUint:
        out[i] = v <= 0.0f ? 0u : uint32_t(v + 0.5f);
        break;
      default:
        std::memcpy(&out[i], &v, 4);
        break;
      }
    }
  }
}