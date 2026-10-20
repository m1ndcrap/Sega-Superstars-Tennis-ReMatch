// Native renderer: guest texture formats on the host, the DXGI format, swizzle and conversion for each Xenos texture format, the GPU's endian swaps, and sampler address modes (texture_cache.cpp)

#pragma once

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>

#include "gpu/native/gfx/d3d_vocab.h"

#include <cstdint>
#include <cstring>

namespace sst::native::texture_format
{
  namespace xenos = rex::graphics::xenos;
  using rex::graphics::FormatInfo;

  enum class Load : uint8_t
  {
    kNone,
    kCopy,
    k565,
    k1555,
    k4444,
    kDepthUnorm,
    kDepthFloat
  };

  constexpr uint16_t Swz(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
  {
    return uint16_t(x | (y << 3) | (z << 6) | (w << 9));
  }

  constexpr uint16_t kRGBA = Swz(0, 1, 2, 3), kRGGG = Swz(0, 1, 1, 1), kRRRR = Swz(0, 0, 0, 0), kRGBB = Swz(0, 1, 2, 2);

  // Host representation of a guest format. `swizzle` maps the guest's data components (x y z w) to host components (as in Xenia's host format table)
  struct HostFormat
  {
    DXGI_FORMAT unorm = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT snorm = DXGI_FORMAT_UNKNOWN; // UNKNOWN: no signed variant
    Load load = Load::kNone;
    uint8_t host_bpb = 0; // bytes per host block (texel for uncompressed)
    bool block = false;   // host format is 4x4 block-compressed
    uint16_t swizzle = kRGBA;
  };

  inline HostFormat GetHostFormat(uint32_t format)
  {
    using F = xenos::TextureFormat;

    auto plain = [](DXGI_FORMAT u, DXGI_FORMAT s, uint8_t bpb, uint16_t swz)
    {
      return HostFormat{u, s, Load::kCopy, bpb, false, swz};
    };

    auto bc = [](DXGI_FORMAT u, uint8_t bpb, uint16_t swz)
    {
      return HostFormat{u, DXGI_FORMAT_UNKNOWN, Load::kCopy, bpb, true, swz};
    };

    switch (F(format))
    {
    case F::k_8:
    case F::k_8_A:
      return plain(DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, 1, kRRRR);

    case F::k_1_5_5_5:
      return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, Load::k1555, 4, false, kRGBA};

    case F::k_5_6_5:
      return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, Load::k565, 4, false, kRGBB};

    case F::k_4_4_4_4:
      return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, Load::k4444, 4, false, kRGBA};

    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_AS_16_16_16_16:
      return plain(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, 4, kRGBA);

    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16:
      return plain(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, 4, kRGBA);

    case F::k_8_8:
      return plain(DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM, 2, kRGGG);

    case F::k_DXT1:
    case F::k_DXT1_AS_16_16_16_16:
      return bc(DXGI_FORMAT_BC1_UNORM, 8, kRGBA);

    case F::k_DXT2_3:
    case F::k_DXT2_3_AS_16_16_16_16:
      return bc(DXGI_FORMAT_BC2_UNORM, 16, kRGBA);

    case F::k_DXT4_5:
    case F::k_DXT4_5_AS_16_16_16_16:
      return bc(DXGI_FORMAT_BC3_UNORM, 16, kRGBA);

    case F::k_DXN:
      return bc(DXGI_FORMAT_BC5_UNORM, 16, kRGGG);

    case F::k_DXT5A:
      return bc(DXGI_FORMAT_BC4_UNORM, 8, kRRRR);

    case F::k_24_8:
      return {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, Load::kDepthUnorm, 4, false, kRRRR};

    case F::k_24_8_FLOAT:
      return {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, Load::kDepthFloat, 4, false, kRRRR};

    case F::k_16:
      return plain(DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, 2, kRRRR);

    case F::k_16_16:
      return plain(DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM, 4, kRGGG);

    case F::k_16_16_16_16:
      return plain(DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM, 8, kRGBA);

    case F::k_16_EXPAND:
    case F::k_16_FLOAT:
      return plain(DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_UNKNOWN, 2, kRRRR);

    case F::k_16_16_EXPAND:
    case F::k_16_16_FLOAT:
      return plain(DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN, 4, kRGGG);

    case F::k_16_16_16_16_EXPAND:
    case F::k_16_16_16_16_FLOAT:
      return plain(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, 8, kRGBA);

    case F::k_32_FLOAT:
      return plain(DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, 4, kRRRR);

    case F::k_32_32_FLOAT:
      return plain(DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_UNKNOWN, 8, kRGGG);

    case F::k_32_32_32_32_FLOAT:
      return plain(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_UNKNOWN, 16, kRGBA);

    default:
      return {};
    }
  }

  inline DXGI_FORMAT SrgbVariant(DXGI_FORMAT f)
  {
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

    case DXGI_FORMAT_BC1_UNORM:
      return DXGI_FORMAT_BC1_UNORM_SRGB;

    case DXGI_FORMAT_BC2_UNORM:
      return DXGI_FORMAT_BC2_UNORM_SRGB;

    case DXGI_FORMAT_BC3_UNORM:
      return DXGI_FORMAT_BC3_UNORM_SRGB;

    default:
      return f;
    }
  }

  inline const char *FormatName(uint32_t format)
  {
    const FormatInfo *fi = FormatInfo::Get(format);
    return fi && fi->name ? fi->name : "?";
  }

  inline void SwapEndian(uint8_t *p, uint32_t n, uint32_t endian)
  {
    switch (endian)
    {
    case 1: // 8in16
      for (uint32_t i = 0; i + 1 < n; i += 2)
        std::swap(p[i], p[i + 1]);

      break;

    case 2: // 8in32
      for (uint32_t i = 0; i + 3 < n; i += 4)
      {
        uint32_t v;
        std::memcpy(&v, p + i, 4);
        v = __builtin_bswap32(v);
        std::memcpy(p + i, &v, 4);
      }

      break;

    case 3: // 16in32
      for (uint32_t i = 0; i + 3 < n; i += 4)
      {
        uint32_t v;
        std::memcpy(&v, p + i, 4);
        v = (v >> 16) | (v << 16);
        std::memcpy(p + i, &v, 4);
      }

      break;

    default:
      break;
    }
  }

  // Xenos 20e4 float depth (upper 24 bits) -> float32 (Xenia's Float20e4To32)
  inline uint32_t Float20e4To32(uint32_t f24)
  {
    f24 &= 0xFFFFFF;

    if (!f24)
      return 0;

    uint32_t mantissa = f24 & 0xFFFFF;
    uint32_t exponent = f24 >> 20;

    if (!exponent)
    {
      uint32_t lz = 0;

      while (!(mantissa & (0x80000u >> lz)) && lz < 20)
        ++lz;

      lz += 1; // shift so the leading 1 leaves the 20-bit mantissa
      exponent = uint32_t(1 - int32_t(lz));
      mantissa = (mantissa << lz) & 0xFFFFF;
    }

    return ((exponent + 112) << 23) | (mantissa << 3);
  }

  // Converts one swapped guest block into the host block
  inline void Convert(Load load, const uint8_t *in, uint32_t in_bytes, uint8_t *out)
  {
    switch (load)
    {
    case Load::kCopy:
      std::memcpy(out, in, in_bytes);
      break;

    case Load::k565:
    {
      uint16_t v;
      std::memcpy(&v, in, 2);
      const uint32_t r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
      out[0] = uint8_t((r << 3) | (r >> 2));
      out[1] = uint8_t((g << 2) | (g >> 4));
      out[2] = uint8_t((b << 3) | (b >> 2));
      out[3] = 255;
      break;
    }

    case Load::k1555:
    {
      uint16_t v;
      std::memcpy(&v, in, 2);
      const uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
      out[0] = uint8_t((r << 3) | (r >> 2));
      out[1] = uint8_t((g << 3) | (g >> 2));
      out[2] = uint8_t((b << 3) | (b >> 2));
      out[3] = (v >> 15) ? 255 : 0;
      break;
    }

    case Load::k4444:
    {
      uint16_t v;
      std::memcpy(&v, in, 2);

      for (int i = 0; i < 4; ++i)
        out[i] = uint8_t(((v >> (4 * i)) & 15) * 17);

      break;
    }

    case Load::kDepthUnorm:
    {
      uint32_t v;
      std::memcpy(&v, in, 4);
      const float f = float(v >> 8) * (1.0f / 16777215.0f);
      std::memcpy(out, &f, 4);
      break;
    }

    case Load::kDepthFloat:
    {
      uint32_t v;
      std::memcpy(&v, in, 4);
      const uint32_t f = Float20e4To32(v >> 8);
      std::memcpy(out, &f, 4);
      break;
    }

    default:
      break;
    }
  }

  inline uint32_t Log2Floor(uint32_t v)
  {
    uint32_t r = 0;

    while (v >>= 1)
      ++r;

    return r;
  }

  inline D3D12_TEXTURE_ADDRESS_MODE AddressMode(xenos::ClampMode m)
  {
    switch (m)
    {
    case xenos::ClampMode::kRepeat:
      return D3D12_TEXTURE_ADDRESS_MODE_WRAP;

    case xenos::ClampMode::kMirroredRepeat:
      return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;

    case xenos::ClampMode::kClampToEdge:
    case xenos::ClampMode::kClampToHalfway:
      return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
      return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;

    default:
      return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
  }

  inline constexpr char kSwizzleChars[] = "xyzw01??";
}