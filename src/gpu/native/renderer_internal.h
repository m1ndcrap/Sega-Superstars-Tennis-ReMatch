// Native renderer: what the renderer_*.cpp files share (includes, options, guest-memory and state helpers, the pipeline key). Private to the renderer.

#pragma once

#include "gpu/native/renderer.h"
#include "gpu/native/guest_memory.h"
#include "gpu/d3d12_setup.h"
#include "gpu/native/shader_cache.h"
#include "gpu/native/texture_cache.h"
#include "gpu/native/vertex_format.h"
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <xxhash.h>
#include <fmt/format.h>
#include <algorithm>
#include <bit>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <cfloat>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <cmath>
#include <cstring>

// Renderer options (defined in renderer.cpp)
REXCVAR_DECLARE(bool, sst_native_quads);
REXCVAR_DECLARE(bool, sst_native_debug_shading);
REXCVAR_DECLARE(bool, sst_native_half_pixel);
REXCVAR_DECLARE(bool, sst_native_memexport);
REXCVAR_DECLARE(bool, sst_native_present_queue);
REXCVAR_DECLARE(int32_t, sst_native_present_wait_ms);
REXCVAR_DECLARE(int32_t, sst_native_geometry_cache_mb);
REXCVAR_DECLARE(bool, sst_native_pipeline_async);
REXCVAR_DECLARE(bool, sst_native_pipeline_library);
REXCVAR_DECLARE(int32_t, sst_native_rt_trace);
REXCVAR_DECLARE(bool, sst_native_render_targets);
REXCVAR_DECLARE(bool, sst_native_memexport_async);
REXCVAR_DECLARE(bool, sst_native_diagnostics);
REXCVAR_DECLARE(bool, sst_native_memexport_cache);
REXCVAR_DECLARE(std::string, sst_native_memexport_dump);
REXCVAR_DECLARE(int32_t, sst_native_memexport_threads);
REXCVAR_DECLARE(bool, sst_native_check_vs);
REXCVAR_DECLARE(bool, sst_native_check_vs_all);
REXCVAR_DECLARE(bool, sst_native_depth_flip_vs);
REXCVAR_DECLARE(std::string, sst_native_trace_vs);
REXCVAR_DECLARE(std::string, sst_native_skip_vs);
REXCVAR_DECLARE(bool, sst_native_debug_markers);
REXCVAR_DECLARE(bool, sst_native_debug_bisect);
REXCVAR_DECLARE(bool, sst_native_skip_blended);
REXCVAR_DECLARE(bool, sst_native_check_late_writes);
REXCVAR_DECLARE(int32_t, sst_native_capture_frame);
REXCVAR_DECLARE(bool, sst_native_ui_fit);
REXCVAR_DECLARE(bool, sst_native_wide_targets);
REXCVAR_DECLARE(bool, sst_native_scissor);
REXCVAR_DECLARE(std::string, sst_native_rt);

// Video settings (menu/graphics_menu.cpp)
REXCVAR_DECLARE(int32_t, sst_resolution);
REXCVAR_DECLARE(int32_t, sst_render_scale);
REXCVAR_DECLARE(int32_t, sst_shadow_size);
REXCVAR_DECLARE(int32_t, sst_brightness);
REXCVAR_DECLARE(int32_t, sst_fps_limit);
REXCVAR_DECLARE(int32_t, sst_aa_type);
REXCVAR_DECLARE(int32_t, sst_aa_level);
REXCVAR_DECLARE(bool, sst_bloom);
REXCVAR_DECLARE(bool, sst_fps_counter);
REXCVAR_DECLARE(int32_t, sst_aspect);

namespace sst::native
{
  namespace renderer_detail
  {
    constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    // The presenter's guest output format (the SDK's D3D12 / Vulkan presenters: 10:10:10:2 UNORM)
    constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

    // A debug hotkey (F9 capture, F10 render-target trace, bisect keys) was pressed since the last check. Windows virtual-key codes
    inline bool DebugKeyPressed(int vk)
    {
#if defined(_WIN32)
      return (GetAsyncKeyState(vk) & 1) != 0;
#else
      (void)vk;
      return false;
#endif
    }

    // Read in shaders (pixel and other stages)
    inline const D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    constexpr size_t kUploadPageSize = 32u << 20;
    constexpr uint32_t kMaxDrawVertices = 1u << 20;
    constexpr uint32_t kMaxDrawIndices = 1u << 21;

    // Placeholder descriptor indices (heap slots)
    constexpr uint32_t kPlaceholder2D = 0, kPlaceholder3D = 1, kPlaceholderCube = 2;

    // Black 3D: stands in for the stacked shadow map (sampled as 3D) until render targets are emulated - depth 0 reads as "lit" in the game's shadow test
    constexpr uint32_t kPlaceholder3DBlack = 3;

    // Heap slots below these are placeholders / default samplers; the rest belong to the texture cache
    constexpr uint32_t kSrvHeapSize = 65536, kSamplerHeapSize = 2048;
    constexpr uint32_t kSrvReserved = 512, kSamplerReserved = 16;

    // F9 capture: stream-output buffer (data, then one 16-byte counter per draw)
    constexpr size_t kSoDataBytes = 48u << 20; // 3M vertices
    constexpr uint32_t kSoMaxDraws = 8192;
    constexpr size_t kSoCounterBytes = size_t(kSoMaxDraws) * 16;

    // Slot for VS inputs the declaration doesn't provide (reads zeros)
    constexpr UINT kZeroInputSlot = 31;

    // On the render thread, the guest memory a command needs as it was when the game issued it (device state, patched microcode, vertex data from the caller) is read from snapshots taken by the hook (see native.cpp)
    inline thread_local const GuestOverlay *t_overlays = nullptr;
    inline thread_local uint32_t t_overlay_count = 0;
    inline const uint8_t *GuestPtr(const uint8_t *base, uint32_t addr)
    {
      for (uint32_t i = 0; i < t_overlay_count; ++i)
      {
        const GuestOverlay &o = t_overlays[i];

        if (addr - o.begin < o.end - o.begin)
          return o.data + (addr - o.begin);
      }

      return base + addr + rex::memory::detail::PhysicalHostOffset(addr);
    }

    inline uint32_t be32(const uint8_t *base, uint32_t addr)
    {
      uint32_t v;
      std::memcpy(&v, GuestPtr(base, addr), 4);
      return __builtin_bswap32(v);
    }

    inline uint32_t be32raw(const uint8_t *p)
    {
      uint32_t v;
      std::memcpy(&v, p, 4);
      return __builtin_bswap32(v);
    }

    // D3D object addresses are virtual; tolerate physical ones (map through the 0xA0000000 physical view)
    inline uint32_t ToVirtual(uint32_t addr)
    {
      return addr < 0x20000000u ? (addr | 0xA0000000u) : addr;
    }

    inline bool PlausibleAddress(uint32_t addr)
    {
      return addr >= 0x40000000u;
    }

    // Guest pixels -> host pixels at an internal resolution scale (rounded, so adjacent rectangles stay adjacent)
    inline LONG Px(LONG v, float s)
    {
      return LONG(std::lround(double(v) * s));
    }

    inline uint32_t PxU(uint32_t v, float s)
    {
      return std::max<uint32_t>(1, uint32_t(Px(LONG(v), s)));
    }

    inline D3D12_RECT Scaled(const D3D12_RECT &r, float s)
    {
      return {Px(r.left, s), Px(r.top, s), Px(r.right, s), Px(r.bottom, s)};
    }

    inline D3D12_RECT Scaled(const D3D12_RECT &r, Scale2 s)
    {
      return {Px(r.left, s.x), Px(r.top, s.y), Px(r.right, s.x), Px(r.bottom, s.y)};
    }

    // Vertex declaration bound at dev+0x2E2C (CreateVertexDeclaration sub_82300CA8 / its init sub_82300BB8): +0x18 element count, elements from +0x34, 12 bytes each {u16 stream, u16 offset, u32 D3DDECLTYPE (bits 0-5: the vertex format), u8 method, u8 usage, u8 usage index}. Vertex declaration bound at dev+0x2E2C (CreateVertexDeclaration sub_82300CA8, filled by sub_82300BB8): +0x18 element count, elements from +0x34, 12 bytes each {u16 stream, u16 offset, u32 D3DDECLTYPE (bits 0-5: the vertex format), u8 method, u8 usage, u8 usage index}; the list ends at stream 0xFF
    struct DeclInput
    {
      uint32_t stream = 0, offset = 0, format = 0, components = 0, stride = 0;
    };

    inline bool FindDeclInput(const uint8_t *base, uint32_t dev, uint32_t usage, uint32_t usage_index, DeclInput &out)
    {
      const uint32_t decl = be32(base, dev + 0x2E2C);

      if (!PlausibleAddress(decl))
        return false;

      const uint32_t count = be32(base, decl + 0x18);

      if (count == 0 || count > 32)
        return false;

      bool found = false;

      struct E
      {
        uint32_t stream, offset, format;
      };

      E all[32];

      for (uint32_t i = 0; i < count; ++i)
      {
        const uint32_t e = decl + 0x34 + 12 * i;
        const uint32_t w0 = be32(base, e), type = be32(base, e + 4), w2 = be32(base, e + 8);
        all[i] = {w0 >> 16, w0 & 0xFFFF, type & 0x3F};

        if (all[i].stream == 0xFF)
          return false;

        const uint32_t u = (w2 >> 16) & 0xFF, ui = (w2 >> 8) & 0xFF;

        if (!found && u == usage && ui == usage_index && DeclFormatSize(all[i].format))
        {
          out.stream = all[i].stream;
          out.offset = all[i].offset;
          out.format = all[i].format;
          out.components = uint32_t(rex::graphics::xenos::GetVertexFormatComponentCount(rex::graphics::xenos::VertexFormat(all[i].format)));
          found = true;
        }
      }

      if (!found)
        return false;
      // The vertex size: the end of the stream's furthest element
      out.stride = 0;

      for (uint32_t i = 0; i < count; ++i)
      {
        if (all[i].stream == out.stream)
          out.stride = std::max(out.stride, all[i].offset + DeclFormatSize(all[i].format));
      }

      return out.stride > 0 && out.stride < 256;
    }

    // Device register mirror (see native-renderer-d3d-layer.md): packed blocks
    inline uint32_t Reg(const uint8_t *base, uint32_t dev, uint32_t reg)
    {
      if (reg >= 0x2100 && reg <= 0x2114)
        return be32(base, dev + 0x28CC + 4 * (reg - 0x2100));

      if (reg >= 0x2200 && reg <= 0x220B)
        return be32(base, dev + 0x2934 + 4 * (reg - 0x2200));

      return 0;
    }

    inline float RegF(const uint8_t *base, uint32_t dev, uint32_t reg)
    {
      const uint32_t v = Reg(base, dev, reg);
      float f;
      std::memcpy(&f, &v, 4);
      return f;
    }

    enum : uint32_t
    {
      RB_COLOR_MASK = 0x2104,
      RB_BLEND_RED = 0x2105,
      RB_ALPHA_REF = 0x210E,
      PA_CL_VPORT_XSCALE = 0x210F,
      PA_CL_VPORT_XOFFSET = 0x2110,
      PA_CL_VPORT_YSCALE = 0x2111,
      PA_CL_VPORT_YOFFSET = 0x2112,
      PA_CL_VPORT_ZSCALE = 0x2113,
      PA_CL_VPORT_ZOFFSET = 0x2114,
      RB_DEPTHCONTROL = 0x2200,
      RB_BLENDCONTROL0 = 0x2201,
      RB_COLORCONTROL = 0x2202,
      PA_SU_SC_MODE_CNTL = 0x2205,
      PA_CL_VTE_CNTL = 0x2206,
    };

    // Device fields
    constexpr uint32_t kDevVSConstants = 0x780, kDevPSConstants = 0x1780, kDevBools = 0x2780;
    constexpr uint32_t kDevIndices = 0x3094, kDevRT0 = 0x3098, kDevDepth = 0x30A8;
    constexpr uint32_t kDevPS = 0x3194, kDevVS = 0x3198;
    constexpr uint32_t kDevScissorTL = 0x28C4, kDevScissorBR = 0x28C8;       // PA_SC_WINDOW_SCISSOR_*
    constexpr uint32_t kDevScissorEnable = 0x2E50, kDevScissorRect = 0x3184; // the rectangle as set (left, top, right, bottom; before the viewport intersection)

    // Fetch constants: slot j (2 dwords) at +0x480 + 8j. Textures use 3 slots each (sampler i = slots 3i..3i+2); vertex stream s is slot 95 - s
    constexpr uint32_t kDevFetchConstants = 0x480;

    inline D3D12_BLEND BlendFactor(uint32_t f, bool alpha)
    {
      switch (f)
      {
      case 0:
        return D3D12_BLEND_ZERO;

      case 1:
        return D3D12_BLEND_ONE;

      case 4:
        return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;

      case 5:
        return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;

      case 6:
        return D3D12_BLEND_SRC_ALPHA;

      case 7:
        return D3D12_BLEND_INV_SRC_ALPHA;

      case 8:
        return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;

      case 9:
        return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;

      case 10:
        return D3D12_BLEND_DEST_ALPHA;

      case 11:
        return D3D12_BLEND_INV_DEST_ALPHA;

      case 12:
      case 14:
        return D3D12_BLEND_BLEND_FACTOR;

      case 13:
      case 15:
        return D3D12_BLEND_INV_BLEND_FACTOR;

      case 16:
        return D3D12_BLEND_SRC_ALPHA_SAT;

      default:
        return D3D12_BLEND_ONE;
      }
    }

    inline D3D12_BLEND_OP BlendOp(uint32_t op)
    {
      switch (op)
      {
      case 1:
        return D3D12_BLEND_OP_SUBTRACT;

      case 2:
        return D3D12_BLEND_OP_MIN;

      case 3:
        return D3D12_BLEND_OP_MAX;

      case 4:
        return D3D12_BLEND_OP_REV_SUBTRACT;

      default:
        return D3D12_BLEND_OP_ADD;
      }
    }

    // --sst_native_rt: which render targets draw into the single output until render targets are emulated. "auto" = targets whose viewport covers the 1280x720 frame (main scene + UI; skips shadow maps and bloom), "all", or a size as logged in 'render target' lines (e.g. 640x736).
    inline bool RenderTargetSelected(const uint8_t *base, uint32_t dev, uint32_t size_word)
    {
      const std::string filter = REXCVAR_GET(sst_native_rt);

      if (filter == "all" || filter.empty())
        return true;

      if (filter == "auto")
      {
        const float xs = std::fabs(RegF(base, dev, PA_CL_VPORT_XSCALE));
        const float ys = std::fabs(RegF(base, dev, PA_CL_VPORT_YSCALE));
        return xs == 640.0f && ys == 360.0f;
      }

      const std::string size = std::to_string((size_word >> 18) + 1) + "x" + std::to_string(((size_word >> 3) & 0x7FFF) + 1);
      return filter == size;
    }
  }

  // render targets: host textures, resolve copies, presenting
  namespace renderer_detail
  {
    constexpr uint32_t kRtRtvCount = 256, kRtDsvCount = 64;
    constexpr uint32_t kRtSrvFirst = 16; // renderer-owned SRV slots: [16, kSrvReserved)

    // Host format of a guest colour render-target format (RB_COLOR_INFO bits 16-19)
    inline DXGI_FORMAT ColorTargetFormat(uint32_t format)
    {
      switch (format)
      {
      case 0: // k_8_8_8_8

      case 1: // k_8_8_8_8_GAMMA
        return DXGI_FORMAT_R8G8B8A8_UNORM;

      case 2: // k_2_10_10_10

      case 10: // k_2_10_10_10_AS_10_10_10_10
        return DXGI_FORMAT_R10G10B10A2_UNORM;

      case 3: // k_2_10_10_10_FLOAT

      case 12: // k_2_10_10_10_FLOAT_AS_16_16_16_16

      case 5: // k_16_16_16_16 (-32..32 fixed point)

      case 7: // k_16_16_16_16_FLOAT
        return DXGI_FORMAT_R16G16B16A16_FLOAT;

      case 4: // k_16_16

      case 6: // k_16_16_FLOAT
        return DXGI_FORMAT_R16G16_FLOAT;

      case 14: // k_32_FLOAT
        return DXGI_FORMAT_R32_FLOAT;

      case 15: // k_32_32_FLOAT
        return DXGI_FORMAT_R32G32_FLOAT;

      default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
      }
    }

    // Host format of a resolve destination (guest texture format).
    inline DXGI_FORMAT ResolveTextureFormat(uint32_t format)
    {
      switch (format)
      {
      case 22: // k_24_8

      case 23: // k_24_8_FLOAT

      case 36: // k_32_FLOAT
        return DXGI_FORMAT_R32_FLOAT;

      case 7: // k_2_10_10_10

      case 54: // k_2_10_10_10_AS_16_16_16_16
        return DXGI_FORMAT_R10G10B10A2_UNORM;

      case 32: // k_16_16_16_16_FLOAT

      case 26:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;

      case 31:
      case 25:
        return DXGI_FORMAT_R16G16_FLOAT;

      default: // k_8_8_8_8 and anything else
        return DXGI_FORMAT_R8G8B8A8_UNORM;
      }
    }

  }

  // render targets: resolves + trace
  namespace renderer_detail
  {
    inline float BeFloat(const uint8_t *base, uint32_t addr)
    {
      const uint32_t v = be32(base, addr);
      float f;
      std::memcpy(&f, &v, 4);
      return f;
    }
  }

  // Everything that selects a pipeline. POD so it can be hashed bytewise
  struct PipelineKey
  {
    const ShaderVariant *vs;
    const ShaderVariant *ps;
    uint64_t layout_hash;
    uint32_t blend;     // RB_BLENDCONTROL0
    uint8_t color_mask; // RB_COLOR_MASK & 0xF
    uint8_t depth_enable, depth_write, depth_func;
    uint8_t cull, front_ccw;
    uint8_t topology_type; // D3D12_PRIMITIVE_TOPOLOGY_TYPE
    uint8_t strip_cut;     // 0 none, 1 0xFFFF, 2 0xFFFFFFFF
    uint8_t rtv_format;    // DXGI_FORMAT of the colour target (0: the output, legacy mode)
    uint8_t dsv;           // 0 legacy depth buffer, 1 none, 2 D32_FLOAT_S8X24
    int32_t depth_bias;    // polygon offset (D3D12 integer units)
    float slope_bias;      // polygon offset scale (per pixel)
  };

  // Pipelines are compiled on background threads (PipelineWorkers) and kept in an ID3D12PipelineLibrary saved to cache/pipelines.bin, so a pipeline is only ever compiled once per machine / driver. Until a new one is ready, draws that need it are skipped (no stall when a court loads).
  struct PipelineJob
  {
    uint64_t hash = 0;
    std::wstring name; // pipeline cache name (stable across runs)
    gfx::PipelineDesc desc;
    gfx::PipelineRef pso;
    int32_t error = 0; // the backend's failure code
    bool from_library = false;
    double ms = 0;
    std::atomic<int> state{0}; // 0 pending, 1 done, 2 failed
  };
}