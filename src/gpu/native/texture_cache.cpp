// sst_rematch - native renderer: textures and samplers (see texture_cache.h)

#include "gpu/native/texture_cache.h"

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <xxhash.h>
#include <fmt/format.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>

REXCVAR_DEFINE_BOOL(sst_native_textures, true, "SST",
                    "Native renderer: emulate textures (false = white placeholders)");

REXCVAR_DEFINE_INT32(sst_native_anisotropy, -1, "SST",
                     "Native renderer: anisotropic filtering. -1 = as the game asks, 0 = off, "
                     "2 / 4 / 8 / 16 = force that level on mipmapped linear textures");

REXCVAR_DEFINE_INT32(sst_native_texture_budget_mb, 1536, "SST",
                     "Native renderer: video memory for textures before unused ones are freed");

REXCVAR_DEFINE_BOOL(sst_native_texture_srgb, false, "SST",
                    "Native renderer: sample gamma textures as sRGB (linearised). Off until "
                    "render targets are emulated with gamma, which re-encodes the result");

namespace sst::native
{
  using Microsoft::WRL::ComPtr;
  namespace xenos = rex::graphics::xenos;
  namespace texture_util = rex::graphics::texture_util;
  using rex::graphics::FormatInfo;

  namespace
  {
    constexpr uint32_t kGuestPhysicalSize = 0x20000000u;
    constexpr uint64_t kFrameUploadLimit = 64ull << 20; // new textures per frame (bytes)

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

    HostFormat GetHostFormat(uint32_t format)
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

    DXGI_FORMAT SrgbVariant(DXGI_FORMAT f)
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

    const char *FormatName(uint32_t format)
    {
      const FormatInfo *fi = FormatInfo::Get(format);
      return fi && fi->name ? fi->name : "?";
    }

    void SwapEndian(uint8_t *p, uint32_t n, uint32_t endian)
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
    uint32_t Float20e4To32(uint32_t f24)
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
    void Convert(Load load, const uint8_t *in, uint32_t in_bytes, uint8_t *out)
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

    uint32_t Log2Floor(uint32_t v)
    {
      uint32_t r = 0;

      while (v >>= 1)
        ++r;

      return r;
    }

    D3D12_TEXTURE_ADDRESS_MODE AddressMode(xenos::ClampMode m)
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

    const char kSwizzleChars[] = "xyzw01??";

  }

  bool TextureCache::Initialize(ID3D12Device *device, const uint8_t *physical_membase, ID3D12DescriptorHeap *srv_heap, uint32_t srv_first, uint32_t srv_count, ID3D12DescriptorHeap *sampler_heap, uint32_t sampler_first, uint32_t sampler_count)
  {
    device_ = device;
    membase_ = physical_membase;
    srv_heap_ = srv_heap;
    sampler_heap_ = sampler_heap;
    srv_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    sampler_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    srv_next_ = srv_first;
    srv_end_ = srv_first + srv_count;
    sampler_first_ = sampler_first;
    sampler_next_ = sampler_first;
    sampler_end_ = sampler_first + sampler_count;
    return true;
  }

  void TextureCache::Shutdown()
  {
    textures_.clear();
    deferred_.clear();
    samplers_.clear();
    srv_free_.clear();
  }

  void TextureCache::BeginFrame(uint64_t frame, uint64_t completed_fence, uint64_t frame_fence)
  {
    frame_ = frame;
    frame_fence_ = frame_fence;
    frame_upload_bytes_ = 0;

    for (size_t i = 0; i < deferred_.size();)
    {
      if (deferred_[i].fence <= completed_fence)
      {
        srv_free_.insert(srv_free_.end(), deferred_[i].slots.begin(), deferred_[i].slots.end());
        deferred_[i] = std::move(deferred_.back());
        deferred_.pop_back();
      }
      else
      {
        ++i;
      }
    }

    const uint64_t budget = uint64_t(std::max(REXCVAR_GET(sst_native_texture_budget_mb), 64)) << 20;

    if (total_bytes_ > budget)
      Evict();
  }

  void TextureCache::Evict()
  {
    const uint64_t budget = uint64_t(std::max(REXCVAR_GET(sst_native_texture_budget_mb), 64)) << 20;
    const uint64_t target = budget - budget / 5;
    std::vector<std::pair<uint64_t, uint64_t>> candidates; // (used frame, key hash)

    for (const auto &[hash, t] : textures_)
      if (t->used_frame + 4 < frame_)
        candidates.emplace_back(t->used_frame, hash);

    std::sort(candidates.begin(), candidates.end());

    for (const auto &[used, hash] : candidates)
    {
      if (total_bytes_ <= target)
        break;

      auto it = textures_.find(hash);
      Deferred d;
      d.fence = frame_fence_;
      d.resource = std::move(it->second->resource);

      for (const auto &[view, slot] : it->second->views)
        d.slots.push_back(slot);

      total_bytes_ -= std::min(total_bytes_, it->second->bytes);
      deferred_.push_back(std::move(d));
      textures_.erase(it);
      ++stats_.evicted;
    }
  }

  uint32_t TextureCache::AllocateSrvSlot()
  {
    if (!srv_free_.empty())
    {
      const uint32_t s = srv_free_.back();
      srv_free_.pop_back();
      return s;
    }

    if (srv_next_ < srv_end_)
      return srv_next_++;

    ++stats_.srv_full;
    return kNone;
  }

  TextureCache::Texture *TextureCache::Create(const Key &k, uint64_t hash)
  {
    const HostFormat hf = GetHostFormat(k.format);
    DXGI_FORMAT format = k.signed_mode == 1 ? hf.snorm : hf.unorm;

    if (k.signed_mode == 2)
      format = SrgbVariant(hf.unorm);

    const auto dim = xenos::DataDimension(k.dimension);
    const bool is3d = dim == xenos::DataDimension::k3D;
    auto t = std::make_unique<Texture>();
    t->key = k;
    t->dxgi_format = uint32_t(format);
    t->host_width = hf.block ? (k.width + 3) & ~3u : k.width;
    t->host_height = hf.block ? (k.height + 3) & ~3u : k.height;
    t->levels = k.mip_max + 1;
    t->array_size = is3d ? 1 : k.depth_or_array;
    t->depth = is3d ? k.depth_or_array : 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = is3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = t->host_width;
    desc.Height = t->host_height;
    desc.DepthOrArraySize = UINT16(is3d ? t->depth : t->array_size);
    desc.MipLevels = UINT16(t->levels);
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};

    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t->resource))))
    {
      ++stats_.failed;

      if (LogOnce(0x100000000ull | hash))
        REXLOG_WARN("[sst-native] texture: failed to create {}x{}x{} {} mips {} (DXGI {})", t->host_width, t->host_height, desc.DepthOrArraySize, FormatName(k.format), t->levels, uint32_t(format));

      return nullptr;
    }

    t->bytes = device_->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    total_bytes_ += t->bytes;

    const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(dim, k.pitch, k.width, k.height, k.depth_or_array, k.tiled != 0, xenos::TextureFormat(k.format), k.packed != 0, k.base_page != 0, k.mip_max);
    t->base_size = k.base_page ? layout.base.level_data_extent_bytes : 0;
    t->mip_size = k.mip_page ? layout.mips_total_extent_bytes : 0;

    ++stats_.created;

    if (stats_.logged < 48)
    {
      ++stats_.logged;
      REXLOG_INFO("[sst-native] texture #{}: {}x{}{} {} {} {}, mips {}-{}{}, base {:08X} ({} bytes), "
                  "mips {:08X} ({} bytes), endian {}{} -> DXGI {}",
                  stats_.created, k.width, k.height, dim == xenos::DataDimension::k3D ? fmt::format("x{}", k.depth_or_array) : dim == xenos::DataDimension::kCube ? std::string(" cube")
                                                                                                                           : k.depth_or_array > 1                 ? fmt::format(" [{}]", k.depth_or_array)
                                                                                                                                                                  : std::string(),
                  FormatName(k.format), k.tiled ? "tiled" : "linear", fmt::format("pitch {}", k.pitch << 5), k.mip_min, k.mip_max, k.packed ? " (packed tail)" : "", k.base_page << 12, t->base_size, k.mip_page << 12, t->mip_size, k.endian, k.signed_mode == 1 ? ", signed" : k.signed_mode == 2 ? ", sRGB"
                                                                                                                                                                                                                                                                                                    : "",
                  uint32_t(format));
    }

    Texture *raw = t.get();
    textures_.emplace(hash, std::move(t));
    return raw;
  }

  uint64_t TextureCache::HashGuest(const Texture &t) const
  {
    XXH3_state_t *st = XXH3_createState();
    XXH3_64bits_reset(st);

    auto add = [&](uint32_t page, uint32_t size)
    {
      const uint32_t addr = page << 12;

      if (!page || !size || addr >= kGuestPhysicalSize)
        return;

      size = std::min(size, kGuestPhysicalSize - addr);
      XXH3_64bits_update(st, membase_ + addr, size);
    };

    add(t.key.base_page, t.base_size);
    add(t.key.mip_page, t.mip_size);
    const uint64_t h = XXH3_64bits_digest(st);
    XXH3_freeState(st);
    return h;
  }

  bool TextureCache::Upload(ID3D12GraphicsCommandList *list, const AllocateFn &allocate, Texture &t)
  {
    const Key &k = t.key;
    const HostFormat hf = GetHostFormat(k.format);
    const FormatInfo *fi = FormatInfo::Get(k.format);

    if (!fi || hf.load == Load::kNone)
      return false;

    const uint32_t bw = fi->block_width, bh = fi->block_height, bpb = fi->bytes_per_block();

    if (!bpb || bpb > 16)
      return false;

    const uint32_t bpb_log2 = Log2Floor(bpb);
    const auto dim = xenos::DataDimension(k.dimension);
    const bool is3d = dim == xenos::DataDimension::k3D;
    const auto format = xenos::TextureFormat(k.format);
    const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(dim, k.pitch, k.width, k.height, k.depth_or_array, k.tiled != 0, format, k.packed != 0, k.base_page != 0, k.mip_max);

    const D3D12_RESOURCE_DESC desc = t.resource->GetDesc();
    const UINT subresources = t.levels * t.array_size;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subresources);
    std::vector<UINT> rows(subresources);
    std::vector<UINT64> row_bytes(subresources);
    UINT64 total = 0;
    device_->GetCopyableFootprints(&desc, 0, subresources, 0, fp.data(), rows.data(), row_bytes.data(), &total);
    const UploadSpace up = allocate(size_t(total));

    if (!up.cpu)
      return false;

    const auto start = std::chrono::steady_clock::now();
    std::vector<UINT> loaded;
    uint8_t block[16];

    for (uint32_t level = k.mip_min; level <= k.mip_max; ++level)
    {
      const bool is_base = level == 0;

      if (is_base ? !k.base_page : !k.mip_page)
        continue;

      const uint32_t stored = std::min(level, layout.packed_level);
      const texture_util::TextureGuestLayout::Level &lay = is_base ? layout.base : layout.mips[stored];
      const uint32_t level_addr = is_base ? (k.base_page << 12) : (k.mip_page << 12) + layout.mip_offsets_bytes[stored];
      uint32_t ox = 0, oy = 0, oz = 0;

      if (level >= layout.packed_level)
        texture_util::GetPackedMipOffset(k.width, k.height, t.depth, format, level, ox, oy, oz);

      const uint32_t gw = std::max(k.width >> level, 1u), gh = std::max(k.height >> level, 1u);
      const uint32_t gd = std::max(t.depth >> level, 1u);
      const uint32_t gbx = (gw + bw - 1) / bw, gby = (gh + bh - 1) / bh;
      const uint32_t pitch_blocks = lay.row_pitch_bytes / bpb;

      for (uint32_t slice = 0; slice < t.array_size; ++slice)
      {
        const UINT sub = level + slice * t.levels;
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &f = fp[sub];
        const uint32_t host_cols = uint32_t(row_bytes[sub] / hf.host_bpb);
        const uint32_t slice_addr = level_addr + slice * lay.array_slice_stride_bytes;

        for (uint32_t z = 0; z < f.Footprint.Depth; ++z)
        {
          for (uint32_t y = 0; y < rows[sub]; ++y)
          {
            uint8_t *dst = up.cpu + f.Offset + (size_t(z) * rows[sub] + y) * f.Footprint.RowPitch;

            for (uint32_t x = 0; x < host_cols; ++x, dst += hf.host_bpb)
            {
              if (x >= gbx || y >= gby || z >= gd)
              {
                std::memset(dst, 0, hf.host_bpb);
                continue;
              }

              const int32_t gx = int32_t(ox + x), gy = int32_t(oy + y), gz = int32_t(oz + z);
              uint64_t offset;

              if (k.tiled)
              {
                offset = uint64_t(int64_t(is3d ? texture_util::GetTiledOffset3D(gx, gy, gz, pitch_blocks, lay.z_slice_stride_block_rows, bpb_log2) : texture_util::GetTiledOffset2D(gx, gy, pitch_blocks, bpb_log2)));
              }
              else
              {
                offset = (uint64_t(gz) * lay.z_slice_stride_block_rows + uint64_t(gy)) * lay.row_pitch_bytes + uint64_t(gx) * bpb;
              }

              const uint64_t addr = uint64_t(slice_addr) + offset;

              if (addr + bpb > kGuestPhysicalSize)
              {
                std::memset(dst, 0, hf.host_bpb);
                continue;
              }

              std::memcpy(block, membase_ + addr, bpb);
              SwapEndian(block, bpb, k.endian);
              Convert(hf.load, block, bpb, dst);
            }
          }
        }

        loaded.push_back(sub);
      }
    }

    stats_.upload_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    if (loaded.empty())
      return false;

    const D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (t.resident)
    {
      b.Transition.StateBefore = kShaderRead;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      list->ResourceBarrier(1, &b);
    }

    for (UINT sub : loaded)
    {
      D3D12_TEXTURE_COPY_LOCATION dst = {};
      dst.pResource = t.resource.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = sub;
      D3D12_TEXTURE_COPY_LOCATION src = {};
      src.pResource = up.resource;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = fp[sub];
      src.PlacedFootprint.Offset += up.offset;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = kShaderRead;
    list->ResourceBarrier(1, &b);
    ++stats_.uploads;

    if (t.resident)
      ++stats_.reuploads;

    stats_.upload_bytes += total;
    frame_upload_bytes_ += total;
    return true;
  }

  uint32_t TextureCache::GetTexture(ID3D12GraphicsCommandList *list, const AllocateFn &allocate, const uint32_t fetch[6], uint32_t fetch_dimension)
  {
    if (!REXCVAR_GET(sst_native_textures))
      return kNone;

    ++stats_.lookups;
    xenos::xe_gpu_texture_fetch_t tf;
    std::memcpy(&tf, fetch, sizeof(tf));

    if ((fetch[0] & 3) != 2)
    {
      ++stats_.bad;
      return kNone;
    }

    const uint32_t format = fetch[1] & 0x3F;
    const HostFormat hf = GetHostFormat(format);

    if (hf.load == Load::kNone)
    {
      ++stats_.unsupported;

      if (LogOnce(0x200000000ull | format))
        REXLOG_WARN("[sst-native] texture: format {} ({}) not supported yet (placeholder)", format, FormatName(format));

      return kNone;
    }

    uint32_t wm1 = 0, hm1 = 0, dm1 = 0, base_page = 0, mip_page = 0, mip_min = 0, mip_max = 0;
    texture_util::GetSubresourcesFromFetchConstant(tf, &wm1, &hm1, &dm1, &base_page, &mip_page, &mip_min, &mip_max);

    if (!base_page && !mip_page)
    {
      ++stats_.bad;
      return kNone;
    }

    // Drawn by the renderer (render-target resolve destination)?
    if (!externals_.empty())
    {
      auto it = externals_.find((fetch[1] >> 12) & 0x1FFFF);

      if (it != externals_.end())
        return GetExternalView(it->second, format, tf.swizzle, fetch_dimension, wm1 + 1, hm1 + 1);
    }

    const auto dim = tf.dimension;
    // Shader dimension (FetchOpDimension: 0 1D, 1 2D, 2 3D/stacked, 3 cube) vs the texture
    uint32_t view_kind = 0; // 0 2D, 1 3D, 2 cube
    bool compatible = false;

    switch (fetch_dimension)
    {
    case 1:
      compatible = dim == xenos::DataDimension::k2DOrStacked && !tf.stacked;
      view_kind = 0;
      break;

    case 2:
      compatible = dim == xenos::DataDimension::k3D;
      view_kind = 1;
      break;

    case 3:
      compatible = dim == xenos::DataDimension::kCube;
      view_kind = 2;
      break;

    default:
      break;
    }

    if (!compatible)
    {
      ++stats_.dimension_mismatch;

      if (LogOnce(0x300000000ull | (uint64_t(fetch_dimension) << 8) | uint64_t(dim) << 4 | uint64_t(tf.stacked)))
        REXLOG_WARN("[sst-native] texture: shader samples dimension {} but the texture is {}{} "
                    "(placeholder)",
                    fetch_dimension, uint32_t(dim), tf.stacked ? " stacked" : "");

      return kNone;
    }

    // Signedness of the data components the swizzle reads
    const uint32_t swizzle = tf.swizzle;
    const uint32_t signs[4] = {uint32_t(tf.sign_x), uint32_t(tf.sign_y), uint32_t(tf.sign_z), uint32_t(tf.sign_w)};
    uint32_t used_signs = 0; // bit per TextureSign value

    for (uint32_t i = 0; i < 4; ++i)
    {
      const uint32_t v = (swizzle >> (3 * i)) & 7;

      if (v < 4)
        used_signs |= 1u << signs[v];
    }

    uint32_t signed_mode = 0;

    if (used_signs == (1u << 1) && hf.snorm != DXGI_FORMAT_UNKNOWN)
      signed_mode = 1;

    if ((used_signs & (1u << 3)) && REXCVAR_GET(sst_native_texture_srgb) && SrgbVariant(hf.unorm) != hf.unorm)
      signed_mode = 2;

    if (used_signs & ~1u)
    {
      if (LogOnce(0x400000000ull | (uint64_t(format) << 8) | used_signs))
        REXLOG_INFO("[sst-native] texture: {} with signs {}{}{}{} (swizzle {}{}{}{}): {}", FormatName(format), signs[0], signs[1], signs[2], signs[3], kSwizzleChars[swizzle & 7], kSwizzleChars[(swizzle >> 3) & 7], kSwizzleChars[(swizzle >> 6) & 7], kSwizzleChars[(swizzle >> 9) & 7], signed_mode == 1 ? "signed format" : signed_mode == 2 ? "sRGB format"
                                                                                                                                                                                                                                                                                                                                                  : "sampled as unsigned (not emulated yet)");
    }

    if (tf.exp_adjust != 0 && LogOnce(0x500000000ull | uint32_t(tf.exp_adjust & 63)))
      REXLOG_INFO("[sst-native] texture: exp_adjust {} not emulated yet", int(tf.exp_adjust));

    Key k;
    std::memset(&k, 0, sizeof(k));
    k.base_page = base_page;
    k.mip_page = mip_page;
    k.width = wm1 + 1;
    k.height = hm1 + 1;
    k.depth_or_array = dm1 + 1;
    k.format = format;
    k.endian = uint32_t(tf.endianness);
    k.dimension = uint32_t(dim);
    k.tiled = tf.tiled;
    k.packed = tf.packed_mips;
    k.pitch = tf.pitch;
    k.mip_min = mip_min;
    k.mip_max = mip_max;
    k.signed_mode = signed_mode;
    const uint64_t hash = XXH3_64bits(&k, sizeof(k));

    Texture *t;

    if (auto it = textures_.find(hash); it != textures_.end())
    {
      t = it->second.get();
    }
    else
    {
      t = Create(k, hash);

      if (!t)
        return kNone;
    }

    t->used_frame = frame_;

    // Spread big loads (match start) over frames so the upload ring stays small
    if (!t->resident && frame_upload_bytes_ > kFrameUploadLimit)
    {
      ++stats_.deferred_loads;
      return kNone;
    }

    // Textures whose data hasn't changed for a while are re-checked every 8th frame only (staggered), which keeps hashing cheap in matches
    const bool throttled = t->resident && t->stable_checks >= 60 && ((frame_ + (hash >> 61)) & 7) != 0;

    if (t->checked_frame != frame_ && throttled)
    {
      t->checked_frame = frame_;
      ++stats_.checks_skipped;
    }

    if (t->checked_frame != frame_)
    {
      t->checked_frame = frame_;
      const auto start = std::chrono::steady_clock::now();
      const uint64_t h = HashGuest(*t);
      stats_.hashed_bytes += uint64_t(t->base_size) + t->mip_size;
      stats_.hash_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

      if (t->resident && h == t->content_hash)
      {
        ++t->stable_checks;
      }
      else
      {
        t->stable_checks = 0;

        if (Upload(list, allocate, *t))
        {
          t->resident = true;
          t->content_hash = h;
        }
        else
        {
          ++stats_.failed;
        }
      }
    }

    if (!t->resident)
      return kNone;

    const uint32_t view_key = swizzle | (view_kind << 12);

    if (auto it = t->views.find(view_key); it != t->views.end())
      return it->second;

    const uint32_t slot = AllocateSrvSlot();

    if (slot == kNone)
      return kNone;

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT(t->dxgi_format);
    uint32_t mapping = 0;

    for (uint32_t i = 0; i < 4; ++i)
    {
      const uint32_t v = (swizzle >> (3 * i)) & 7;
      const uint32_t c = v < 4 ? (hf.swizzle >> (3 * v)) & 7 : v == 5 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1
                                                                      : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
      mapping |= c << (3 * i);
    }

    sd.Shader4ComponentMapping = mapping | (1u << 12);
    const UINT most_detailed = k.mip_min, mip_count = k.mip_max - k.mip_min + 1;

    switch (view_kind)
    {
    case 0:
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MostDetailedMip = most_detailed;
      sd.Texture2D.MipLevels = mip_count;
      break;

    case 1:
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      sd.Texture3D.MostDetailedMip = most_detailed;
      sd.Texture3D.MipLevels = mip_count;
      break;

    default:
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      sd.TextureCube.MostDetailedMip = most_detailed;
      sd.TextureCube.MipLevels = mip_count;
      break;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(slot) * srv_size_;
    device_->CreateShaderResourceView(t->resource.Get(), &sd, h);
    t->views.emplace(view_key, slot);
    return slot;
  }

  void TextureCache::SetExternal(uint32_t base_page, const External &e)
  {
    RemoveExternal(base_page);
    externals_[base_page].info = e;
  }

  void TextureCache::RemoveExternal(uint32_t base_page)
  {
    auto it = externals_.find(base_page);

    if (it == externals_.end())
      return;

    Deferred d;
    d.fence = frame_fence_;

    for (const auto &[view, slot] : it->second.views)
      d.slots.push_back(slot);

    deferred_.push_back(std::move(d));
    externals_.erase(it);
  }

  uint32_t TextureCache::GetExternalView(ExternalEntry &e, uint32_t guest_format, uint32_t swizzle, uint32_t fetch_dimension, uint32_t width, uint32_t height)
  {
    // 2D fetches read 2D textures; 3D fetches read volumes (stacked textures the renderer keeps as Texture3D)
    const bool want_volume = fetch_dimension == 2;

    if (fetch_dimension != 1 && fetch_dimension != 2)
      return kNone;

    if (want_volume != e.info.volume || width > e.info.width || height > e.info.height)
    {
      ++stats_.external_mismatch;

      if (LogOnce(0x600000000ull ^ (uint64_t(width) << 16) ^ height ^ (uint64_t(fetch_dimension) << 30)))
        REXLOG_WARN("[sst-native] texture: resolved texture sampled as {} {}x{} but it is {} {}x{}", want_volume ? "3D" : "2D", width, height, e.info.volume ? "3D" : "2D", e.info.width, e.info.height);

      return kNone;
    }

    ++stats_.external_hits;
    const uint32_t view_key = swizzle | (uint32_t(want_volume) << 12);

    if (auto it = e.views.find(view_key); it != e.views.end())
      return it->second;

    const uint32_t slot = AllocateSrvSlot();

    if (slot == kNone)
      return kNone;

    const HostFormat hf = GetHostFormat(guest_format);
    const uint16_t data_swizzle = hf.load != Load::kNone ? hf.swizzle : kRGBA;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT(e.info.dxgi_format);
    uint32_t mapping = 0;

    for (uint32_t i = 0; i < 4; ++i)
    {
      const uint32_t v = (swizzle >> (3 * i)) & 7;
      const uint32_t c = v < 4 ? (data_swizzle >> (3 * v)) & 7 : v == 5 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1
                                                                        : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
      mapping |= c << (3 * i);
    }

    sd.Shader4ComponentMapping = mapping | (1u << 12);

    if (want_volume)
    {
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      sd.Texture3D.MipLevels = 1;
    }
    else
    {
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MipLevels = 1;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(slot) * srv_size_;
    device_->CreateShaderResourceView(e.info.resource, &sd, h);
    e.views.emplace(view_key, slot);
    return slot;
  }

  uint32_t TextureCache::GetSampler(const uint32_t fetch[6], const TextureFetchUse &use)
  {
    xenos::xe_gpu_texture_fetch_t tf;
    std::memcpy(&tf, fetch, sizeof(tf));
    auto pick = [](uint8_t over, uint32_t base)
    { return over != 3 ? uint32_t(over) : base; };
    const uint32_t mag = pick(use.mag_filter, uint32_t(tf.mag_filter)) & 3;
    const uint32_t min = pick(use.min_filter, uint32_t(tf.min_filter)) & 3;
    const uint32_t mip = pick(use.mip_filter, uint32_t(tf.mip_filter)) & 3;
    uint32_t aniso = use.aniso_filter != 7 ? use.aniso_filter : uint32_t(tf.aniso_filter);

    if (aniso == 7)
      aniso = 0;

    D3D12_SAMPLER_DESC d = {};
    const int32_t force = REXCVAR_GET(sst_native_anisotropy);
    uint32_t max_aniso = aniso >= 2 && aniso <= 5 ? 1u << (aniso - 1) : 1;

    if (force == 0)
      max_aniso = 1;

    if (force > 1 && min == 1 && mip != 2)
      max_aniso = uint32_t(std::min(force, 16));

    if (max_aniso > 1)
    {
      d.Filter = D3D12_FILTER_ANISOTROPIC;
      d.MaxAnisotropy = max_aniso;
    }
    else
    {
      // D3D12 basic filter: min << 4 | mag << 2 | mip (0 point, 1 linear)
      const uint32_t mip_linear = mip == 1 ? 1 : 0;
      d.Filter = D3D12_FILTER(((min == 1 ? 1 : 0) << 4) | ((mag == 1 ? 1 : 0) << 2) | mip_linear);
      d.MaxAnisotropy = 1;
    }

    d.AddressU = AddressMode(tf.clamp_x);
    d.AddressV = AddressMode(tf.clamp_y);
    d.AddressW = AddressMode(tf.clamp_z);
    const float bias = float(tf.lod_bias) * (1.0f / 32.0f) + use.lod_bias;
    d.MipLODBias = std::clamp(bias, -16.0f, 15.99f);
    d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;

    if (tf.border_color == xenos::BorderColor::k_ABGR_White)
      d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = 1.0f;

    d.MinLOD = 0;
    d.MaxLOD = mip == 2 ? 0.0f : D3D12_FLOAT32_MAX; // kBaseMap: the base level only

    const uint64_t hash = XXH3_64bits(&d, sizeof(d));

    if (auto it = samplers_.find(hash); it != samplers_.end())
      return it->second;

    if (sampler_next_ >= sampler_end_)
    {
      ++stats_.sampler_full;
      return sampler_first_ ? sampler_first_ - 1 : 0;
    }

    const uint32_t slot = sampler_next_++;
    D3D12_CPU_DESCRIPTOR_HANDLE h = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(slot) * sampler_size_;
    device_->CreateSampler(&d, h);
    samplers_.emplace(hash, slot);
    return slot;
  }

  std::string TextureCache::StatusLine() const
  {
    return fmt::format("textures: {} live ({} MB), {} lookups, created {}, uploads {} (re-uploads {}) {:.1f} MB "
                       "in {:.1f} ms, hashed {:.1f} MB in {:.1f} ms, unsupported {}, bad {}, dimension "
                       "mismatches {}, failed {}, evicted {}, loads deferred {}, checks skipped (stable) {}, resolved-texture reads {} (size mismatches {}), samplers {}{}{}",
                       textures_.size(), total_bytes_ >> 20, stats_.lookups, stats_.created, stats_.uploads, stats_.reuploads, double(stats_.upload_bytes) / (1 << 20), stats_.upload_ms, double(stats_.hashed_bytes) / (1 << 20), stats_.hash_ms, stats_.unsupported, stats_.bad, stats_.dimension_mismatch, stats_.failed, stats_.evicted, stats_.deferred_loads, stats_.checks_skipped, stats_.external_hits, stats_.external_mismatch, samplers_.size(), stats_.srv_full ? fmt::format(", SRV heap full x{}", stats_.srv_full) : std::string(), stats_.sampler_full ? fmt::format(", sampler heap full x{}", stats_.sampler_full) : std::string());
  }
}