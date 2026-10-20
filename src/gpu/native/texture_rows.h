// The fast texture load path for formats loaded as a plain copy (most of them: everything but 565 / 1555 / 4444 and depth). TextureCache::Upload used to handle every block on its own (a tiling call into the SDK, a block-sized copy, the endian swap and the conversion switch), ~6 ns a block.
// Fine for 4x4-compressed textures, but an 8-bit video plane is one block per texel: the menus' background movie (1280x720 Y + two 640x360 planes, re-uploaded every frame) cost ~15 ms a frame on the render thread. THis loads a guest row at a time into a scratch row, swaps it in one pass and
// writes it to the upload buffer in one copy...

#pragma once

#include <rex/graphics/pipeline/texture/util.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gpu/native/texture_formats.h"

namespace sst::native::texture_rows
{
  // Whether a texture's rows can take the fast path: a plain copy into a host block of the same size, an endian swap that works on whole rows (the swap unit divides the block: per block, a smaller block isn't swapped at all), and no 3D tiling. Tiled rows also need a tile-aligned pitch (TiledPitchOk), checked per level
  inline bool CanCopyRows(texture_format::Load load, uint32_t host_bpb, uint32_t bpb, uint32_t endian, bool tiled, bool is3d)
  {
    if (load != texture_format::Load::kCopy || host_bpb != bpb || !bpb || bpb > 16)
      return false;

    if (endian > 3 || (endian == 1 && (bpb & 1)) || (endian >= 2 && (bpb & 3)))
      return false;

    return !(tiled && is3d);
  }

  inline bool TiledPitchOk(uint32_t pitch_blocks)
  {
    return pitch_blocks && pitch_blocks % 32 == 0;
  }

  struct RowSource
  {
    const uint8_t *membase;  // guest physical memory
    const uint8_t *snapshot; // this frame's copy of the region (== membase: none)
    uint64_t region_begin;   // the region the snapshot covers
    uint64_t region_end;
    uint64_t memory_size; // guest physical size
  };

  // Loads `cols` blocks of guest row (gy, gz) starting at column gx0 into `scratch`, swapped, then writes host_cols blocks to dst (columns past `cols` are zero). Same result as the per-block loop in TextureCache::Upload
  inline void CopyRow(const RowSource &src, uint64_t slice_addr, bool tiled, uint32_t gx0, uint32_t gy, uint32_t gz, uint32_t cols, uint32_t host_cols, uint32_t bpb, uint32_t bpb_log2, uint32_t pitch_blocks, uint32_t row_pitch_bytes, uint32_t z_slice_stride_block_rows, uint32_t endian, std::vector<uint8_t> &scratch, uint8_t *dst)
  {
    cols = std::min(cols, host_cols);
    const size_t row_bytes = size_t(cols) * bpb;

    if (scratch.size() < row_bytes)
      scratch.resize(row_bytes);

    uint8_t *row = scratch.data();

    auto block_source = [&](uint64_t addr) -> const uint8_t *
    {
      if (addr + bpb > src.memory_size)
        return nullptr;

      return (src.snapshot != src.membase && addr >= src.region_begin && addr + bpb <= src.region_end ? src.snapshot : src.membase) + addr;
    };

    if (!tiled)
    {
      const uint64_t first = slice_addr + (uint64_t(gz) * z_slice_stride_block_rows + gy) * row_pitch_bytes + uint64_t(gx0) * bpb;
      const uint64_t last = first + row_bytes;

      // One copy when the whole row comes from one source (the usual case), else block by block (a row crossing the snapshot's edge)
      const bool one_source = last <= src.memory_size && (src.snapshot == src.membase || (first >= src.region_begin && last <= src.region_end) || last <= src.region_begin || first >= src.region_end);

      if (one_source && cols)
      {
        std::memcpy(row, block_source(first), row_bytes);
      }
      else
      {
        for (uint32_t x = 0; x < cols; ++x)
        {
          const uint8_t *b = block_source(first + uint64_t(x) * bpb);

          if (b)
            std::memcpy(row + size_t(x) * bpb, b, bpb);
          else
            std::memset(row + size_t(x) * bpb, 0, bpb);
        }
      }
    }
    else
    {
      namespace tu = rex::graphics::texture_util;
      int32_t table[128];

      for (int32_t i = 0; i < 128; ++i)
        table[i] = tu::GetTiledOffset2D(i, int32_t(gy), pitch_blocks, bpb_log2);

      const int64_t step = int64_t(tu::GetTiledOffset2D(128, int32_t(gy), pitch_blocks, bpb_log2)) - table[0];

      auto copy = [&](auto size_tag)
      {
        constexpr uint32_t kSize = decltype(size_tag)::value;

        for (uint32_t x = 0; x < cols; ++x)
        {
          const uint32_t gx = gx0 + x;
          const uint64_t addr = uint64_t(int64_t(slice_addr) + table[gx & 127] + int64_t(gx >> 7) * step);
          const uint8_t *b = block_source(addr);

          if (b)
            std::memcpy(row + size_t(x) * kSize, b, kSize);
          else
            std::memset(row + size_t(x) * kSize, 0, kSize);
        }
      };

      switch (bpb)
      {
      case 1:
        copy(std::integral_constant<uint32_t, 1>{});
        break;

      case 2:
        copy(std::integral_constant<uint32_t, 2>{});
        break;

      case 4:
        copy(std::integral_constant<uint32_t, 4>{});
        break;

      case 8:
        copy(std::integral_constant<uint32_t, 8>{});
        break;

      case 16:
        copy(std::integral_constant<uint32_t, 16>{});
        break;

      default:
        for (uint32_t x = 0; x < cols; ++x)
        {
          const uint32_t gx = gx0 + x;
          const uint8_t *b = block_source(uint64_t(int64_t(slice_addr) + table[gx & 127] + int64_t(gx >> 7) * step));

          if (b)
            std::memcpy(row + size_t(x) * bpb, b, bpb);
          else
            std::memset(row + size_t(x) * bpb, 0, bpb);
        }
      }
    }

    texture_format::SwapEndian(row, uint32_t(row_bytes), endian);
    std::memcpy(dst, row, row_bytes);

    if (host_cols > cols)
      std::memset(dst + row_bytes, 0, size_t(host_cols - cols) * bpb);
  }
}