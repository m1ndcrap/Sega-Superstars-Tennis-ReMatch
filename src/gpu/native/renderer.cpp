// sst_rematch - native renderer: D3D12 frame recording and draws

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
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include <xxhash.h>
#include <dxcapi.h>
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

REXCVAR_DEFINE_BOOL(sst_native_quads, true, "SST",
                    "Native renderer: draw QUADLIST / TRIFAN primitives (converted to triangle "
                    "lists). Off until textures work: full-screen passes would cover the scene");

REXCVAR_DEFINE_BOOL(sst_native_debug_shading, false, "SST",
                    "Native renderer: replace the game's pixel shaders with distance bands "
                    "(checks geometry independently of lighting / textures)");

REXCVAR_DEFINE_BOOL(sst_native_half_pixel, true, "SST",
                    "Native renderer: shift geometry by half a pixel (D3D9 pixel centres)");

REXCVAR_DEFINE_BOOL(sst_native_memexport, true, "SST",
                    "Native renderer: run memory-export (morph) vertex shaders on the CPU so "
                    "characters' faces and morphing meshes appear");

REXCVAR_DEFINE_BOOL(sst_native_present_queue, true, "SST",
                    "Native renderer (render thread): show every frame once, in order, keeping "
                    "one in reserve after a late frame (smooth, ~1 frame more latency). false = "
                    "always show the newest frame");

REXCVAR_DEFINE_INT32(sst_native_present_wait_ms, 4, "SST",
                     "Native renderer: at a swap, wait up to this long for the render thread to "
                     "finish a new frame before showing the previous one again (0 = never wait)");

REXCVAR_DEFINE_INT32(sst_native_geometry_cache_mb, 384, "SST",
                     "Native renderer: video memory for decoded vertex / index data kept between "
                     "frames (0 = decode every draw)");

REXCVAR_DEFINE_BOOL(sst_native_pipeline_async, true, "SST",
                    "Native renderer: create new pipelines on background threads (a draw whose "
                    "pipeline isn't ready yet is skipped for those frames instead of stalling)");

REXCVAR_DEFINE_BOOL(sst_native_pipeline_library, true, "SST",
                    "Native renderer: keep created pipelines in cache/pipelines.bin so later runs "
                    "load them instead of compiling");

REXCVAR_DEFINE_INT32(sst_native_rt_trace, 0, "SST",
                     "Native renderer debug: log the render-target passes, clears and resolves of "
                     "this frame number (F10 traces the next frame)");

REXCVAR_DEFINE_BOOL(sst_native_render_targets, true, "SST",
                    "Native renderer: emulate render targets and resolves (shadows, bloom, "
                    "post-processing). false = everything drawn straight into the output "
                    "(the earlier mode, with --sst_native_rt choosing targets)");

REXCVAR_DEFINE_BOOL(sst_native_memexport_async, true, "SST",
                    "Native renderer: run memory-export (morph) shaders on a background thread; "
                    "draws reading their output wait for them");

REXCVAR_DEFINE_BOOL(sst_native_diagnostics, false, "SST",
                    "Native renderer debug: extra consistency checks on morph (face) output "
                    "(costs CPU time)");

REXCVAR_DEFINE_BOOL(sst_native_memexport_cache, true, "SST",
                    "Native renderer: skip a memory-export draw when its inputs and previous "
                    "output are unchanged");

REXCVAR_DEFINE_STRING(sst_native_memexport_dump, "", "SST",
                      "Native renderer debug: write the first few runs of each memory-export (morph) shader to this folder - "
                      "microcode, constants, fetch constants, indices, input vertex data and the output - for offline "
                      "benchmarking of the interpreter");

REXCVAR_DEFINE_INT32(sst_native_memexport_threads, -1, "SST",
                     "Native renderer: extra threads for memory-export (morph) shaders; -1 = "
                     "automatic (half the CPU threads minus one, at most 6)");

REXCVAR_DEFINE_BOOL(sst_native_check_vs, false, "SST",
                    "Native renderer debug: run the vertex shaders of face (morph) draws on the "
                    "CPU too and log vertices that jump on screen while the mesh stays put");

REXCVAR_DEFINE_BOOL(sst_native_check_vs_all, false, "SST",
                    "Native renderer debug: like sst_native_check_vs for every draw (slow)");

REXCVAR_DEFINE_BOOL(sst_native_depth_flip_vs, true, "SST",
                    "Native renderer: flip the game's reversed depth in the vertex shader (z = w - z) instead of with an "
                    "inverted viewport depth range, which loses small depth gaps on Intel GPUs (eyes, moustaches, decals)");

REXCVAR_DEFINE_STRING(sst_native_trace_vs, "", "SST",
                      "Native renderer debug: log the state of draws with this vertex shader hash (16 hex digits): vertex "
                      "streams, decoded vertices, textures (first 40)");

REXCVAR_DEFINE_STRING(sst_native_skip_vs, "", "SST",
                      "Native renderer debug: skip draws with this vertex shader hash (16 hex digits)");

REXCVAR_DEFINE_BOOL(sst_native_debug_markers, false, "SST",
                    "Native renderer debug: label every draw for RenderDoc / PIX (shader hashes, depth / colour state)");

REXCVAR_DEFINE_BOOL(sst_native_debug_bisect, false, "SST",
                    "Native renderer debug: find the pixel shader that draws something wrong. Home = start (takes "
                    "the pixel shaders drawn since the last stop; half of them are hidden), Page Up = the problem is still "
                    "visible, Page Down = the problem is gone, End = stop. Each step is logged");

REXCVAR_DEFINE_BOOL(sst_native_skip_blended, false, "SST",
                    "Native renderer debug: skip alpha-blended draws (effects, glows, light "
                    "beams), which look like solid white shapes until textures are emulated");

REXCVAR_DEFINE_BOOL(sst_native_check_late_writes, false, "SST",
                    "Native renderer debug: report draws whose index / vertex data the game "
                    "changes after issuing the draw (within the frame)");

REXCVAR_DEFINE_INT32(sst_native_capture_frame, 0, "SST",
                     "Native renderer debug: capture this frame number (as F9 does): every "
                     "draw's vertex positions from the GPU are compared with the original "
                     "shader run on the CPU. -1 = every frame, logging only disagreements "
                     "(slow)");

REXCVAR_DEFINE_BOOL(sst_native_ui_fit, true, "SST",
                    "Native renderer: keep 2D UI in a 16:9 box at other aspect ratios (debug: "
                    "false stretches it, to tell UI fitting apart from game-side issues)");

REXCVAR_DEFINE_BOOL(sst_native_wide_targets, true, "SST",
                    "Native renderer: render targets take the display's shape (2520x1080 at "
                    "21:9 and 1080p) instead of a 16:9 image stretched to it");

REXCVAR_DEFINE_BOOL(sst_native_scissor, true, "SST",
                    "Native renderer: apply the game's scissor rectangle (menus clip panels "
                    "with it)");
// VIDEO page settings (src/menu/graphics_menu.cpp)
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

REXCVAR_DEFINE_STRING(sst_native_rt, "auto", "SST",
                      "Native renderer: which render targets draw into the output until render "
                      "targets are emulated: auto (viewport covers 1280x720), all, or a size as "
                      "logged in 'render target' lines (e.g. 640x736)");

namespace sst::native
{
  namespace
  {
    // --sst_native_debug_bisect (render thread only): halves the candidate pixel shaders on each key press
    struct ShaderBisect
    {
      std::unordered_set<uint64_t> recent; // pixel shaders drawn since the last start
      std::vector<uint64_t> candidates;    // sorted; the first `hidden` are skipped
      size_t hidden = 0;
      bool active = false;

      bool Hidden(uint64_t ps) const
      {
        return active && std::binary_search(candidates.begin(), candidates.begin() + ptrdiff_t(hidden), ps);
      }

      void Log(const char *what) const
      {
        std::string list;

        for (size_t i = 0; i < candidates.size() && i < 64; ++i)
          list += fmt::format(" {:016X}{}", candidates[i], i < hidden ? "*" : "");

        REXLOG_INFO("[sst-bisect] {}: {} candidate pixel shaders, {} hidden (*):{}", what, candidates.size(), hidden, list);

        if (candidates.size() == 1)
          REXLOG_INFO("[sst-bisect] FOUND: pixel shader {:016X}", candidates[0]);
      }

      void Split() { hidden = candidates.size() > 1 ? candidates.size() / 2 : candidates.size(); }

      void Poll()
      {
        if (!REXCVAR_GET(sst_native_debug_bisect))
          return;

        if (GetAsyncKeyState(VK_HOME) & 1)
        {
          candidates.assign(recent.begin(), recent.end());
          std::sort(candidates.begin(), candidates.end());
          active = !candidates.empty();
          Split();
          Log("start");
        }
        else if (active && (GetAsyncKeyState(VK_PRIOR) & 1))
        { // still visible: the culprit is among the shown ones
          candidates.erase(candidates.begin(), candidates.begin() + ptrdiff_t(hidden));
          Split();
          Log("still visible");
        }
        else if (active && (GetAsyncKeyState(VK_NEXT) & 1))
        { // gone: the culprit is among the hidden ones
          candidates.resize(hidden);
          Split();
          Log("gone");
        }
        else if (GetAsyncKeyState(VK_END) & 1)
        {
          active = false;
          recent.clear();
          REXLOG_INFO("[sst-bisect] stopped");
        }
      }
    };

    ShaderBisect &Bisect()
    {
      static ShaderBisect b;
      return b;
    }
  }

  using Microsoft::WRL::ComPtr;
  using rex::ui::d3d12::D3D12Presenter;
  using rex::ui::d3d12::D3D12Provider;

  namespace
  {
    constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    constexpr size_t kUploadPageSize = 32u << 20;
    constexpr uint32_t kMaxDrawVertices = 1u << 20;
    constexpr uint32_t kMaxDrawIndices = 1u << 21;

    // Root parameters (XenosRecomp D3D12 layout + SST raw-constant views)
    enum RootParam : UINT
    {
      kRootVSConstants = 0, // b0 space4
      kRootPSConstants,     // b1 space4
      kRootShared,          // b2 space4
      kRootVSConstantsRaw,  // b3 space4 (same buffer as b0)
      kRootPSConstantsRaw,  // b4 space4 (same buffer as b1)
      kRootTextures2D,      // t0 space0 (unbounded)
      kRootTextures3D,      // t0 space1
      kRootTexturesCube,    // t0 space2
      kRootSamplers,        // s0 space3
      kRootCount
    };

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

    // guest memory
    // On the render thread, the guest memory a command needs as it was when the game issued it (device state, patched microcode, vertex data from the caller) is read from snapshots taken by the hook (see native.cpp)
    thread_local const GuestOverlay *t_overlays = nullptr;
    thread_local uint32_t t_overlay_count = 0;
    const uint8_t *GuestPtr(const uint8_t *base, uint32_t addr)
    {
      for (uint32_t i = 0; i < t_overlay_count; ++i)
      {
        const GuestOverlay &o = t_overlays[i];

        if (addr - o.begin < o.end - o.begin)
          return o.data + (addr - o.begin);
      }

      return base + addr + rex::memory::detail::PhysicalHostOffset(addr);
    }

    uint32_t be32(const uint8_t *base, uint32_t addr)
    {
      uint32_t v;
      std::memcpy(&v, GuestPtr(base, addr), 4);
      return __builtin_bswap32(v);
    }

    uint32_t be32raw(const uint8_t *p)
    {
      uint32_t v;
      std::memcpy(&v, p, 4);
      return __builtin_bswap32(v);
    }
    // D3D object addresses are virtual; tolerate physical ones (map through the 0xA0000000 physical view)
    uint32_t ToVirtual(uint32_t addr)
    {
      return addr < 0x20000000u ? (addr | 0xA0000000u) : addr;
    }

    bool PlausibleAddress(uint32_t addr)
    {
      return addr >= 0x40000000u;
    }

    // Guest pixels -> host pixels at an internal resolution scale (rounded, so adjacent rectangles stay adjacent)
    LONG Px(LONG v, float s)
    {
      return LONG(std::lround(double(v) * s));
    }

    uint32_t PxU(uint32_t v, float s)
    {
      return std::max<uint32_t>(1, uint32_t(Px(LONG(v), s)));
    }

    D3D12_RECT Scaled(const D3D12_RECT &r, float s)
    {
      return {Px(r.left, s), Px(r.top, s), Px(r.right, s), Px(r.bottom, s)};
    }

    D3D12_RECT Scaled(const D3D12_RECT &r, Scale2 s)
    {
      return {Px(r.left, s.x), Px(r.top, s.y), Px(r.right, s.x), Px(r.bottom, s.y)};
    }

    // Vertex declaration bound at dev+0x2E2C (CreateVertexDeclaration sub_82300CA8 / its init sub_82300BB8): +0x18 element count, elements from +0x34, 12 bytes each {u16 stream, u16 offset, u32 D3DDECLTYPE (bits 0-5: the vertex format), u8 method, u8 usage, u8 usage index}. Vertex declaration bound at dev+0x2E2C (CreateVertexDeclaration sub_82300CA8, filled by sub_82300BB8): +0x18 element count, elements from +0x34, 12 bytes each {u16 stream, u16 offset, u32 D3DDECLTYPE (bits 0-5: the vertex format), u8 method, u8 usage, u8 usage index}; the list ends at stream 0xFF
    struct DeclInput
    {
      uint32_t stream = 0, offset = 0, format = 0, components = 0, stride = 0;
    };

    bool FindDeclInput(const uint8_t *base, uint32_t dev, uint32_t usage, uint32_t usage_index, DeclInput &out)
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
        if (all[i].stream == out.stream)
          out.stride = std::max(out.stride, all[i].offset + DeclFormatSize(all[i].format));

      return out.stride > 0 && out.stride < 256;
    }

    // Device register mirror (see native-renderer-d3d-layer.md): packed blocks
    uint32_t Reg(const uint8_t *base, uint32_t dev, uint32_t reg)
    {
      if (reg >= 0x2100 && reg <= 0x2114)
        return be32(base, dev + 0x28CC + 4 * (reg - 0x2100));

      if (reg >= 0x2200 && reg <= 0x220B)
        return be32(base, dev + 0x2934 + 4 * (reg - 0x2200));

      return 0;
    }

    float RegF(const uint8_t *base, uint32_t dev, uint32_t reg)
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
    constexpr uint32_t kDevScissorTL = 0x28C4, kDevScissorBR = 0x28C8; // PA_SC_WINDOW_SCISSOR_*
    // Fetch constants: slot j (2 dwords) at +0x480 + 8j. Textures use 3 slots each (sampler i = slots 3i..3i+2); vertex stream s is slot 95 - s
    constexpr uint32_t kDevFetchConstants = 0x480;

    D3D12_RESOURCE_BARRIER Transition(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = r;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = before;
      b.Transition.StateAfter = after;
      return b;
    }

    D3D12_BLEND BlendFactor(uint32_t f, bool alpha)
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

    D3D12_BLEND_OP BlendOp(uint32_t op)
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
    bool RenderTargetSelected(const uint8_t *base, uint32_t dev, uint32_t size_word)
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

  void SetGuestOverlays(const GuestOverlay *overlays, uint32_t count)
  {
    t_overlays = overlays;
    t_overlay_count = overlays ? count : 0;
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
    std::wstring name; // library name (stable across runs)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = S_OK;
    bool from_library = false;
    double ms = 0;
    std::atomic<int> state{0}; // 0 pending, 1 done, 2 failed
  };

  struct Renderer::PipelineWorkers
  {
    using Job = PipelineJob;

    PipelineWorkers(ID3D12Device *device, uint32_t threads, bool use_library, bool fast_shaders) : device(device)
    {
      if (use_library)
        OpenLibrary(fast_shaders);

      for (uint32_t i = 0; i < threads; ++i)
        workers.emplace_back([this]
                             { Main(); });
    }

    ~PipelineWorkers()
    {
      {
        std::lock_guard lock(mutex);
        quit = true;
      }

      wake.notify_all();

      for (auto &t : workers)
        t.join();

      while (saving.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

      Save();
    }

    void OpenLibrary(bool fast_shaders)
    {
      ComPtr<ID3D12Device1> device1;

      if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
        return;

      // SHADER PRECISION: FAST pipelines hold other DXIL under the same names: a file of their own
      path = rex::filesystem::GetExecutableFolder() / "cache" / (fast_shaders ? "pipelines_fast.bin" : "pipelines.bin");
      {
        std::ifstream in(path, std::ios::binary);

        if (in)
          blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      }

      HRESULT hr = E_FAIL;

      if (!blob.empty())
      {
        hr = device1->CreatePipelineLibrary(blob.data(), blob.size(), IID_PPV_ARGS(&library));

        if (FAILED(hr))
          REXLOG_INFO("[sst-native] pipeline cache {} not usable ({:08X}: new driver?); starting "
                      "a new one",
                      path.string(), uint32_t(hr));
      }

      if (!library)
      {
        blob.clear();
        hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library));

        if (FAILED(hr))
        {
          library.Reset();
          REXLOG_WARN("[sst-native] pipeline cache unavailable ({:08X})", uint32_t(hr));
          return;
        }
      }

      REXLOG_INFO("[sst-native] pipeline cache: {} ({} KB)", path.string(), blob.size() >> 10);
    }

    void Submit(std::shared_ptr<Job> job)
    {
      {
        std::lock_guard lock(mutex);
        queue.push_back(std::move(job));
      }

      wake.notify_one();
    }

    // Pipelines already in the cache file load in well under a millisecond: done right away by the render thread, so the draw isn't skipped
    bool TryLoad(Job &job)
    {
      if (!library)
        return false;

      const auto t0 = std::chrono::steady_clock::now();
      job.desc.InputLayout = {job.layout.data(), UINT(job.layout.size())};
      {
        std::lock_guard lock(library_mutex);

        if (FAILED(library->LoadGraphicsPipeline(job.name.c_str(), &job.desc, IID_PPV_ARGS(&job.pso))))
          return false;
      }

      job.from_library = true;
      job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      job.state.store(1, std::memory_order_release);
      return true;
    }

    void Build(Job &job)
    {
      const auto t0 = std::chrono::steady_clock::now();
      job.desc.InputLayout = {job.layout.data(), UINT(job.layout.size())};

      if (library)
      {
        std::lock_guard lock(library_mutex);

        if (SUCCEEDED(library->LoadGraphicsPipeline(job.name.c_str(), &job.desc, IID_PPV_ARGS(&job.pso))))
          job.from_library = true;
      }

      if (!job.pso)
      {
        job.hr = device->CreateGraphicsPipelineState(&job.desc, IID_PPV_ARGS(&job.pso));

        if (SUCCEEDED(job.hr) && library)
        {
          std::lock_guard lock(library_mutex);

          if (SUCCEEDED(library->StorePipeline(job.name.c_str(), job.pso.Get())))
            dirty = true;
        }
      }

      job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      job.state.store(job.pso ? 1 : 2, std::memory_order_release);
    }

    // Writes the library if it gained pipelines (on a worker when `background`).
    void Save()
    {
      std::vector<uint8_t> data;
      {
        std::lock_guard lock(library_mutex);

        if (!library || !dirty)
          return;

        dirty = false;
        data.resize(library->GetSerializedSize());

        if (data.empty() || FAILED(library->Serialize(data.data(), data.size())))
          return;
      }

      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      const auto tmp = std::filesystem::path(path).concat(".tmp");
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);

        if (!out)
          return;

        out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));

        if (!out)
          return;
      }

      std::filesystem::rename(tmp, path, ec);

      if (!ec)
        saved_bytes = data.size();
    }

    void SaveInBackground()
    {
      if (!library || !dirty || saving.exchange(true))
        return;

      std::thread(
          [this]
          {
            Save();
            saving = false;
          })
          .detach();
    }

    void Main()
    {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // background
      for (;;)
      {
        std::shared_ptr<Job> job;
        {
          std::unique_lock lock(mutex);
          wake.wait(lock, [&]
                    { return quit || !queue.empty(); });

          if (quit)
            return;

          job = std::move(queue.front());
          queue.pop_front();
        }

        Build(*job);
      }
    }

    ID3D12Device *device;
    ComPtr<ID3D12PipelineLibrary> library;
    std::vector<uint8_t> blob; // must outlive the library
    std::filesystem::path path;
    std::mutex mutex, library_mutex;
    std::condition_variable wake;
    std::deque<std::shared_ptr<Job>> queue;
    std::vector<std::thread> workers;
    bool quit = false, dirty = false;
    std::atomic<bool> saving{false};
    size_t saved_bytes = 0;
  };

  // setup

  bool Renderer::Initialize(const D3D12Provider &provider, ShaderCache *shaders, const uint8_t *physical_membase, uint32_t width, uint32_t height)
  {
    provider_ = &provider;
    physical_membase_ = physical_membase;
    // Guest physical memory is writable host memory; memexport writes into it
    {
      int32_t threads = REXCVAR_GET(sst_native_memexport_threads);

      if (threads < 0)
        threads = std::clamp(int32_t(std::thread::hardware_concurrency()) / 2 - 1, 0, 6);

      memexport_ = std::make_unique<MemexportRunner>(const_cast<uint8_t *>(physical_membase), uint32_t(std::min(threads, 16)));
      REXLOG_INFO("[sst-native] memexport: {} worker threads", memexport_->worker_threads());
    }

    device_ = provider.GetDevice();
    {
      static Renderer *diagnostics = nullptr;
      diagnostics = this;
      sst::gpu::SetDiagnosticsDevice(device_, [](uint64_t frame, uint32_t draw)
                                     { return diagnostics ? diagnostics->DrawLabel(frame, draw) : std::string(); });
    }
    shaders_ = shaders;
    width_ = width;
    height_ = height;

    // Outputs + RTVs
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kOutputs;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_))))
      return false;

    rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = D3D12Presenter::kGuestOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    for (uint32_t i = 0; i < kOutputs; ++i)
    {
      if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&outputs_[i]))))
        return false;

      device_->CreateRenderTargetView(outputs_[i].Get(), nullptr, Rtv(i));
      output_scale_[i] = Scale2{};
    }

    for (uint32_t i = 0; i < kSlots; ++i)
    {
      if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frames_[i].allocator))))
        return false;
    }

    // Depth
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_))))
      return false;

    desc.Format = kDepthFormat;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kDepthFormat;
    clear.DepthStencil.Depth = 1.0f;

    if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth_))))
      return false;

    device_->CreateDepthStencilView(depth_.Get(), nullptr, dsv_heap_->GetCPUDescriptorHandleForHeapStart());

    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr, IID_PPV_ARGS(&list_))))
      return false;

    list_->Close();

    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
      return false;

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!event_)
      return false;

    if (!CreateRootSignature())
    {
      REXLOG_ERROR("[sst-native] failed to create the root signature");
      return false;
    }

    CreateDebugPixelShader();
    CreateTimestampQueries();

    if (!CreateBlitResources())
      REXLOG_WARN("[sst-native] render-target resources unavailable: drawing into the output "
                  "only");

    if (!CreateDescriptorsAndPlaceholders())
    {
      REXLOG_ERROR("[sst-native] failed to create placeholder textures");
      return false;
    }

    textures_ = std::make_unique<TextureCache>();
    textures_->Initialize(device_, physical_membase, srv_heap_.Get(), kSrvReserved, kSrvHeapSize - kSrvReserved, sampler_heap_.Get(), kSamplerReserved, kSamplerHeapSize - kSamplerReserved);
    return true;
  }

  bool Renderer::CreateRootSignature()
  {
    D3D12_ROOT_PARAMETER params[kRootCount] = {};
    const UINT cbv_registers[] = {0, 1, 2, 3, 4};

    for (UINT i = 0; i < 5; ++i)
    {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      params[i].Descriptor.ShaderRegister = cbv_registers[i];
      params[i].Descriptor.RegisterSpace = 4;
      params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_DESCRIPTOR_RANGE ranges[4] = {};

    for (UINT i = 0; i < 4; ++i)
    {
      ranges[i].RangeType = i < 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      ranges[i].NumDescriptors = UINT_MAX; // unbounded (resource binding tier 2+)
      ranges[i].BaseShaderRegister = 0;
      ranges[i].RegisterSpace = i;
      ranges[i].OffsetInDescriptorsFromTableStart = 0;
      D3D12_ROOT_PARAMETER &p = params[kRootTextures2D + i];
      p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      p.DescriptorTable.NumDescriptorRanges = 1;
      p.DescriptorTable.pDescriptorRanges = &ranges[i];
      p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = kRootCount;
    rs.pParameters = params;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; // F9 capture
    ComPtr<ID3DBlob> blob, error;

    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
    {
      if (error)
        REXLOG_ERROR("[sst-native] root signature: {}", std::string(static_cast<const char *>(error->GetBufferPointer()), error->GetBufferSize()));

      return false;
    }

    return SUCCEEDED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature_)));
  }

  void Renderer::CreateDebugPixelShader()
  {
    // Colour by view distance (SV_Position.w) in bands, slightly darker on steep depth slopes so shapes read without lighting or textures
    static const char kSource[] = "float4 main(float4 p : SV_Position) : SV_Target0 {\n"
                                  "  float d = p.w;\n"
                                  "  float band = frac(d * 0.1);\n"
                                  "  float edge = saturate(1.0 - 40.0 * (abs(ddx(p.z)) + abs(ddy(p.z))));\n"
                                  "  float3 c = float3(0.35 + 0.65 * band, 0.5 + 0.5 * sin(d * 0.37), 1.0 - 0.6 * band);\n"
                                  "  return float4(c * (0.4 + 0.6 * edge), 1.0);\n"
                                  "}\n";
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;

    if (FAILED(provider_->DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
      return;

    const wchar_t *args[] = {L"-T", L"ps_6_0", L"-E", L"main", L"-Qstrip_reflect", L"-Qstrip_debug"};
    DxcBuffer buffer = {kSource, sizeof(kSource) - 1, DXC_CP_UTF8};
    Microsoft::WRL::ComPtr<IDxcResult> result;
    HRESULT status = E_FAIL;

    if (FAILED(compiler->Compile(&buffer, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result))) || FAILED(result->GetStatus(&status)) || FAILED(status))
      return;

    Microsoft::WRL::ComPtr<IDxcBlob> object;

    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
      return;

    const auto *b = static_cast<const uint8_t *>(object->GetBufferPointer());
    debug_ps_ = std::make_unique<ShaderVariant>();
    debug_ps_->dxil.assign(b, b + object->GetBufferSize());
    debug_ps_->state.store(ShaderVariant::State::kReady);
  }

  bool Renderer::CreateDescriptorsAndPlaceholders()
  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvHeapSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap_))))
      return false;

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = kSamplerHeapSize;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sampler_heap_))))
      return false;

    const UINT srv_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT sampler_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    // White 1x1 placeholders: 2D, 3D, cube (6 faces)
    struct Placeholder
    {
      D3D12_RESOURCE_DIMENSION dim;
      UINT16 depth_or_array;
      D3D12_SRV_DIMENSION view;
      uint32_t heap_index;
    } const kinds[] = {
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, D3D12_SRV_DIMENSION_TEXTURE2D, kPlaceholder2D},
        {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 1, D3D12_SRV_DIMENSION_TEXTURE3D, kPlaceholder3D},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 6, D3D12_SRV_DIMENSION_TEXTURECUBE, kPlaceholderCube},
        {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 1, D3D12_SRV_DIMENSION_TEXTURE3D, kPlaceholder3DBlack},
    };

    // One upload buffer: 9 subresources, 512 bytes each (aligned for copies); the last one is black
    D3D12_HEAP_PROPERTIES upload_heap = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 9 * 512;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;

    if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging))))
      return false;

    uint8_t *mapped = nullptr;
    staging->Map(0, nullptr, reinterpret_cast<void **>(&mapped));
    std::memset(mapped, 0xFF, 8 * 512);
    std::memset(mapped + 8 * 512, 0, 512);

    // Zero buffer for missing vertex inputs (also upload heap; never written)
    bd.Width = 256;

    if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&zero_buffer_))))
      return false;

    uint8_t *zero = nullptr;
    zero_buffer_->Map(0, nullptr, reinterpret_cast<void **>(&zero));
    std::memset(zero, 0, 256);
    zero_buffer_->Unmap(0, nullptr);

    frames_[0].allocator->Reset();
    list_->Reset(frames_[0].allocator.Get(), nullptr);
    uint32_t staging_index = 0;

    for (const Placeholder &k : kinds)
    {
      D3D12_RESOURCE_DESC td = {};
      td.Dimension = k.dim;
      td.Width = 1;
      td.Height = 1;
      td.DepthOrArraySize = k.depth_or_array;
      td.MipLevels = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      ComPtr<ID3D12Resource> tex;

      if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
        return false;

      const UINT subresources = k.dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : k.depth_or_array;

      for (UINT s = 0; s < subresources; ++s)
      {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = s;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = staging.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 512 * staging_index++;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256};
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }

      auto barrier = Transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      list_->ResourceBarrier(1, &barrier);

      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      sd.ViewDimension = k.view;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

      if (k.view == D3D12_SRV_DIMENSION_TEXTURE2D)
        sd.Texture2D.MipLevels = 1;

      if (k.view == D3D12_SRV_DIMENSION_TEXTURE3D)
        sd.Texture3D.MipLevels = 1;

      if (k.view == D3D12_SRV_DIMENSION_TEXTURECUBE)
        sd.TextureCube.MipLevels = 1;

      D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
      h.ptr += size_t(k.heap_index) * srv_size;
      device_->CreateShaderResourceView(tex.Get(), &sd, h);
      placeholder_textures_.push_back(tex);
    }
    // Fill the rest of the heap with the 2D placeholder so any index is valid
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      sd.Texture2D.MipLevels = 1;

      for (uint32_t i = 4; i < kSrvReserved; ++i)
      {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(i) * srv_size;
        device_->CreateShaderResourceView(placeholder_textures_[0].Get(), &sd, h);
      }
    }
    // Samplers: linear wrap everywhere
    D3D12_SAMPLER_DESC smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;

    for (uint32_t i = 0; i < kSamplerReserved; ++i)
    {
      D3D12_CPU_DESCRIPTOR_HANDLE h = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
      h.ptr += size_t(i) * sampler_size;
      device_->CreateSampler(&smp, h);
    }

    list_->Close();
    ID3D12CommandList *lists[] = {list_.Get()};
    provider_->GetDirectQueue()->ExecuteCommandLists(1, lists);
    provider_->GetDirectQueue()->Signal(fence_.Get(), ++fence_value_);
    WaitFence(fence_value_);
    staging->Unmap(0, nullptr);
    return true;
  }

  void Renderer::Shutdown()
  {
    if (memexport_)
      CollectMemexports(true);

    if (fence_)
      WaitFence(fence_value_);

    if (event_)
      CloseHandle(event_);

    event_ = nullptr;
    pipeline_workers_.reset(); // finishes compiles, saves the pipeline cache
    pending_pipelines_.clear();
    pipelines_.clear();

    if (textures_)
      textures_->Shutdown();

    geo_pool_.Reset();
    geo_entries_.clear();
    color_rts_.clear();
    depth_rts_.clear();
    resolved_.clear();
    retired_rts_.clear();
    blit_pipelines_.clear();
    textures_.reset();
    list_.Reset();

    for (auto &f : frames_)
    {
      f.allocator.Reset();
      f.pages.clear();
    }

    for (auto &o : outputs_)
      o.Reset();

    placeholder_textures_.clear();
    zero_buffer_.Reset();
    depth_.Reset();
    srv_heap_.Reset();
    sampler_heap_.Reset();
    rtv_heap_.Reset();
    dsv_heap_.Reset();
    root_signature_.Reset();
    fence_.Reset();
  }

  D3D12_CPU_DESCRIPTOR_HANDLE Renderer::Rtv(uint32_t slot) const
  {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(slot) * rtv_size_;
    return h;
  }

  void Renderer::WaitFence(uint64_t value)
  {
    if (!value || fence_->GetCompletedValue() >= value)
      return;

    if (SUCCEEDED(fence_->SetEventOnCompletion(value, event_)))
      WaitForSingleObject(event_, INFINITE);
  }

  Renderer::Upload Renderer::Allocate(size_t size, size_t alignment)
  {
    FrameResources &f = frames_[frame_res_];

    for (;;)
    {
      if (f.page < f.pages.size())
      {
        UploadPage &p = f.pages[f.page];
        const size_t start = (f.offset + alignment - 1) & ~(alignment - 1);

        if (start + size <= p.size)
        {
          f.offset = start + size;
          stats_.upload_bytes += size;
          return {p.cpu + start, p.gpu + start, p.resource.Get(), start};
        }

        ++f.page;
        f.offset = 0;
        continue;
      }

      UploadPage p;
      p.size = std::max(kUploadPageSize, (size + 0xFFFF) & ~size_t(0xFFFF));
      D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
      D3D12_RESOURCE_DESC bd = {};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width = p.size;
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

      if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p.resource))))
      {
        return {};
      }

      p.resource->SetName(L"sst upload page");

      p.resource->Map(0, nullptr, reinterpret_cast<void **>(&p.cpu));
      p.gpu = p.resource->GetGPUVirtualAddress();
      f.pages.push_back(std::move(p));
      f.page = f.pages.size() - 1;
      f.offset = 0;
    }
  }

  // render targets: host textures, resolve copies, presenting

  namespace
  {
    constexpr uint32_t kRtRtvCount = 256, kRtDsvCount = 64;
    constexpr uint32_t kRtSrvFirst = 16; // renderer-owned SRV slots: [16, kSrvReserved)

    // Host format of a guest colour render-target format (RB_COLOR_INFO bits 16-19)
    DXGI_FORMAT ColorTargetFormat(uint32_t format)
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
    DXGI_FORMAT ResolveTextureFormat(uint32_t format)
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

    const D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  bool Renderer::RenderTargetsEnabled() const
  {
    return REXCVAR_GET(sst_native_render_targets) && rt_rtv_heap_ && blit_root_;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE Renderer::RtRtv(uint32_t index) const
  {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rt_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(index) * rtv_size_;
    return h;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE Renderer::RtDsv(uint32_t index) const
  {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rt_dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(index) * dsv_size_;
    return h;
  }

  D3D12_GPU_DESCRIPTOR_HANDLE Renderer::SrvGpu(uint32_t slot) const
  {
    D3D12_GPU_DESCRIPTOR_HANDLE h = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += size_t(slot) * srv_size_;
    return h;
  }

  uint32_t Renderer::AllocateRtSrv()
  {
    if (rt_srv_next_ < kRtSrvFirst)
      rt_srv_next_ = kRtSrvFirst;

    return rt_srv_next_ < kSrvReserved ? rt_srv_next_++ : 0;
  }

  void Renderer::SetRTState(HostRT &rt, D3D12_RESOURCE_STATES state)
  {
    if (rt.state == state)
      return;

    auto b = Transition(rt.resource.Get(), rt.state, state);
    list_->ResourceBarrier(1, &b);
    rt.state = state;
  }

  Renderer::HostRT *Renderer::GetHostRT(const uint8_t *base, uint32_t surface, bool depth, Scale2 scale, uint32_t samples, uint32_t min_width, uint32_t min_height)
  {
    if (!PlausibleAddress(surface))
      return nullptr;

    auto &map = depth ? depth_rts_ : color_rts_;
    // Logical size: the surface's own size (size word at +0x24) or what is drawn into it, whichever is larger
    const uint32_t size = be32(base, surface + 0x24);
    const uint32_t sw = (size >> 18) + 1, sh = ((size >> 3) & 0x7FFF) + 1;
    const uint32_t w = std::clamp(std::max(sw, min_width), 1u, 4096u);
    const uint32_t h = std::clamp(std::max(sh, min_height), 1u, 4096u);
    const uint32_t guest_format = (be32(base, surface + 0x1C) >> 16) & 0xF;
    samples = UsableSamples(depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : ColorTargetFormat(guest_format), samples);
    auto it = map.find(surface);

    if (it != map.end())
    {
      HostRT &rt = it->second;

      if (rt.width >= w && rt.height >= h && rt.scale == scale && rt.samples == samples && (depth || rt.guest_format == guest_format))
        return &rt;
      // Grows (or changes format): make a new one; the old one may still be in use by submitted frames, so keep it alive until the GPU is done
      if (!depth && rt.guest_format != guest_format && LogOnce(0xA800000000000000ull ^ (uint64_t(surface) << 8) ^ guest_format))
        REXLOG_INFO("[sst-native] render target {:08X} changed format {} -> {}", surface, rt.guest_format, guest_format);

      retired_rts_.push_back({fence_value_ + 1, rt.resource});
    }

    HostRT rt;
    rt.depth = depth;
    rt.width = std::max(w, it != map.end() ? it->second.width : 0u);
    rt.height = std::max(h, it != map.end() ? it->second.height : 0u);
    rt.guest_format = guest_format;
    rt.scale = scale;
    rt.samples = samples;
    rt.format = depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : ColorTargetFormat(guest_format);
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = PxU(rt.width, scale.x);
    desc.Height = PxU(rt.height, scale.y);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = depth ? DXGI_FORMAT_R32G8X24_TYPELESS : rt.format;
    desc.SampleDesc.Count = samples;
    desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = rt.format;
    rt.state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;

    if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, rt.state, &clear, IID_PPV_ARGS(&rt.resource))))
    {
      if (LogOnce(0xA900000000000000ull ^ surface))
        REXLOG_WARN("[sst-native] could not create the {}x{} {} target for surface {:08X}", rt.width, rt.height, depth ? "depth" : "colour", surface);

      return nullptr;
    }

    {
      const std::string name = fmt::format("sst {} target {:08X} {}x{}", depth ? "depth" : "colour", surface, rt.width, rt.height);
      rt.resource->SetName(std::wstring(name.begin(), name.end()).c_str());
    }
    // Views: reuse the slots of the target being replaced
    if (it != map.end())
    {
      rt.view = it->second.view;
      rt.srv = it->second.srv;
    }
    else
    {
      rt.view = depth ? rt_dsv_next_++ : rt_rtv_next_++;
      rt.srv = AllocateRtSrv();

      if ((depth && rt.view >= kRtDsvCount) || (!depth && rt.view >= kRtRtvCount) || !rt.srv)
      {
        if (LogOnce(0xAA00000000000000ull))
          REXLOG_WARN("[sst-native] out of render-target descriptors");

        return nullptr;
      }
    }

    if (depth)
    {
      D3D12_DEPTH_STENCIL_VIEW_DESC dd = {};
      dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
      dd.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
      device_->CreateDepthStencilView(rt.resource.Get(), &dd, RtDsv(rt.view));
    }
    else
    {
      device_->CreateRenderTargetView(rt.resource.Get(), nullptr, RtRtv(rt.view));
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : rt.format;
    sd.ViewDimension = samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    srv_handle.ptr += size_t(rt.srv) * srv_size_;
    device_->CreateShaderResourceView(rt.resource.Get(), &sd, srv_handle);
    ++stats_.host_rts;
    REXLOG_INFO("[sst-native] render target {:08X}: {}x{} {}{} (surface {}x{}, guest format {}){}", surface, rt.width, rt.height, depth ? "depth D32S8" : "colour", samples > 1 ? fmt::format(" {}x MSAA", samples) : std::string(), sw, sh, guest_format, scale != Scale2{} ? fmt::format(", host {}x{}", PxU(rt.width, scale.x), PxU(rt.height, scale.y)) : std::string());
    bound_targets_ = 0;
    HostRT &stored = map[surface] = std::move(rt);

    if (!recording_)
      return &stored;
    // New contents are undefined: start from zero like EDRAM after a clear
    if (depth)
    {
      list_->ClearDepthStencilView(RtDsv(stored.view), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
    }
    else
    {
      static constexpr float kZero[4] = {};
      list_->ClearRenderTargetView(RtRtv(stored.view), kZero, 0, nullptr);
    }

    return &stored;
  }

  Renderer::ResolvedTexture *Renderer::GetResolvedTexture(uint32_t page, uint32_t guest_format, uint32_t width, uint32_t height, uint32_t slices, bool volume, Scale2 scale)
  {
    const DXGI_FORMAT format = ResolveTextureFormat(guest_format);
    auto it = resolved_.find(page);

    if (it != resolved_.end())
    {
      ResolvedTexture &t = *it->second;

      if (t.format == format && t.width == width && t.height == height && t.slices == slices && t.volume == volume && t.scale == scale)
        return &t;

      textures_->RemoveExternal(page);
      retired_rts_.push_back({fence_value_ + 1, t.resource});
    }

    auto t = std::make_unique<ResolvedTexture>();
    t->format = format;
    t->width = width;
    t->height = height;
    t->slices = slices;
    t->volume = volume;
    t->guest_format = guest_format;
    t->scale = scale;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = PxU(width, scale.x);
    desc.Height = PxU(height, scale.y);
    desc.DepthOrArraySize = UINT16(slices);
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    t->state = kShaderRead;

    if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, t->state, nullptr, IID_PPV_ARGS(&t->resource))))
    {
      if (LogOnce(0xAB00000000000000ull ^ page))
        REXLOG_WARN("[sst-native] could not create the {}x{}x{} resolve texture at {:08X}", width, height, slices, page << 12);

      resolved_.erase(page);
      return nullptr;
    }

    {
      const std::string name = fmt::format("sst resolve texture {:08X} {}x{}x{}", page << 12, width, height, slices);
      t->resource->SetName(std::wstring(name.begin(), name.end()).c_str());
    }
    // Views: one RTV per slice, an identity SRV (presenting). Reuse slots
    if (it != resolved_.end())
    {
      t->rtvs = it->second->rtvs;
      t->srv = it->second->srv;
    }
    while (t->rtvs.size() < slices)
    {
      if (rt_rtv_next_ >= kRtRtvCount)
        return nullptr;

      t->rtvs.push_back(rt_rtv_next_++);
    }

    if (!t->srv)
      t->srv = AllocateRtSrv();

    for (uint32_t i = 0; i < slices; ++i)
    {
      D3D12_RENDER_TARGET_VIEW_DESC rd = {};
      rd.Format = format;

      if (volume)
      {
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        rd.Texture3D.FirstWSlice = i;
        rd.Texture3D.WSize = 1;
      }
      else
      {
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      }

      device_->CreateRenderTargetView(t->resource.Get(), &rd, RtRtv(t->rtvs[i]));
    }

    if (t->srv)
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = format;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

      if (volume)
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
      h.ptr += size_t(t->srv) * srv_size_;
      device_->CreateShaderResourceView(t->resource.Get(), &sd, h);
    }

    TextureCache::External e;
    e.resource = t->resource.Get();
    e.dxgi_format = uint32_t(format);
    e.width = width;
    e.height = height;
    e.slices = slices;
    e.volume = volume;
    textures_->SetExternal(page, e);
    REXLOG_INFO("[sst-native] resolve texture {:08X}: {}x{}{} guest format {} -> DXGI {}", page << 12, width, height, slices > 1 ? fmt::format("x{}", slices) : "", guest_format, uint32_t(format));
    ResolvedTexture *raw = t.get();
    resolved_[page] = std::move(t);
    return raw;
  }

  bool Renderer::CreateBlitResources()
  {
    // Root signature: b0 = 8 root constants (source offset, flags, gamma; source rectangle for FXAA), t0 = source
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 8};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC linear = {};
    linear.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linear.AddressU = linear.AddressV = linear.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linear.MaxLOD = D3D12_FLOAT32_MAX;
    linear.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &linear;
    ComPtr<ID3DBlob> blob, error;

    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) || FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&blit_root_))))
      return false;

    static const char kVS[] = "float4 main(uint id : SV_VertexID) : SV_Position {\n"
                              "  float2 uv = float2((id << 1) & 2, id & 2);\n"
                              "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
                              "}\n";
    static const char kPS[] = "cbuffer C : register(b0) { int4 offset; };\n"
                              "Texture2D<float4> src : register(t0);\n"
                              "float4 main(float4 p : SV_Position) : SV_Target {\n"
                              "  float4 c = src.Load(int3(int2(p.xy) + offset.xy, 0));\n"
                              "  if (offset.z & 1) c = c.bgra;\n"
                              "  if (offset.w != 0) c.rgb = pow(saturate(c.rgb), asfloat(offset.w));\n"
                              "  return c;\n"
                              "}\n";
    // offset.z: bit 0 swap red / blue, bits 8-15 source samples, bit 16 depth
    static const char kPSMS[] = "cbuffer C : register(b0) { int4 offset; };\n"
                                "Texture2DMS<float4> src : register(t0);\n"
                                "float4 main(float4 p : SV_Position) : SV_Target {\n"
                                "  int2 at = int2(p.xy) + offset.xy;\n"
                                "  uint n = max((uint(offset.z) >> 8) & 0xFF, 1u);\n"
                                "  float4 c = src.Load(at, 0);\n"
                                "  if (((uint(offset.z) >> 16) & 1) == 0) {\n"
                                "    for (uint i = 1; i < n; ++i) c += src.Load(at, i);\n"
                                "    c /= float(n);\n"
                                "  }\n"
                                "  if (offset.z & 1) c = c.bgra;\n"
                                "  if (offset.w != 0) c.rgb = pow(saturate(c.rgb), asfloat(offset.w));\n"
                                "  return c;\n"
                                "}\n";
    // FXAA (the compact 'console' variant: 4 diagonal luma taps, one blur direction, two / four taps along it)
    static const char kPSFxaa[] = "cbuffer C : register(b0) { int4 offset; int4 rect; };\n"
                                  "Texture2D<float4> src : register(t0);\n"
                                  "SamplerState lin : register(s0);\n"
                                  "static float2 lo_uv, hi_uv;\n"
                                  "float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
                                  // Taps stay inside the copied rectangle: with tiled rendering the rest of the source holds another tile's (older) pixels
                                  "float3 S(float2 uv) { return src.SampleLevel(lin, clamp(uv, lo_uv, hi_uv), 0).rgb; }\n"
                                  "float4 main(float4 p : SV_Position) : SV_Target {\n"
                                  "  uint w, h; src.GetDimensions(w, h);\n"
                                  "  float2 rcp = 1.0 / float2(w, h);\n"
                                  "  lo_uv = (float2(rect.xy) + 0.5) * rcp;\n"
                                  "  hi_uv = (float2(rect.zw) - 0.5) * rcp;\n"
                                  "  float2 uv = (floor(p.xy) + float2(offset.xy) + 0.5) * rcp;\n"
                                  "  float4 m = src.SampleLevel(lin, uv, 0);\n"
                                  "  float nw = Luma(S(uv + float2(-0.5, -0.5) * rcp));\n"
                                  "  float ne = Luma(S(uv + float2(0.5, -0.5) * rcp));\n"
                                  "  float sw = Luma(S(uv + float2(-0.5, 0.5) * rcp));\n"
                                  "  float se = Luma(S(uv + float2(0.5, 0.5) * rcp));\n"
                                  "  float lm = Luma(m.rgb);\n"
                                  "  float lo = min(lm, min(min(nw, ne), min(sw, se)));\n"
                                  "  float hi = max(lm, max(max(nw, ne), max(sw, se)));\n"
                                  "  float4 c = m;\n"
                                  "  if (hi - lo >= max(0.0312, hi * 0.125)) {\n"
                                  "    float2 dir = float2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));\n"
                                  "    float reduce = max((nw + ne + sw + se) * (0.25 * 0.125), 1.0 / 128.0);\n"
                                  "    float k = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
                                  "    dir = clamp(dir * k, -8.0, 8.0) * rcp;\n"
                                  "    float3 a = 0.5 * (S(uv + dir * (1.0 / 3.0 - 0.5)) + S(uv + dir * (2.0 / 3.0 - 0.5)));\n"
                                  "    float3 b = a * 0.5 + 0.25 * (S(uv - dir * 0.5) + S(uv + dir * 0.5));\n"
                                  "    float lb = Luma(b);\n"
                                  "    c.rgb = (lb < lo || lb > hi) ? a : b;\n"
                                  "  }\n"
                                  "  if (offset.z & 1) c = c.bgra;\n"
                                  "  return c;\n"
                                  "}\n";
    ComPtr<IDxcCompiler3> compiler;

    if (FAILED(provider_->DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
      return false;

    auto compile = [&](const char *source, const wchar_t *profile, std::vector<uint8_t> &out)
    {
      const wchar_t *args[] = {L"-T", profile, L"-E", L"main", L"-Qstrip_reflect", L"-Qstrip_debug"};
      DxcBuffer buffer = {source, std::strlen(source), DXC_CP_UTF8};
      ComPtr<IDxcResult> result;
      HRESULT status = E_FAIL;

      if (FAILED(compiler->Compile(&buffer, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result))) || FAILED(result->GetStatus(&status)) || FAILED(status))
        return false;

      ComPtr<IDxcBlob> object;

      if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
        return false;

      const auto *b = static_cast<const uint8_t *>(object->GetBufferPointer());
      out.assign(b, b + object->GetBufferSize());
      return true;
    };

    // FPS counter: c.xy = top-left (host pixels), c.z = font pixel size, c.w = frames per second x 10. 3x5 digits, drawn on a dark box
    static const char kPSText[] = "cbuffer C : register(b0) { int4 c; };\n"
                                  "static const uint kFont[11] = {0x7b6f, 0x2c97, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef,\n"
                                  "                               0x7249, 0x7bef, 0x7bcf, 0x2};\n"
                                  "float4 main(float4 p : SV_Position) : SV_Target {\n"
                                  "  int2 q = (int2(p.xy) - c.xy) / max(c.z, 1) - int2(1, 1);\n"
                                  "  uint chars[5]; uint n = 0;\n"
                                  "  uint v = uint(c.w) / 10, t = uint(c.w) % 10;\n"
                                  "  if (v >= 100) chars[n++] = (v / 100) % 10;\n"
                                  "  if (v >= 10) chars[n++] = (v / 10) % 10;\n"
                                  "  chars[n++] = v % 10; chars[n++] = 10; chars[n++] = t;\n"
                                  "  float4 col = float4(0, 0, 0, 0.55);\n"
                                  "  if (q.x >= 0 && q.y >= 0 && q.y < 5) {\n"
                                  "    uint i = uint(q.x) / 4, x = uint(q.x) % 4;\n"
                                  "    if (i < n && x < 3 && ((kFont[chars[i]] >> (14 - (uint(q.y) * 3 + x))) & 1))\n"
                                  "      col = float4(1, 1, 1, 1);\n"
                                  "  }\n"
                                  "  return col;\n"
                                  "}\n";

    if (!compile(kVS, L"vs_6_0", blit_vs_) || !compile(kPS, L"ps_6_0", blit_ps_) || !compile(kPSText, L"ps_6_0", blit_ps_text_) || !compile(kPSMS, L"ps_6_0", blit_ps_ms_) || !compile(kPSFxaa, L"ps_6_0", blit_ps_fxaa_))
    {
      blit_root_.Reset();
      return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kRtRtvCount;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rt_rtv_heap_))))
      return false;

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = kRtDsvCount;

    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rt_dsv_heap_))))
      return false;

    dsv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    srv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
  }

  ID3D12PipelineState *Renderer::GetBlitPipeline(DXGI_FORMAT format, uint32_t kind)
  {
    const uint32_t pso_key = uint32_t(format) | (kind << 16);
    auto it = blit_pipelines_.find(pso_key);

    if (it != blit_pipelines_.end())
      return it->second.Get();

    const std::vector<uint8_t> &ps = kind == 1 ? blit_ps_ms_ : kind == 2 ? blit_ps_fxaa_
                                                           : kind == 3   ? blit_ps_text_
                                                                         : blit_ps_;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = blit_root_.Get();
    d.VS = {blit_vs_.data(), blit_vs_.size()};
    d.PS = {ps.data(), ps.size()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    if (kind == 3)
    {
      D3D12_RENDER_TARGET_BLEND_DESC &b = d.BlendState.RenderTarget[0];
      b.BlendEnable = TRUE;
      b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      b.BlendOp = D3D12_BLEND_OP_ADD;
      b.SrcBlendAlpha = D3D12_BLEND_ONE;
      b.DestBlendAlpha = D3D12_BLEND_ZERO;
      b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      b.LogicOp = D3D12_LOGIC_OP_NOOP;
    }

    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = format;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;

    if (FAILED(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))))
      pso = nullptr;

    return blit_pipelines_.emplace(pso_key, pso).first->second.Get();
  }

  void Renderer::Blit(uint32_t src_srv, int32_t src_x, int32_t src_y, D3D12_CPU_DESCRIPTOR_HANDLE rtv, DXGI_FORMAT format, const D3D12_RECT &dst, bool swap_rb, float gamma, uint32_t src_samples, bool fxaa)
  {
    // src_samples: bits 0-7 MSAA samples of the source, 0x100 = depth source
    const uint32_t samples = std::max(src_samples & 0xFF, 1u);
    const uint32_t kind = samples > 1 ? 1 : fxaa ? 2
                                                 : 0;
    ID3D12PipelineState *pso = GetBlitPipeline(format, kind);

    if (!pso || dst.right <= dst.left || dst.bottom <= dst.top)
      return;

    list_->SetGraphicsRootSignature(blit_root_.Get());
    list_->SetPipelineState(pso);
    int32_t gamma_bits = 0;

    if (gamma > 0 && gamma != 1.0f)
      std::memcpy(&gamma_bits, &gamma, 4);

    const int32_t flags = (swap_rb ? 1 : 0) | int32_t(samples << 8) | int32_t((src_samples & 0x100) << 8);
    const int32_t offset[8] = {src_x - dst.left, src_y - dst.top, flags, gamma_bits, src_x, src_y, src_x + (dst.right - dst.left), src_y + (dst.bottom - dst.top)};
    list_->SetGraphicsRoot32BitConstants(0, 8, offset, 0);
    list_->SetGraphicsRootDescriptorTable(1, SrvGpu(src_srv));
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    // The triangle covers the whole viewport; viewport + scissor = destination
    const D3D12_VIEWPORT vp = {float(dst.left), float(dst.top), float(dst.right - dst.left), float(dst.bottom - dst.top), 0, 1};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &dst);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    NoteDraw(0, 1);
    list_->DrawInstanced(3, 1, 0, 0);
    RestoreDrawState();
  }

  void Renderer::RestoreDrawState()
  {
    list_->SetGraphicsRootSignature(root_signature_.Get());

    for (UINT i = 0; i < 3; ++i)
      list_->SetGraphicsRootDescriptorTable(kRootTextures2D + i, srv_heap_->GetGPUDescriptorHandleForHeapStart());

    list_->SetGraphicsRootDescriptorTable(kRootSamplers, sampler_heap_->GetGPUDescriptorHandleForHeapStart());
    bound_targets_ = 0;
  }

  // End of frame: show the game's front buffer (its last full-size resolve), or failing that the last colour target drawn to.
  void Renderer::PresentRenderTargets()
  {
    const D3D12_RECT full = Scaled({0, 0, LONG(width_), LONG(height_)}, output_scale_[target_]);
    // (Looked up by page: the texture may have been replaced since the resolve.)
    ResolvedTexture *present_source_ = nullptr;

    if (present_page_)
    {
      auto it = resolved_.find(present_page_);

      if (it != resolved_.end())
        present_source_ = it->second.get();
    }

    present_page_ = 0;

    if (present_source_ && present_source_->srv && !present_source_->volume)
    {
      ++stats_.presented_from_resolve;

      if (present_source_->state != kShaderRead)
      {
        auto b = Transition(present_source_->resource.Get(), present_source_->state, kShaderRead);
        list_->ResourceBarrier(1, &b);
        present_source_->state = kShaderRead;
      }

      Blit(present_source_->srv, 0, 0, Rtv(target_), D3D12Presenter::kGuestOutputFormat, full, present_source_->rb_swapped, gamma_);
    }
    else if (last_color_rt_ && last_color_rt_->srv)
    {
      SetRTState(*last_color_rt_, kShaderRead);
      Blit(last_color_rt_->srv, 0, 0, Rtv(target_), D3D12Presenter::kGuestOutputFormat, full, false, gamma_, last_color_rt_->samples);
      SetRTState(*last_color_rt_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    last_color_rt_ = nullptr;

    if (REXCVAR_GET(sst_fps_counter))
      DrawFpsCounter(full);
  }

  void Renderer::DrawFpsCounter(const D3D12_RECT &output)
  {
    // Rate of new frames handed to the display, updated twice a second
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const uint64_t now = uint64_t(qpc.QuadPart), frames = shown_frames_.load(std::memory_order_relaxed);
    static const double kFreq = []
    {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return double(f.QuadPart);
    }();

    if (!fps_last_tick_)
    {
      fps_last_tick_ = now;
      fps_last_frames_ = frames;
    }
    else if (double(now - fps_last_tick_) >= kFreq * 0.5)
    {
      const double fps = double(frames - fps_last_frames_) * kFreq / double(now - fps_last_tick_);
      fps_tenths_ = uint32_t(std::clamp(fps * 10.0 + 0.5, 0.0, 9999.0));
      fps_last_tick_ = now;
      fps_last_frames_ = frames;
    }

    ID3D12PipelineState *pso = GetBlitPipeline(D3D12Presenter::kGuestOutputFormat, 3);

    if (!pso)
      return;

    const LONG out_h = output.bottom - output.top;
    const int32_t px = std::max<int32_t>(2, int32_t(out_h / 240));
    const int32_t chars = fps_tenths_ >= 1000 ? 5 : fps_tenths_ >= 100 ? 4
                                                                       : 3;
    const D3D12_RECT box = {px * 4, px * 4, px * 4 + px * (chars * 4 + 1), px * 4 + px * 7};
    list_->SetGraphicsRootSignature(blit_root_.Get());
    list_->SetPipelineState(pso);
    const int32_t c[4] = {box.left, box.top, px, int32_t(fps_tenths_)};
    list_->SetGraphicsRoot32BitConstants(0, 4, c, 0);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(target_);
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp = {float(box.left), float(box.top), float(box.right - box.left), float(box.bottom - box.top), 0, 1};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &box);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    NoteDraw(0, 1);
    list_->DrawInstanced(3, 1, 0, 0);
    RestoreDrawState();
  }

  // render targets: resolves + trace

  namespace
  {
    float BeFloat(const uint8_t *base, uint32_t addr)
    {
      const uint32_t v = be32(base, addr);
      float f;
      std::memcpy(&f, &v, 4);
      return f;
    }
  }

  std::string Renderer::DescribeTexture(const uint32_t fetch[6])
  {
    const uint32_t type = fetch[0] & 3, format = fetch[1] & 0x3F;
    const uint32_t dim = (fetch[5] >> 9) & 3, stacked = (fetch[1] >> 10) & 1;
    uint32_t w, h, d;

    if (dim == 2)
    { // 3D
      w = (fetch[2] & 0x7FF) + 1, h = ((fetch[2] >> 11) & 0x7FF) + 1, d = (fetch[2] >> 22) + 1;
    }
    else
    {
      w = (fetch[2] & 0x1FFF) + 1, h = ((fetch[2] >> 13) & 0x1FFF) + 1, d = (fetch[2] >> 26) + 1;
    }

    return fmt::format("{:08X} fmt {} {}x{}{} dim {}{}{} pitch {} endian {} swz {:03X} mips {:08X} "
                       "(type {})",
                       fetch[1] & ~0xFFFu, format, w, h, d > 1 ? fmt::format("x{}", d) : "", dim, stacked ? " stacked" : "", (fetch[0] >> 31) ? " tiled" : " linear", ((fetch[0] >> 22) & 0x1FF) * 32, (fetch[1] >> 6) & 3, (fetch[3] >> 1) & 0xFFF, fetch[5] & ~0xFFFu, type);
  }

  std::string Renderer::DescribeSurface(const uint8_t *base, uint32_t surface)
  {
    if (!surface)
      return "none";

    if (!PlausibleAddress(surface))
      return fmt::format("{:08X}?", surface);

    const uint32_t info = be32(base, surface + 0x1C), size = be32(base, surface + 0x24);
    std::string s = fmt::format("{:08X} [edram base {} fmt {} size word {:08X}]", surface, info & 0xFFF, (info >> 16) & 0xF, size);

    if (rt_trace_.surfaces_logged.insert(surface).second)
    {
      std::string words;

      for (uint32_t i = 0; i < 16; ++i)
        words += fmt::format(" {:08X}", be32(base, surface + 4 * i));

      rt_trace_.lines.push_back(fmt::format("  surface {:08X}:{}", surface, words));
    }

    return s;
  }

  void Renderer::TraceClosePass()
  {
    RtTrace &t = rt_trace_;

    if (!t.has_pass)
      return;

    std::string prims;

    for (auto &[p, n] : t.pass_prims)
      prims += fmt::format(" {:X}:{}", p, n);

    t.lines.push_back(fmt::format("  -> {} draws ({} morph) prims{}; {} pixel shaders", t.pass_draws, t.pass_morph, prims, t.pass_ps.size()));

    for (const std::string &tex : t.pass_textures)
      t.lines.push_back("     samples " + tex);

    t.has_pass = false;
  }

  void Renderer::TraceState(const uint8_t *base, uint32_t dev)
  {
    RtTrace &t = rt_trace_;
    uint32_t words[20];
    uint32_t n = 0;

    for (uint32_t i = 0; i < 5; ++i)
      words[n++] = be32(base, dev + kDevRT0 + 4 * i); // RT0-3, depth
    words[n++] = be32(base, dev + 0x2880);            // RB_SURFACE_INFO
    words[n++] = be32(base, dev + 0x2884);            // RB_COLOR_INFO
    words[n++] = be32(base, dev + 0x2888);            // RB_DEPTH_INFO
    for (uint32_t r = PA_CL_VPORT_XSCALE; r <= PA_CL_VPORT_ZOFFSET; ++r)
      words[n++] = Reg(base, dev, r);

    words[n++] = be32(base, dev + kDevScissorTL);
    words[n++] = be32(base, dev + kDevScissorBR);
    const uint64_t key = XXH3_64bits(words, n * 4);

    if (t.has_pass && key == t.pass_key)
      return;

    TraceClosePass();
    t.has_pass = true;
    t.pass_key = key;
    t.pass_draws = t.pass_morph = 0;
    t.pass_prims.clear();
    t.pass_textures.clear();
    t.pass_texture_keys.clear();
    t.pass_ps.clear();
    const float xs = RegF(base, dev, PA_CL_VPORT_XSCALE), ys = RegF(base, dev, PA_CL_VPORT_YSCALE);
    const float xo = RegF(base, dev, PA_CL_VPORT_XOFFSET), yo = RegF(base, dev, PA_CL_VPORT_YOFFSET);
    const uint32_t tl = words[n - 2], br = words[n - 1];
    t.lines.push_back(fmt::format("pass {}: RT0 {} | RT1 {:08X} | depth {} | surface info {:08X} colour info {:08X} depth info "
                                  "{:08X} | viewport scale {} {} offset {} {} | scissor {}x{}-{}x{}",
                                  ++t.pass_index, DescribeSurface(base, words[0]), words[1], DescribeSurface(base, words[4]), words[5], words[6], words[7], xs, ys, xo, yo, tl & 0x7FFF, (tl >> 16) & 0x7FFF, br & 0x7FFF, (br >> 16) & 0x7FFF));
  }

  void Renderer::TraceDraw(const uint8_t *base, uint32_t dev, const DrawCall &call, bool morph)
  {
    RtTrace &t = rt_trace_;
    TraceState(base, dev);
    ++t.pass_draws;

    if (morph)
      ++t.pass_morph;

    ++t.pass_prims[call.prim];
    const uint32_t ps_obj = be32(base, dev + kDevPS);
    ShaderEntry *ps = ps_obj ? ResolveShader(base, ps_obj, false) : nullptr;

    if (call.up_data && PlausibleAddress(call.up_data))
    {
      // Vertex-data draws (DrawVerticesUP / immediate mode): the raw vertices.
      const uint32_t vs_obj = be32(base, dev + kDevVS);
      ShaderEntry *vs = vs_obj ? ResolveShader(base, vs_obj, true) : nullptr;
      std::string line = fmt::format("  {} draw: prim {} vertices {} stride {} vs {:016X} ps {:016X} depthcontrol {:08X} "
                                     "blendcontrol {:08X} colorcontrol {:08X} mode {:08X}",
                                     call.immediate ? "immediate-mode" : "DrawVerticesUP", call.prim, call.count, call.up_stride, vs ? vs->hash : 0, ps ? ps->hash : 0, Reg(base, dev, RB_DEPTHCONTROL), Reg(base, dev, RB_BLENDCONTROL0), Reg(base, dev, RB_COLORCONTROL), Reg(base, dev, PA_SU_SC_MODE_CNTL));
      const uint32_t words = std::min(call.up_stride / 4, 8u);
      const uint32_t shown = std::min(call.count, 6u);

      for (uint32_t v = 0; v < call.count && words; ++v)
      {
        if (v == shown && v + 1 < call.count)
          v = call.count - 1; // ... and the last one
        t.lines.push_back(std::move(line));
        line = fmt::format("      v{}:", v);

        for (uint32_t w = 0; w < words; ++w)
        {
          const uint32_t x = be32(base, call.up_data + v * call.up_stride + 4 * w);
          float f;
          std::memcpy(&f, &x, 4);
          line += (std::fabs(f) > 1e-6f && std::fabs(f) < 1e6f) ? fmt::format(" {:.3f}", f) : fmt::format(" {:08X}", x);
        }
      }

      t.lines.push_back(line);
    }

    if (!ps)
      return;

    t.pass_ps.insert(ps->hash);
    const TextureUsage &u = GetTextureUsage(ps);

    for (uint32_t i = 0; i < 16; ++i)
    {
      if (!((u.mask >> i) & 1))
        continue;

      uint32_t fetch[6];

      for (uint32_t k = 0; k < 6; ++k)
        fetch[k] = be32(base, dev + kDevFetchConstants + 24 * i + 4 * k);

      const uint64_t key = (uint64_t(fetch[1]) << 32) ^ fetch[2] ^ (uint64_t(i) << 58);

      if (t.pass_textures.size() < 16 && t.pass_texture_keys.insert(key).second)
        t.pass_textures.push_back(fmt::format("s{} (tfetch dim {}): {}", i, u.uses[i].dimension - 1, DescribeTexture(fetch)));
    }
  }

  void Renderer::TraceEvent(const uint8_t *base, uint32_t device, const std::string &what)
  {
    if (!rt_trace_.active)
      return;

    TraceState(base, device);
    rt_trace_.lines.push_back("  " + what);
  }

  void Renderer::Resolve(const uint8_t *base, const ResolveCall &c)
  {
    ++stats_.resolves;
    const uint32_t dev = c.device;

    if (!PlausibleAddress(dev))
      return;
    // Destination texture (its fetch constant at +0x1C) and the copy rectangle
    uint32_t fetch[6] = {};

    if (PlausibleAddress(c.dst_texture))
      for (uint32_t k = 0; k < 6; ++k)
        fetch[k] = be32(base, c.dst_texture + 0x1C + 4 * k);

    int32_t rect[4] = {-1, -1, -1, -1}, point[2] = {-1, -1};

    if (PlausibleAddress(c.src_rect))
      for (int i = 0; i < 4; ++i)
        rect[i] = int32_t(be32(base, c.src_rect + 4 * i));

    if (PlausibleAddress(c.dst_point))
      for (int i = 0; i < 2; ++i)
        point[i] = int32_t(be32(base, c.dst_point + 4 * i));

    const uint32_t which = c.flags & 7; // 0-3 colour target, 4 depth
    const uint32_t surface = which == 4 ? be32(base, dev + kDevDepth) : be32(base, dev + kDevRT0 + 4 * (which & 3));
    std::string colour = "none";

    if (PlausibleAddress(c.clear_color))
      colour = fmt::format("{} {} {} {}", BeFloat(base, c.clear_color), BeFloat(base, c.clear_color + 4), BeFloat(base, c.clear_color + 8), BeFloat(base, c.clear_color + 12));

    const std::string line = fmt::format("resolve flags {:X} from {} rect {} {} {} {} -> texture {:08X} {} at {} {} level {} slice {} "
                                         "clear colour {} z {}",
                                         c.flags, which == 4 ? "depth" : fmt::format("RT{}", which & 3), rect[0], rect[1], rect[2], rect[3], c.dst_texture, fetch[0] ? DescribeTexture(fetch) : std::string("(none)"), point[0], point[1], c.level, c.slice, colour, c.clear_z);

    if (rt_trace_.active)
    {
      TraceState(base, dev);
      rt_trace_.lines.push_back("  " + line);
      rt_trace_.lines.push_back("     source surface " + DescribeSurface(base, surface));
    }

    if (capture_active_)
      REXLOG_INFO("[sst-native] capture (before draw {}): {} source {:08X}{}", draw_seq_ + 1, line, surface, scene_surfaces_.count(surface) ? " (scene)" : "");
    else if (LogOnce(0xA700000000000000ull ^ (uint64_t(c.flags) << 40) ^ (fetch[1] & ~0xFFFu) ^ (uint64_t(surface) << 8)) && stats_.resolves_logged++ < 40)
      REXLOG_INFO("[sst-native] {}", line);

    // the copy
    if (!RenderTargetsEnabled())
      return;

    if (!recording_)
      BeginFrame();

    const bool depth = which == 4;
    auto &map = depth ? depth_rts_ : color_rts_;
    auto src_it = map.find(surface);

    if (src_it == map.end() || !src_it->second.srv || (fetch[0] & 3) != 2 || c.level != 0)
    {
      ++stats_.resolves_skipped;

      if (LogOnce(0xAD00000000000000ull ^ (uint64_t(c.flags) << 32) ^ surface))
        REXLOG_INFO("[sst-native] resolve skipped ({}): {}", src_it == map.end() ? "nothing drawn to the source yet" : c.level != 0 ? "mip level"
                                                                                                                                    : "destination",
                    line);

      return;
    }

    HostRT &src = src_it->second;
    const bool stacked = (fetch[1] >> 10) & 1;
    const uint32_t dw = (fetch[2] & 0x1FFF) + 1, dh = ((fetch[2] >> 13) & 0x1FFF) + 1;
    const uint32_t slices = stacked ? (fetch[2] >> 26) + 1 : 1;
    const uint32_t page = (fetch[1] >> 12) & 0x1FFFF;
    // The shadow map: its depth is resolved into a stacked (3D) texture. Its targets get the shadow map scale from the next frame on
    if (stacked && depth && shadow_surfaces_.insert(surface).second)
    {
      shadow_surfaces_.insert(be32(base, dev + kDevRT0));
      REXLOG_INFO("[sst-native] shadow map surfaces: depth {:08X}, colour {:08X}", surface, be32(base, dev + kDevRT0));
    }

    ResolvedTexture *dst = GetResolvedTexture(page, fetch[1] & 0x3F, dw, dh, slices, stacked, src.scale);
    // Full-screen images (the 3D scene, the glow chain, frame copies): draws that sample them are composites, not UI (aspect ratio fit)
    {
      const int32_t rw = (rect[0] >= 0 ? rect[2] - rect[0] : int32_t(dw));
      const bool full = !depth && !stacked && (scene_surfaces_.count(surface) || BloomSurface(src, surface) || rw * 10 >= int32_t(width_) * 9);

      if (full)
        composite_pages_.insert(page);
      else
        composite_pages_.erase(page);
    }

    if (!dst)
    {
      ++stats_.resolves_skipped;
      return;
    }
    // Source rectangle (default: the destination's size) and destination point
    const int32_t sx0 = rect[0] >= 0 ? rect[0] : 0, sy0 = rect[1] >= 0 ? rect[1] : 0;
    const int32_t sx1 = rect[0] >= 0 ? rect[2] : int32_t(dw), sy1 = rect[1] >= 0 ? rect[3] : int32_t(dh);
    const int32_t px = point[0] >= 0 ? point[0] : 0, py = point[1] >= 0 ? point[1] : 0;
    D3D12_RECT dr = {std::max<LONG>(px, 0), std::max<LONG>(py, 0), std::min<LONG>(px + (sx1 - sx0), LONG(dw)), std::min<LONG>(py + (sy1 - sy0), LONG(dh))};
    const uint32_t slice = std::min(c.slice, slices - 1);
    SetRTState(src, kShaderRead);

    if (dst->state != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
      auto b = Transition(dst->resource.Get(), dst->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      list_->ResourceBarrier(1, &b);
      dst->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    // Colour resolves into textures whose fetch swizzle reads blue from data component x (ARGB-style formats: swizzle zyxw) are stored red/blue swapped, as the Xbox's resolve does for them; the sampling swizzle then swaps them back
    const uint32_t dst_swizzle = (fetch[3] >> 1) & 0xFFF;
    const bool swap_rb = !depth && (dst_swizzle & 7) == 2 && ((dst_swizzle >> 6) & 7) == 0;
    dst->rb_swapped = swap_rb;
    {
      const Scale2 k = src.scale;
      const D3D12_RECT host = Scaled(dr, k);
      // FXAA (VIDEO > ANTI-ALIASING): applied to the 3D scene's colour copies, before the UI is drawn over the final image
      const bool fxaa = fxaa_ && !depth && !stacked && src.samples == 1 && scene_surfaces_.count(surface);
      Blit(src.srv, Px(sx0 + (dr.left - px), k.x), Px(sy0 + (dr.top - py), k.y), RtRtv(dst->rtvs[slice]), dst->format, host, swap_rb, 0.0f, src.samples | (depth ? 0x100u : 0u), fxaa);
    }
    {
      auto b = Transition(dst->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderRead);
      list_->ResourceBarrier(1, &b);
      dst->state = kShaderRead;
    }

    SetRTState(src, depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
    ++stats_.resolves_done;

    // Clears after the copy: 0x100 the colour target, 0x200 depth / stencil.
    const D3D12_RECT sr = {std::max(sx0, 0), std::max(sy0, 0), std::min<LONG>(sx1, src.width), std::min<LONG>(sy1, src.height)};

    if (sr.right > sr.left && sr.bottom > sr.top)
    {
      if ((c.flags & 0x100) && !depth)
      {
        float col[4] = {0, 0, 0, 0};

        if (PlausibleAddress(c.clear_color))
          for (int i = 0; i < 4; ++i)
            col[i] = BeFloat(base, c.clear_color + 4 * i);

        const D3D12_RECT host = Scaled(sr, src.scale);
        list_->ClearRenderTargetView(RtRtv(src.view), col, 1, &host);
      }

      if (c.flags & 0x200)
      {
        const uint32_t ds = be32(base, dev + kDevDepth);
        auto dit = depth_rts_.find(ds);

        if (dit != depth_rts_.end())
        {
          HostRT &d = dit->second;
          SetRTState(d, D3D12_RESOURCE_STATE_DEPTH_WRITE);
          const D3D12_RECT dsr = {sr.left, sr.top, std::min<LONG>(sr.right, d.width), std::min<LONG>(sr.bottom, d.height)};
          const D3D12_RECT host = Scaled(dsr, d.scale);

          if (dsr.right > dsr.left && dsr.bottom > dsr.top)
            list_->ClearDepthStencilView(RtDsv(d.view), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, std::clamp(c.clear_z, 0.0f, 1.0f), 0, 1, &host);
        }
      }
    }
    // The game's front buffer copy: the last full-size colour resolve
    if (!depth && !stacked && dw == width_ && dh == height_)
      present_page_ = page;
  }

  // performance helpers

  bool Renderer::GeoCacheUsable()
  {
    if (REXCVAR_GET(sst_native_geometry_cache_mb) <= 0 || capture_active_ || REXCVAR_GET(sst_native_check_vs) || REXCVAR_GET(sst_native_check_vs_all) || REXCVAR_GET(sst_native_check_late_writes))
      return false;

    if (!geo_pool_ && !geo_failed_)
    {
      geo_pool_size_ = uint64_t(std::min(REXCVAR_GET(sst_native_geometry_cache_mb), 4000)) << 20;
      D3D12_RESOURCE_DESC bd = {};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width = geo_pool_size_;
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

      if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&geo_pool_))))
      {
        geo_failed_ = true;
        REXLOG_WARN("[sst-native] geometry cache: could not allocate {} MB", geo_pool_size_ >> 20);
        return false;
      }

      geo_pool_->SetName(L"sst geometry cache");

      geo_state_ = D3D12_RESOURCE_STATE_COMMON;
      REXLOG_INFO("[sst-native] geometry cache: {} MB of video memory", geo_pool_size_ >> 20);
    }

    if (!geo_pool_)
      return false;
    // Buffers decay to COMMON after each submission: make it readable as vertex / index data for this frame's draws
    const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER);

    if (geo_state_ != kRead)
    {
      auto b = Transition(geo_pool_.Get(), geo_state_, kRead);
      list_->ResourceBarrier(1, &b);
      geo_state_ = kRead;
    }

    return true;
  }

  D3D12_GPU_VIRTUAL_ADDRESS Renderer::GeoAddress(const GeoEntry &e) const
  {
    return geo_pool_->GetGPUVirtualAddress() + e.offset;
  }

  void Renderer::GeoStore(GeoEntry &e, const Upload &src, uint32_t size)
  {
    if (!geo_pool_ || !src.resource || !size)
      return;

    const uint64_t aligned = (uint64_t(size) + 255) & ~uint64_t(255);

    if (aligned > geo_pool_size_ / 4)
      return; // not worth a quarter of the pool
    if (geo_offset_ + aligned > geo_pool_size_)
    {
      // Full: start over. Older entries become misses (generation); copies later in this frame are ordered after the draws that read the old data
      ++geo_generation_;
      geo_offset_ = 0;
      ++stats_.geo_flushes;
    }

    const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER);
    auto to_copy = Transition(geo_pool_.Get(), geo_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    list_->ResourceBarrier(1, &to_copy);
    list_->CopyBufferRegion(geo_pool_.Get(), geo_offset_, src.resource, src.offset, size);
    auto to_read = Transition(geo_pool_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    list_->ResourceBarrier(1, &to_read);
    geo_state_ = kRead;
    e.offset = geo_offset_;
    e.size = size;
    e.generation = geo_generation_;
    geo_offset_ += aligned;
    ++stats_.geo_stores;
    stats_.geo_store_bytes += size;
  }

  uint32_t Renderer::ConstantBytes(const ShaderEntry *e)
  {
    if (!e)
      return 16;

    if (auto it = constant_bytes_.find(e); it != constant_bytes_.end())
      return it->second;

    uint32_t result = 4096;
    const std::vector<uint8_t> &ct = e->container;
    auto ct32 = [&](size_t at) -> uint32_t
    {
      return at + 4 <= ct.size() ? (uint32_t(ct[at]) << 24) | (uint32_t(ct[at + 1]) << 16) | (uint32_t(ct[at + 2]) << 8) | ct[at + 3] : 0;
    };

    const uint32_t vsz = ct32(4), shader_off = ct32(0x18);
    const uint32_t phys_off = ct32(shader_off), bytes = ct32(size_t(shader_off) + 4);

    if (bytes && bytes % 12 == 0 && size_t(vsz) + phys_off + bytes <= ct.size())
    {
      std::vector<uint32_t> ucode(bytes / 4);

      for (size_t i = 0; i < ucode.size(); ++i)
        ucode[i] = ct32(size_t(vsz) + phys_off + 4 * i);

      uint64_t used[4];

      if (FloatConstantsRead(ucode.data(), uint32_t(ucode.size()), used))
      {
        int top = -1;

        for (int w = 3; w >= 0 && top < 0; --w)
          if (used[w])
            top = 64 * w + 63 - __builtin_clzll(used[w]);

        result = uint32_t(std::max(top + 1, 1)) * 16;
      }
    }

    constant_bytes_.emplace(e, result);
    return result;
  }

  D3D12_GPU_VIRTUAL_ADDRESS Renderer::UploadConstants(const uint8_t *guest, uint32_t bytes)
  {
    bytes = std::clamp(bytes, 16u, 4096u);
    const uint64_t hash = XXH3_64bits(guest, bytes) ^ (uint64_t(bytes) << 52);

    if (auto it = constant_cache_.find(hash); it != constant_cache_.end())
    {
      ++stats_.constants_reused;
      return it->second;
    }

    const Upload up = Allocate(bytes);

    if (!up.cpu)
      return 0;

    SwapVertexData(2, guest, up.cpu, bytes);
    constant_cache_.emplace(hash, up.gpu);
    ++stats_.constants_uploaded;
    return up.gpu;
  }

  void Renderer::ReadGpuTime(uint32_t slot)
  {
    if (!timestamps_ || !timestamp_pending_[slot])
      return;

    // Per pass: the time from each mark to the next goes to the mark's target
    if (pass_ts_ && pass_marks_[slot].size() >= 2)
    {
      const std::vector<PassMark> &marks = pass_marks_[slot];
      const D3D12_RANGE prange = {8 * size_t(slot) * kPassMarks, 8 * (size_t(slot) * kPassMarks + marks.size())};
      void *pm = nullptr;

      if (SUCCEEDED(pass_readback_->Map(0, &prange, &pm)) && pm)
      {
        const uint64_t *t = reinterpret_cast<const uint64_t *>(static_cast<const uint8_t *>(pm) + prange.Begin);

        for (size_t i = 0; i + 1 < marks.size(); ++i)
        {
          if (t[i + 1] < t[i] || !timestamp_frequency_)
            continue;

          const PassMark &m = marks[i];
          const uint64_t key = (uint64_t(m.rt) << 32) | (uint64_t(m.w) << 18) | (uint64_t(m.h) << 4) | m.kind;
          PassCost &c = pass_costs_[key];
          c.ms += double(t[i + 1] - t[i]) * 1000.0 / double(timestamp_frequency_);
          ++c.passes;
        }

        ++pass_frames_;
        const D3D12_RANGE none = {0, 0};
        pass_readback_->Unmap(0, &none);
      }
    }

    pass_marks_[slot].clear();

    timestamp_pending_[slot] = false;
    const D3D12_RANGE range = {16 * size_t(slot), 16 * size_t(slot) + 16};
    void *mapped = nullptr;

    if (FAILED(timestamp_readback_->Map(0, &range, &mapped)) || !mapped)
      return;

    uint64_t t[2];
    std::memcpy(t, static_cast<const uint8_t *>(mapped) + 16 * slot, 16);
    const D3D12_RANGE none = {0, 0};
    timestamp_readback_->Unmap(0, &none);

    if (t[1] <= t[0] || !timestamp_frequency_)
      return;

    const double ms = double(t[1] - t[0]) * 1000.0 / double(timestamp_frequency_);
    prof_.gpu_ms_sum += ms;
    prof_.gpu_ms_max = std::max(prof_.gpu_ms_max, ms);
    ++prof_.gpu_frames;
  }

  void Renderer::CreateTimestampQueries()
  {
    if (FAILED(provider_->GetDirectQueue()->GetTimestampFrequency(&timestamp_frequency_)))
      return;

    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2 * kSlots;

    if (FAILED(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&timestamps_))))
      return;

    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 16 * kSlots;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timestamp_readback_))))
      timestamps_.Reset();

    // Per-pass timestamps
    qd.Count = kPassMarks * kSlots;
    bd.Width = 8 * uint64_t(kPassMarks) * kSlots;

    if (FAILED(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&pass_ts_))) || FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pass_readback_))))
    {
      pass_ts_.Reset();
      pass_readback_.Reset();
    }
  }

  void Renderer::MarkPass(uint32_t rt, uint32_t w, uint32_t h, uint8_t kind)
  {
    std::vector<PassMark> &marks = pass_marks_[frame_res_];

    if (!pass_ts_ || marks.size() >= kPassMarks - 1)
      return;

    list_->EndQuery(pass_ts_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame_res_ * kPassMarks + uint32_t(marks.size()));
    marks.push_back({rt, uint16_t(std::min(w, 65535u)), uint16_t(std::min(h, 65535u)), kind});
  }

  // Every sst_native_log_every frames: where the frame time went.
  void Renderer::LogProfile()
  {
    Profile &p = prof_;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t tsc = Tick();

    if (p.period_start.time_since_epoch().count() && p.frames)
    {
      const double wall_ms = std::chrono::duration<double, std::milli>(now - p.period_start).count();
      const double ms_per_tick = wall_ms / double(std::max<uint64_t>(1, tsc - p.period_tsc));
      const double frames = double(p.frames);
      auto ms = [&](Phase ph)
      {
        return double(p.ticks[ph]) * ms_per_tick / frames;
      };

      double renderer = 0;

      for (uint32_t i = 0; i < kPhCount; ++i)
        if (i != kPhFenceWait)
          renderer += ms(Phase(i));

      const double frame = p.frame_ms_sum / frames;
      REXLOG_INFO("[sst-native] perf: {:.1f} fps, frame {:.2f} ms avg / {:.1f} max ({:.0f}% over "
                  "1/60 s, {:.0f}% over 1/30 s); GPU {:.2f} ms avg / {:.1f} max",
                  1000.0 / frame, frame, p.frame_ms_max, 100.0 * p.over_60 / frames, 100.0 * p.over_30 / frames, p.gpu_frames ? p.gpu_ms_sum / p.gpu_frames : 0.0, p.gpu_ms_max);

      if (pass_frames_ && !pass_costs_.empty())
      {
        std::vector<std::pair<uint64_t, PassCost>> v(pass_costs_.begin(), pass_costs_.end());
        std::sort(v.begin(), v.end(), [](const auto &x, const auto &y)
                  { return x.second.ms > y.second.ms; });
        std::string text;
        static constexpr const char *kKinds[5] = {"", " 3D scene", " shadow map", " frame start", " present / UI blits"};

        for (size_t i = 0; i < v.size() && i < 10; ++i)
        {
          const uint64_t k = v[i].first;
          const uint32_t kind = uint32_t(k & 15), w = uint32_t((k >> 18) & 0x3FFF), h = uint32_t((k >> 4) & 0x3FFF);
          text += fmt::format(" {}{:.2f} ms ({:.1f}/frame);", kind >= 3 ? std::string(kKinds[kind] + 1) + " " : fmt::format("{:08X} {}x{}{} ", uint32_t(k >> 32), w, h, kKinds[kind]), v[i].second.ms / double(pass_frames_), double(v[i].second.passes) / double(pass_frames_));
        }

        REXLOG_INFO("[sst-native] perf: GPU by pass (per frame):{}", text);
        pass_costs_.clear();
        pass_frames_ = 0;
      }
      REXLOG_INFO("[sst-native] perf: {} per frame: {:.2f} ms in the renderer, {:.2f} ms "
                  "{}; renderer: memexport {:.2f}, shaders {:.2f}, "
                  "inputs {:.2f}, indices {:.2f}, pipelines {:.2f}, vertices {:.2f}, constants "
                  "{:.2f}, textures {:.2f}, ui fit {:.2f}, recording {:.2f}, other {:.2f}, clears {:.2f}, "
                  "submit {:.2f}; waits: GPU {:.2f}, morphs {:.2f}",
                  threaded_ ? "render thread" : "game thread", renderer, std::max(0.0, frame - renderer), threaded_ ? "idle (waiting for the game's next frame)" : "in the game (and its own D3D)", ms(kPhMemexport), ms(kPhShaders), ms(kPhInputs), ms(kPhIndices), ms(kPhPipeline), ms(kPhVertices), ms(kPhConstants), ms(kPhTextures), ms(kPhUiFit), ms(kPhRecord), ms(kPhOther), ms(kPhClear), ms(kPhSubmit), ms(kPhFenceWait), stats_.memexport_wait_ms / frames);
      uint64_t acquires, repeats, skips;
      {
        std::lock_guard lock(slot_mutex_);
        acquires = acquires_;
        repeats = acquire_repeats_;
        skips = acquire_skips_;
        acquires_ = acquire_repeats_ = acquire_skips_ = 0;
      }

      REXLOG_INFO("[sst-native] perf: frames handed to the display: {} ({} shown twice, {} never "
                  "shown); frame intervals over 20 ms: {}",
                  acquires, repeats, skips, p.over_20);
    }

    const Profile fresh;
    const auto last = p.last_frame;
    p = fresh;
    p.last_frame = last;
    p.period_start = now;
    p.period_tsc = tsc;
  }

  // frame

  uint32_t Renderer::UsableSamples(DXGI_FORMAT format, uint32_t samples)
  {
    while (samples > 1)
    {
      const uint32_t key = (uint32_t(format) << 8) | samples;
      auto it = msaa_support_.find(key);

      if (it == msaa_support_.end())
      {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q = {};
        q.Format = format;
        q.SampleCount = samples;
        const bool ok = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) && q.NumQualityLevels > 0;
        it = msaa_support_.emplace(key, ok ? 1u : 0u).first;

        if (!ok)
          REXLOG_WARN("[sst-native] {}x MSAA not supported for DXGI format {}", samples, uint32_t(format));
      }

      if (it->second)
        return samples;

      samples /= 2;
    }

    return 1;
  }

  // The quarter-size glow / bloom chain (320x180 down to 20x11, 16:9 targets that are neither the 3D scene nor the shadow map)
  bool Renderer::BloomSurface(const HostRT &rt, uint32_t surface) const
  {
    if (rt.depth || rt.width > 320 || rt.width < 16 || scene_surfaces_.count(surface) || shadow_surfaces_.count(surface))
      return false;
    // Same shape as the frame (16:9, or 5:4 in the game's 4:3 mode)
    const int64_t aspect = int64_t(rt.width) * height_ - int64_t(rt.height) * width_;
    return std::abs(aspect) <= int64_t(width_) * 2;
  }

  // VIDEO > ASPECT RATIO: 0 4:3, 1 16:9, 2 16:10, 3 21:9, 4 32:9. 4:3 is the game's own 4:3 mode (a 1280x1024 frame, from startup); 4:3 picked later waits for a restart (16:9 until then)
  int Renderer::DisplayAspect() const
  {
    int aspect = std::clamp(REXCVAR_GET(sst_aspect), 0, 4);

    if (height_ > 720)
      aspect = 0;
    else if (aspect == 0)
      aspect = 1;

    return aspect;
  }
  // How much wider than the game's frame the display is (1 in its 4:3 mode)
  float Renderer::DisplayStretch() const
  {
    static constexpr float kAspects[5] = {4.0f / 3.0f, 16.0f / 9.0f, 1.6f, 21.0f / 9.0f, 32.0f / 9.0f};
    return height_ > 720 ? 1.0f : kAspects[DisplayAspect()] / (16.0f / 9.0f);
  }

  // Which scale a guest surface (and the depth buffer drawn with it) renders at: the shadow map's, the 3D scene's (surfaces drawn with MSAA, RB_SURFACE_INFO bits 16-17), or the UI / final image's. Remembered per surface so a target keeps its size between passes
  Scale2 Renderer::SurfaceScale(const uint8_t *base, uint32_t dev, uint32_t color, uint32_t depth)
  {
    if ((color && shadow_surfaces_.count(color)) || (depth && shadow_surfaces_.count(depth)))
      return shadow_scale_;

    const bool msaa = (be32(base, dev + 0x2880) >> 16) & 3;

    if (msaa)
    {
      if (color && scene_surfaces_.insert(color).second)
        REXLOG_INFO("[sst-native] 3D scene surface {:08X}", color);

      if (depth)
        scene_surfaces_.insert(depth);
    }

    if ((color && scene_surfaces_.count(color)) || (depth && scene_surfaces_.count(depth)))
      return scene_scale_;

    return scale_;
  }

  // The output this frame renders into (and the output depth buffer) at the current scale; the old textures are released once the GPU is done with them. Called with slot_mutex_ held (Acquire reads output_scale_)
  bool Renderer::ApplyScale()
  {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = PxU(width_, scale_.x);
    desc.Height = PxU(height_, scale_.y);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;

    if (output_scale_[target_] != scale_)
    {
      desc.Format = D3D12Presenter::kGuestOutputFormat;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      Microsoft::WRL::ComPtr<ID3D12Resource> output;

      if (FAILED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&output))))
      {
        REXLOG_ERROR("[sst-native] could not create a {}x{} output", desc.Width, desc.Height);
        scale_ = output_scale_[target_];
        return false;
      }

      retired_rts_.push_back({fence_value_ + 1, outputs_[target_]});
      outputs_[target_] = output;
      device_->CreateRenderTargetView(output.Get(), nullptr, Rtv(target_));
      output_scale_[target_] = scale_;
    }

    if (depth_scale_ != scale_)
    {
      desc.Format = kDepthFormat;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
      D3D12_CLEAR_VALUE clear = {};
      clear.Format = kDepthFormat;
      clear.DepthStencil.Depth = 1.0f;
      Microsoft::WRL::ComPtr<ID3D12Resource> depth;

      if (SUCCEEDED(device_->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth))))
      {
        retired_rts_.push_back({fence_value_ + 1, depth_});
        depth_ = depth;
        device_->CreateDepthStencilView(depth_.Get(), nullptr, dsv_heap_->GetCPUDescriptorHandleForHeapStart());
        depth_scale_ = scale_;
      }
    }

    return true;
  }

  void Renderer::BeginFrame()
  {
    {
      // VIDEO page settings. Targets / resolve textures are re-made at a new size on their next use. Internal resolution in lines of the game's frame (720, or 1024 in its 4:3 mode)
      const float lines = float(std::clamp(REXCVAR_GET(sst_resolution), 240, 4320)) / float(height_);
      const float scene_lines = lines * float(std::clamp(REXCVAR_GET(sst_render_scale), 25, 100)) / 100.0f;
      // Width follows the display's shape (RESOLUTION is the number of lines): at 21:9, 1080p renders 2520x1080 rather than a 1920x1080 image stretched. (Narrower displays - 16:10 - keep the full 16:9 width.)
      const float wide = REXCVAR_GET(sst_native_wide_targets) ? std::max(DisplayStretch(), 1.0f) : 1.0f;
      const Scale2 scale{lines * wide, lines}, scene{scene_lines * wide, scene_lines};
      // The stacked shadow map is a 3D texture: D3D12 caps those at 2048 texels
      const int shadow = std::clamp(REXCVAR_GET(sst_shadow_size), 0, 2048);
      const float shadow_k = shadow ? float(shadow) / 1024.0f : 0.125f;
      const Scale2 shadow_scale{shadow_k, shadow_k};
      wide_x_ = wide;

      if (scale != scale_ || scene != scene_scale_ || shadow_scale != shadow_scale_ || shadows_off_ != (shadow == 0))
      {
        REXLOG_INFO("[sst-native] internal resolution {}x{}, 3D {}x{}, shadow map {}", PxU(width_, scale.x), PxU(height_, scale.y), PxU(width_, scene.x), PxU(height_, scene.y), shadow ? fmt::format("{0}x{0}", shadow) : "off");
        scale_ = scale;
        scene_scale_ = scene;
        shadow_scale_ = shadow_scale;
        shadows_off_ = shadow == 0;
      }
      // Brightness 0-100 (50 = as rendered): the game's own curve is x^(1 / (0.9 + 0.006 b)) (its gamma ramp, which the port does not emulate), applied relative to its default (b = 50)
      const int b = std::clamp(REXCVAR_GET(sst_brightness), 0, 100);
      gamma_ = (0.9f + 0.006f * 50) / (0.9f + 0.006f * float(b));
      fps30_ = REXCVAR_GET(sst_fps_limit) <= 30;
      const int aa_type = REXCVAR_GET(sst_aa_type);
      const uint32_t msaa = aa_type == 2 ? uint32_t(std::clamp(REXCVAR_GET(sst_aa_level), 2, 8)) : 1;

      if (msaa != msaa_ || fxaa_ != (aa_type == 1) || bloom_off_ != !REXCVAR_GET(sst_bloom))
        REXLOG_INFO("[sst-native] anti-aliasing {}, bloom {}", aa_type == 1 ? std::string("FXAA") : msaa > 1 ? fmt::format("{}x MSAA", msaa)
                                                                                                             : std::string("off"),
                    REXCVAR_GET(sst_bloom) ? "on" : "off");

      msaa_ = msaa;
      fxaa_ = aa_type == 1;
      bloom_off_ = !REXCVAR_GET(sst_bloom);
      const int aspect = DisplayAspect();
      const float s = DisplayStretch();
      ui_fit_x_ = s > 1.0f ? 1.0f / s : 1.0f;
      // Narrower than 16:9 (16:10): the 2D layer is stretched to the full height (11 % taller) instead of fitted into a 16:9 box. The rules that decide what to fit are built around the screen's width (full-width layers stay full), so a vertical fit split bars from their texts and left uncovered bands above and below the menus; stretched, everything stays together
      ui_fit_y_ = 1.0f;

      if (aspect != aspect_)
        REXLOG_INFO("[sst-native] aspect ratio {}: UI fit {:.3f} x {:.3f}", std::array<const char *, 5>{"4:3", "16:9", "16:10", "21:9", "32:9"}[aspect], ui_fit_x_, ui_fit_y_);

      aspect_ = aspect;
    }
    {
      std::lock_guard lock(slot_mutex_);
      // A slot that holds nothing still to be shown: not the one being copied (pinned), not the last one shown (a swap may show it again), not a frame waiting its turn; else the oldest waiting frame (dropped)
      int pick = -1, oldest_waiting = -1;

      for (uint32_t i = 0; i < kOutputs; ++i)
      {
        const int s = int(i);

        if (s == pinned_ || s == acquired_slot_ || s == latest_)
          continue;

        if (output_seq_[i] > acquired_seq_)
        {
          if (oldest_waiting < 0 || output_seq_[i] < output_seq_[oldest_waiting])
            oldest_waiting = s;

          continue;
        }

        pick = s;
        break;
      }

      if (pick < 0)
        pick = oldest_waiting >= 0 ? oldest_waiting : (latest_ + 1) % int(kOutputs);

      if (pick == pinned_)
        pick = (pick + 1) % int(kOutputs);

      target_ = uint32_t(pick);
      output_seq_[target_] = 0;

      if (output_scale_[target_] != scale_ || depth_scale_ != scale_)
        ApplyScale();
    }

    frame_res_ = uint32_t(submitted_ % kSlots);
    FrameResources &f = frames_[frame_res_];
    {
      const uint64_t t0 = Tick();
      WaitFence(f.fence_value);
      prof_.ticks[kPhFenceWait] += Tick() - t0;
    }

    ReadGpuTime(frame_res_);
    constant_cache_.clear();
    bound_targets_ = 0;
    {
      const uint64_t done = fence_->GetCompletedValue();
      std::erase_if(retired_rts_, [&](const auto &r)
                    { return r.first <= done; });
    }

    geo_state_ = D3D12_RESOURCE_STATE_COMMON; // decayed after the last submission
    if (geo_entries_.size() > 400000)
    {
      geo_entries_.clear(); // forget old keys (data is re-stored on next use)
      ++geo_generation_;
      geo_offset_ = 0;
    }

    f.allocator->Reset();
    f.page = 0;
    f.offset = 0;
    list_->Reset(f.allocator.Get(), nullptr);
    {
      DrawLabels &labels = draw_labels_[(submitted_ + 1) % std::size(draw_labels_)];
      labels.frame = submitted_ + 1;
      labels.draws.clear();
      const std::wstring name = L"sst frame " + std::to_wstring(labels.frame);
      list_->SetName(name.c_str());
    }

    if (timestamps_)
      list_->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame_res_);

    pass_marks_[frame_res_].clear();
    pass_pair_ = 0;
    MarkPass(0, 0, 0, 3);

    auto to_rt = Transition(outputs_[target_].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &to_rt);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(target_);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    // Start each frame defined (the game clears explicitly too)
    static constexpr float kBlack[4] = {0, 0, 0, 1};
    list_->ClearRenderTargetView(rtv, kBlack, 0, nullptr);
    list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, last_depth_clear_, 0, 0, nullptr);

    if (capture_requested_ && EnsureCaptureResources())
    {
      capture_requested_ = false;
      capture_active_ = true;
      captured_.clear();
      so_used_ = 0;
      list_->CopyBufferRegion(so_buffer_.Get(), kSoDataBytes, so_zero_.Get(), 0, kSoCounterBytes);
      auto to_so = Transition(so_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
      list_->ResourceBarrier(1, &to_so);

      if (!capture_quiet_)
        REXLOG_INFO("[sst-native] capture: recording frame {}", submitted_ + 1);
    }

    textures_->BeginFrame(submitted_ + 1, fence_->GetCompletedValue(), fence_value_ + 1);
    ID3D12DescriptorHeap *heaps[] = {srv_heap_.Get(), sampler_heap_.Get()};
    list_->SetDescriptorHeaps(2, heaps);
    list_->SetGraphicsRootSignature(root_signature_.Get());

    for (UINT i = 0; i < 3; ++i)
      list_->SetGraphicsRootDescriptorTable(kRootTextures2D + i, srv_heap_->GetGPUDescriptorHandleForHeapStart());

    list_->SetGraphicsRootDescriptorTable(kRootSamplers, sampler_heap_->GetGPUDescriptorHandleForHeapStart());
    const D3D12_RECT scissor = Scaled({0, 0, LONG(width_), LONG(height_)}, output_scale_[target_]);
    list_->RSSetScissorRects(1, &scissor);
    recording_ = true;
  }

  void Renderer::NoteDraw(uint64_t vs, uint64_t ps)
  {
    draw_labels_[(submitted_ + 1) % std::size(draw_labels_)].draws.emplace_back(vs, ps);
  }

  std::string Renderer::DrawLabel(uint64_t frame, uint32_t draw) const
  {
    for (const DrawLabels &l : draw_labels_)
    {
      if (l.frame != frame)
        continue;

      if (draw >= l.draws.size())
        return fmt::format("draw {} of frame {} (not recorded)", draw, frame);

      const auto &[vs, ps] = l.draws[draw];
      return vs ? fmt::format("draw {}: vs {:016X} ps {:016X}", draw, vs, ps) : fmt::format("draw {}: renderer's own pass {}", draw, ps);
    }

    return fmt::format("draw {} of frame {} (labels gone)", draw, frame);
  }

  void Renderer::AddTextureSnapshot(uint32_t begin, uint32_t end, const uint8_t *data)
  {
    if (textures_)
      textures_->AddSnapshot(begin, end, data);
  }

  void Renderer::AddTextureUnchanged(uint32_t base_page, uint64_t hash)
  {
    if (textures_)
      textures_->AddUnchanged(base_page, hash);
  }

  void Renderer::EndFrame()
  {
    PhaseClock clk(prof_, kPhSubmit);

    if (!recording_)
      BeginFrame();
    // --sst_native_check_late_writes: did any draw's index / vertex data change after the draw was issued (the console's GPU would have read it later)?
    for (const LateCheck &lc : late_checks_)
    {
      for (size_t r = 0; r < lc.ranges.size(); ++r)
      {
        const auto &g = lc.ranges[r];

        if (XXH3_64bits(g.ptr, g.size) == g.hash)
          continue;

        ++stats_.late_changed;

        if (g.index)
          ++stats_.late_changed_index;

        if (stats_.late_logged < 40)
        {
          ++stats_.late_logged;
          REXLOG_WARN("[sst-native] late write: {} data of draw (VS {:016X}, prim {}, count {}{}) "
                      "changed after the draw was issued, before the frame ended ({} bytes at "
                      "host {}, {} draws ago)",
                      g.index ? std::string("index") : fmt::format("vertex stream {}", lc.ranges[0].index ? r - 1 : r), lc.vs, lc.prim, lc.count, lc.morph ? ", reads morph output" : "", g.size, fmt::ptr(g.ptr), draw_seq_ - lc.draw);
        }
      }
    }

    stats_.late_checked += late_checks_.size();
    late_checks_.clear();

    MarkPass(0, 0, 0, 4);

    if (RenderTargetsEnabled())
      PresentRenderTargets();

    auto to_src = Transition(outputs_[target_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(1, &to_src);
    const bool capturing = capture_active_;

    if (capturing)
    {
      D3D12_RESOURCE_BARRIER b[2] = {Transition(so_buffer_.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE), {}};
      list_->ResourceBarrier(1, b);
      list_->CopyResource(so_readback_.Get(), so_buffer_.Get());
      b[0] = Transition(so_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      list_->ResourceBarrier(1, b);
      capture_active_ = false;
    }

    if (timestamps_)
    {
      list_->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame_res_ + 1);
      list_->ResolveQueryData(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame_res_, 2, timestamp_readback_.Get(), 16 * frame_res_);
      timestamp_pending_[frame_res_] = true;

      if (pass_ts_)
      {
        std::vector<PassMark> &marks = pass_marks_[frame_res_];
        list_->EndQuery(pass_ts_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame_res_ * kPassMarks + uint32_t(marks.size()));
        marks.push_back({0, 0, 0, 4}); // end (closes the last segment; not counted itself)
        list_->ResolveQueryData(pass_ts_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame_res_ * kPassMarks, uint32_t(marks.size()), pass_readback_.Get(), 8 * uint64_t(frame_res_) * kPassMarks);
      }
    }

    list_->Close();
    ID3D12CommandQueue *queue = provider_->GetDirectQueue();
    ID3D12CommandList *lists[] = {list_.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence_.Get(), ++fence_value_);
    frames_[frame_res_].fence_value = fence_value_;

    if (sst::gpu::DumpDeviceRemoved())
      REXLOG_ERROR("[sst-native] the graphics device was lost at frame {} (see the [sst-gpu] lines above)", submitted_ + 1);
    ++submitted_;
    recording_ = false;
    memexport_ranges_.clear();

    if (capturing)
    {
      WaitFence(fence_value_);
      FinishCapture();
      captured_.clear();
    }
    Bisect().Poll();
    // F9 (or --sst_native_capture_frame) captures the next frame
    capture_quiet_ = REXCVAR_GET(sst_native_capture_frame) == -1;

    if ((GetAsyncKeyState(VK_F9) & 1))
    {
      capture_requested_ = true;
      capture_quiet_ = false;
    }
    else if (capture_quiet_ || (REXCVAR_GET(sst_native_capture_frame) > 0 && submitted_ + 1 == uint64_t(REXCVAR_GET(sst_native_capture_frame))))
    {
      capture_requested_ = true;
    }
    // Render-target trace: log the frame that just ended; F10 (or --sst_native_rt_trace) traces the next one
    if (rt_trace_.active)
    {
      TraceClosePass();
      REXLOG_INFO("[rt-trace] ---- frame {} ({} events) ----", rt_trace_.frame, rt_trace_.lines.size());

      for (const std::string &line : rt_trace_.lines)
        REXLOG_INFO("[rt-trace] {}", line);

      REXLOG_INFO("[rt-trace] ---- end of frame {} ----", rt_trace_.frame);
      rt_trace_.active = false;
      rt_trace_.lines.clear();
    }

    if ((GetAsyncKeyState(VK_F10) & 1) || (REXCVAR_GET(sst_native_rt_trace) > 0 && submitted_ + 1 == uint64_t(REXCVAR_GET(sst_native_rt_trace))))
    {
      rt_trace_.active = true;
      rt_trace_.frame = submitted_ + 1;
      rt_trace_.has_pass = false;
      rt_trace_.pass_index = 0;
      rt_trace_.lines.clear();
    }

    CollectMemexports(false);
    memexport_written_prev_.swap(memexport_written_);
    clip_occurrence_.clear();
    memexport_written_.clear();

    {
      std::lock_guard lock(slot_mutex_);
      latest_ = int(target_);
      output_seq_[target_] = ++latest_seq_;
    }

    frame_cv_.notify_all();
    // Frame pacing: wall time between Swaps
    const auto now = std::chrono::steady_clock::now();

    if (prof_.last_frame.time_since_epoch().count())
    {
      const double ms = std::chrono::duration<double, std::milli>(now - prof_.last_frame).count();
      prof_.frame_ms_sum += ms;
      prof_.frame_ms_max = std::max(prof_.frame_ms_max, ms);
      ++prof_.frames;

      if (ms > 17.5)
        ++prof_.over_60;

      if (ms > 20.0)
        ++prof_.over_20;

      if (ms > 34.0)
        ++prof_.over_30;
    }

    prof_.last_frame = now;

    if (textures_)
      textures_->ClearSnapshots(); // their memory (the frame's arena) is recycled after this
  }

  ID3D12Resource *Renderer::Acquire(uint32_t *slot, uint32_t *width, uint32_t *height)
  {
    std::unique_lock lock(slot_mutex_);

    if (latest_ < 0)
      return nullptr;
    // Threaded: every frame is shown once, in order. The render thread finishes frame N-1 around the game's swap N; when it is a little late, wait for it. If it is later than that, the last frame is shown again and the late one next time - the queue then holds a frame in reserve, which absorbs the next late frame instead of dropping it. More than 2 waiting: catch up. VIDEO > FPS LIMIT 30: every other display frame repeats the last one; the others show the newest frame
    if (fps30_.load(std::memory_order_relaxed) && acquired_slot_ >= 0)
    {
      if (fps30_acquires_++ & 1)
      {
        ++acquires_;
        ++acquire_repeats_;
        pinned_ = acquired_slot_;
        *slot = uint32_t(acquired_slot_);
        *width = PxU(width_, output_scale_[acquired_slot_].x);
        *height = PxU(height_, output_scale_[acquired_slot_].y);
        return outputs_[acquired_slot_].Get();
      }
    }

    const bool queued = threaded_ && REXCVAR_GET(sst_native_present_queue) && !fps30_.load(std::memory_order_relaxed);
    uint64_t want = latest_seq_;

    if (queued && acquired_seq_)
    {
      want = acquired_seq_ + 1;

      if (latest_seq_ < want)
      {
        const int wait_ms = REXCVAR_GET(sst_native_present_wait_ms);

        if (wait_ms > 0)
          frame_cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), [&]
                             { return latest_seq_ >= want; });
      }

      if (latest_seq_ > acquired_seq_ + 2)
        want = latest_seq_ - 1;
    }

    int pick = -1;

    if (latest_seq_ >= want)
    {
      for (uint32_t i = 0; i < kOutputs; ++i)
        if (output_seq_[i] >= want && output_seq_[i] <= latest_seq_ && (pick < 0 || output_seq_[i] < output_seq_[pick]))
          pick = int(i);
    }

    ++acquires_;

    if (pick < 0)
    {
      // Nothing new: show the last frame again
      ++acquire_repeats_;
      pick = acquired_slot_ >= 0 ? acquired_slot_ : latest_;
    }
    else
    {
      if (acquired_seq_ && output_seq_[pick] > acquired_seq_ + 1)
        acquire_skips_ += output_seq_[pick] - acquired_seq_ - 1;

      acquired_seq_ = output_seq_[pick];
      acquired_slot_ = pick;
      shown_frames_.fetch_add(1, std::memory_order_relaxed);
    }

    pinned_ = pick;
    *slot = uint32_t(pick);
    *width = PxU(width_, output_scale_[pick].x);
    *height = PxU(height_, output_scale_[pick].y);
    return outputs_[pick].Get();
  }

  void Renderer::Release(uint32_t slot)
  {
    std::lock_guard lock(slot_mutex_);

    if (pinned_ == int(slot))
      pinned_ = -1;
  }

  // clear

  void Renderer::Clear(const uint8_t *base, uint32_t device, uint32_t flags, uint32_t color, float z, uint32_t stencil)
  {
    PhaseClock clk(prof_, kPhClear);

    if (rt_trace_.active)
    {
      TraceState(base, device);
      rt_trace_.lines.push_back(fmt::format("  clear flags {:X} colour {:08X} z {} stencil {}", flags, color, z, stencil));
    }

    ++stats_.clears;
    ++stats_.clear_flags[flags];

    if (!recording_)
      BeginFrame();

    const uint32_t rt0 = be32(base, device + kDevRT0);

    if (RenderTargetsEnabled())
    {
      // Xbox D3DCLEAR_TARGET0-3 = 0x1-0x8, ZBUFFER 0x10, STENCIL 0x20 (Clear drops 0xF0 when no depth surface is set). The area is viewport (+ scissor), which PA_SC_WINDOW_SCISSOR holds.
      const uint32_t tl = be32(base, device + kDevScissorTL), br = be32(base, device + kDevScissorBR);
      const LONG x0 = LONG(tl & 0x7FFF), y0 = LONG((tl >> 16) & 0x7FFF);
      const LONG x1 = LONG(br & 0x7FFF), y1 = LONG((br >> 16) & 0x7FFF);
      uint32_t vw = 0, vh = 0;

      if (Reg(base, device, PA_CL_VTE_CNTL) & 1)
      {
        const float xs = std::fabs(RegF(base, device, PA_CL_VPORT_XSCALE));
        const float ys = std::fabs(RegF(base, device, PA_CL_VPORT_YSCALE));
        const float xo = RegF(base, device, PA_CL_VPORT_XOFFSET);
        const float yo = RegF(base, device, PA_CL_VPORT_YOFFSET);

        if (std::isfinite(xs + ys + xo + yo) && xs < 8192 && ys < 8192)
        {
          vw = uint32_t(std::max(0.0f, std::ceil(xo + xs)));
          vh = uint32_t(std::max(0.0f, std::ceil(yo + ys)));
        }
      }

      const uint32_t clear_ds = be32(base, device + kDevDepth);
      const Scale2 rs = SurfaceScale(base, device, rt0, clear_ds);
      const uint32_t samples = (scene_surfaces_.count(rt0) || scene_surfaces_.count(clear_ds)) ? msaa_ : 1;
      auto area = [&](const HostRT &rt)
      {
        D3D12_RECT r = {0, 0, LONG(rt.width), LONG(rt.height)};

        if (x1 > x0 && y1 > y0)
          r = {std::min(x0, r.right), std::min(y0, r.bottom), std::min(x1, r.right), std::min(y1, r.bottom)};

        return Scaled(r, rt.scale);
      };

      if ((flags & 0xF) && rt0)
      {
        if (HostRT *c = GetHostRT(base, rt0, false, rs, samples, vw, vh))
        {
          SetRTState(*c, D3D12_RESOURCE_STATE_RENDER_TARGET);
          float col[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f, float(color & 0xFF) / 255.0f, float((color >> 24) & 0xFF) / 255.0f};

          if (bloom_off_ && BloomSurface(*c, rt0))
            col[0] = col[1] = col[2] = col[3] = 0;

          const D3D12_RECT r = area(*c);

          if (r.right > r.left && r.bottom > r.top)
            list_->ClearRenderTargetView(RtRtv(c->view), col, 1, &r);
        }
      }

      const uint32_t ds = be32(base, device + kDevDepth);

      if ((flags & 0x30) && ds)
      {
        if (HostRT *d = GetHostRT(base, ds, true, rs, samples, vw, vh))
        {
          SetRTState(*d, D3D12_RESOURCE_STATE_DEPTH_WRITE);
          D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(0);

          if (flags & 0x10)
            cf |= D3D12_CLEAR_FLAG_DEPTH;

          if (flags & 0x20)
            cf |= D3D12_CLEAR_FLAG_STENCIL;

          const D3D12_RECT r = area(*d);

          if (r.right > r.left && r.bottom > r.top)
            list_->ClearDepthStencilView(RtDsv(d->view), cf, std::clamp(z, 0.0f, 1.0f), UINT8(stencil), 1, &r);
        }
      }

      return;
    }

    if (stats_.clear_logged++ < 12)
      REXLOG_INFO("[sst-native] clear flags {:X} colour {:08X} z {} stencil {} RT0 {:08X} [{:08X}]", flags, color, z, stencil, rt0, rt0 ? be32(base, rt0 + 0x24) : 0);

    if (rt0)
    {
      // In auto mode the viewport isn't reliable at clear time (it is often still the previous target's), so use what draws taught us about this render target, and the viewport only for targets not drawn to yet
      bool selected;

      if (REXCVAR_GET(sst_native_rt) == "auto" && main_rts_.count(rt0))
        selected = true;
      else if (REXCVAR_GET(sst_native_rt) == "auto" && other_rts_.count(rt0))
        selected = false;
      else
        selected = RenderTargetSelected(base, device, be32(base, rt0 + 0x24));

      if (!selected)
        return;
    }
    // Xbox D3DCLEAR_TARGET0..3 = 0x10..0x80 (the D3D clear helper masks 0xF0 out before handling depth / stencil); the low bits are depth / stencil
    if ((flags & 0xF0) && rt0)
    {
      const float c[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f, float(color & 0xFF) / 255.0f, float((color >> 24) & 0xFF) / 255.0f};
      list_->ClearRenderTargetView(Rtv(target_), c, 0, nullptr);
    }

    D3D12_CLEAR_FLAGS ds = D3D12_CLEAR_FLAGS(0);

    if (flags & 0x3)
      ds |= D3D12_CLEAR_FLAG_DEPTH;

    if (flags & 0x4)
      ds |= D3D12_CLEAR_FLAG_STENCIL;

    if (ds & D3D12_CLEAR_FLAG_DEPTH)
      last_depth_clear_ = std::clamp(z, 0.0f, 1.0f);

    if (ds)
      list_->ClearDepthStencilView(dsv_heap_->GetCPUDescriptorHandleForHeapStart(), ds, std::clamp(z, 0.0f, 1.0f), UINT8(stencil), 0, nullptr);
  }

  // pipelines

  uint64_t Renderer::DxilHash(const ShaderVariant *v)
  {
    if (!v)
      return 0;

    auto [it, fresh] = dxil_hashes_.try_emplace(v, 0);

    if (fresh)
      it->second = XXH3_64bits(v->dxil.data(), v->dxil.size());

    return it->second;
  }

  Renderer::Renderer() = default;
  Renderer::~Renderer() = default;

  void Renderer::FinishPipeline(uint64_t hash, PipelineJob &job, const PipelineKey &key)
  {
    if (job.state.load(std::memory_order_acquire) == 1)
    {
      ++stats_.pipelines_created;

      if (job.from_library)
        ++stats_.pipelines_from_cache;

      stats_.pipeline_ms += job.ms;
      pipelines_.emplace(hash, std::move(job.pso));
      return;
    }

    failed_pipelines_.insert(hash);
    ++stats_.pipelines_failed;

    if (stats_.pipelines_failed <= 10)
    {
      std::string inputs;

      for (const auto &e : job.layout)
        inputs += fmt::format(" {}{}:{}@{}+{}", e.SemanticName, e.SemanticIndex, int(e.Format), e.InputSlot, e.AlignedByteOffset);

      REXLOG_WARN("[sst-native] pipeline creation failed ({:08X}): blend {:08X} topology {} "
                  "inputs{}",
                  uint32_t(job.hr), key.blend, key.topology_type, inputs);
    }
  }

  // urgent: a UI-like draw (no depth test, few vertices). Skipping it while its pipeline compiles makes menus and HUD blink out, and on slow
  // CPUs (one background compile thread) a pipeline can wait behind a court's worth of others for seconds. These pipelines are simple:
  // they are built (or waited for) right away instead.
  ID3D12PipelineState *Renderer::GetPipeline(const PipelineKey &key, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout, const ShaderVariant *vs, const ShaderVariant *ps, bool urgent)
  {
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));

    if (auto it = pipelines_.find(hash); it != pipelines_.end())
      return it->second.Get();

    if (failed_pipelines_.count(hash))
      return nullptr;

    if (auto it = pending_pipelines_.find(hash); it != pending_pipelines_.end())
    {
      if (urgent && it->second->state.load(std::memory_order_acquire) == 0)
      {
        // Already queued for an earlier, non-urgent draw: wait for it (bounded)
        const auto t0 = std::chrono::steady_clock::now();

        while (it->second->state.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
          std::this_thread::sleep_for(std::chrono::milliseconds(1));

        ++stats_.pipelines_urgent;
        stats_.pipeline_urgent_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      }

      if (it->second->state.load(std::memory_order_acquire) == 0)
      {
        ++stats_.skip_pipeline_pending;
        return nullptr;
      }

      FinishPipeline(hash, *it->second, key);
      pending_pipelines_.erase(it);
      auto done = pipelines_.find(hash);
      return done != pipelines_.end() ? done->second.Get() : nullptr;
    }

    if (!pipeline_workers_)
    {
      const uint32_t threads = std::clamp(uint32_t(std::thread::hardware_concurrency()) / 4, 1u, 4u);
      pipeline_workers_ = std::make_unique<PipelineWorkers>(device_, threads, REXCVAR_GET(sst_native_pipeline_library), shaders_ && shaders_->fast());
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = root_signature_.Get();
    d.VS = {vs->dxil.data(), vs->dxil.size()};

    if (ps)
      d.PS = {ps->dxil.data(), ps->dxil.size()}; // null: depth-only

    const uint32_t b = key.blend;
    D3D12_RENDER_TARGET_BLEND_DESC &rt = d.BlendState.RenderTarget[0];
    rt.SrcBlend = BlendFactor(b & 0x1F, false);
    rt.BlendOp = BlendOp((b >> 5) & 7);
    rt.DestBlend = BlendFactor((b >> 8) & 0x1F, false);
    rt.SrcBlendAlpha = BlendFactor((b >> 16) & 0x1F, true);
    rt.BlendOpAlpha = BlendOp((b >> 21) & 7);
    rt.DestBlendAlpha = BlendFactor((b >> 24) & 0x1F, true);
    rt.BlendEnable = !(rt.SrcBlend == D3D12_BLEND_ONE && rt.DestBlend == D3D12_BLEND_ZERO && rt.BlendOp == D3D12_BLEND_OP_ADD && rt.SrcBlendAlpha == D3D12_BLEND_ONE && rt.DestBlendAlpha == D3D12_BLEND_ZERO && rt.BlendOpAlpha == D3D12_BLEND_OP_ADD);
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = key.color_mask;
    d.SampleMask = UINT_MAX;

    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = key.cull == 1 ? D3D12_CULL_MODE_FRONT : key.cull == 2 ? D3D12_CULL_MODE_BACK
                                                                                       : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = key.front_ccw;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.RasterizerState.DepthBias = key.depth_bias;
    d.RasterizerState.SlopeScaledDepthBias = key.slope_bias;

    d.DepthStencilState.DepthEnable = key.depth_enable;
    d.DepthStencilState.DepthWriteMask = key.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(key.depth_func + 1); // xenos 0-7

    d.InputLayout = {layout.data(), UINT(layout.size())};
    d.IBStripCutValue = key.strip_cut == 1 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF : key.strip_cut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                                                                            : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology_type);
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = key.rtv_format ? DXGI_FORMAT(key.rtv_format) : D3D12Presenter::kGuestOutputFormat;
    const uint32_t dsv_kind = key.dsv & 0xF;
    d.DSVFormat = dsv_kind == 0 ? kDepthFormat : dsv_kind == 2 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT
                                                               : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1u << (key.dsv >> 4);

    auto job = std::make_shared<PipelineJob>();
    job->hash = hash;
    job->layout = layout;
    job->desc = d;
    // Library name: everything in the key, with the shaders by content (the variant pointers differ between runs)
    {
      PipelineKey stable = key;
      stable.vs = nullptr;
      stable.ps = nullptr;
      const uint64_t parts[3] = {XXH3_64bits(&stable, sizeof(stable)), DxilHash(vs), DxilHash(ps)};
      const std::string name = fmt::format("sst{:016X}", XXH3_64bits(parts, sizeof(parts)));
      job->name.assign(name.begin(), name.end());
    }

    if (REXCVAR_GET(sst_native_pipeline_async) && !urgent)
    {
      if (pipeline_workers_->TryLoad(*job))
      {
        FinishPipeline(hash, *job, key);
        auto done = pipelines_.find(hash);
        return done != pipelines_.end() ? done->second.Get() : nullptr;
      }

      pending_pipelines_.emplace(hash, job);
      pipeline_workers_->Submit(job);
      ++stats_.skip_pipeline_pending;
      return nullptr;
    }

    pipeline_workers_->Build(*job);

    if (urgent)
    {
      ++stats_.pipelines_urgent;
      stats_.pipeline_urgent_ms += job->ms;
    }

    FinishPipeline(hash, *job, key);
    auto done = pipelines_.find(hash);
    return done != pipelines_.end() ? done->second.Get() : nullptr;
  }

  // draw

  ShaderEntry *Renderer::ResolveShader(const uint8_t *base, uint32_t object, bool vertex)
  {
    if (!object)
      return nullptr;

    if (!PlausibleAddress(object))
      return shaders_->FindObject(object);
    // The object's copy of the container's virtual part identifies the shader. Objects are reused by address: if a new shader was created at the same address by a path the Create hooks don't see, the cached binding would run the old shader (seen as GPU / CPU disagreements and spikes)
    const uint32_t vcopy = object + (vertex ? 0x368 : 0x28);
    const uint32_t vsize = be32(base, vcopy + 4);
    // The microcode can change under an unchanged header (variants sharing their constant / input layout): when the object's microcode pointer changes, compare the live microcode with the bound entry's (skipping the vertex fetches D3D patches at bind)
    const uint32_t ucode_ptr = be32(base, object + (vertex ? 0x20 : 0x18));
    // Already validated this frame with the same microcode: skip re-hashing the object (shader objects aren't re-created mid-frame)
    if (auto it = resolved_objects_.find(object); it != resolved_objects_.end() && it->second.frame == submitted_ && it->second.ucode == ucode_ptr && it->second.entry && shaders_->FindObject(object) == it->second.entry)
      return it->second.entry;

    ShaderEntry *resolved = ResolveShaderSlow(base, object, vertex, vcopy, vsize, ucode_ptr);
    resolved_objects_[object] = {submitted_, ucode_ptr, resolved};
    return resolved;
  }

  ShaderEntry *Renderer::ResolveShaderSlow(const uint8_t *base, uint32_t object, bool vertex, uint32_t vcopy, uint32_t vsize, uint32_t ucode_ptr)
  {
    const uint64_t vhash = vsize && vsize < 0x10000 ? XXH3_64bits(GuestPtr(base, vcopy), vsize) : 0;
    auto microcode_matches = [&](const ShaderEntry *e) -> bool
    {
      const std::vector<uint8_t> &ct = e->container;

      if (ct.size() < 0x20)
        return false;

      auto ct32 = [&](size_t at) -> uint32_t
      {
        return at + 4 <= ct.size() ? (uint32_t(ct[at]) << 24) | (uint32_t(ct[at + 1]) << 16) | (uint32_t(ct[at + 2]) << 8) | ct[at + 3] : 0;
      };

      const uint32_t vsz = ct32(4);
      const uint32_t shader_off = ct32(0x18);
      const uint32_t phys_off = ct32(shader_off), bytes = ct32(size_t(shader_off) + 4);
      const uint32_t live = ToVirtual(ucode_ptr);

      if (!PlausibleAddress(live) || !bytes || size_t(vsz) + phys_off + bytes > ct.size())
        return false;

      const uint8_t *a = ct.data() + vsz + phys_off;
      const uint8_t *b = GuestPtr(base, live + (vertex ? e->ucode_offset : phys_off));
      std::vector<bool> skip(bytes / 12 + 1, false);

      for (const auto &in : e->vertex_inputs)
        if (in.address < skip.size())
          skip[in.address] = true;

      for (uint32_t i = 0; i * 12 + 12 <= bytes; ++i)
        if (!skip[i] && std::memcmp(a + 12 * i, b + 12 * i, 12) != 0)
          return false;

      return true;
    };

    if (ShaderEntry *e = shaders_->FindObject(object))
    {
      auto it = object_vhash_.find(object);

      if (it == object_vhash_.end())
      {
        object_vhash_.emplace(object, vhash); // first sighting of this binding
        object_ucode_[object] = ucode_ptr;
        return e;
      }

      if (it->second == vhash)
      {
        uint32_t &known = object_ucode_[object];

        if (known == ucode_ptr)
          return e;

        known = ucode_ptr;

        if (microcode_matches(e))
          return e;

        ++stats_.shader_rebinds;

        if (stats_.shader_rebinds_logged++ < 20)
          REXLOG_WARN("[sst-native] {} shader object {:08X}: same header but different microcode "
                      "(was {:016X}); rebuilding it from the object",
                      vertex ? "vertex" : "pixel", object, e->hash);
        // Fall through to the rebuild
      }
      else
      {
        // Re-created through a hooked Create call: the binding is already new
        if (vsize <= e->container.size() && XXH3_64bits(e->container.data(), vsize) == vhash)
        {
          it->second = vhash;
          object_ucode_[object] = ucode_ptr;
          return e;
        }

        ++stats_.shader_rebinds;

        if (stats_.shader_rebinds_logged++ < 20)
          REXLOG_WARN("[sst-native] {} shader object {:08X} now holds a different shader (was "
                      "{:016X}); rebuilding it from the object",
                      vertex ? "vertex" : "pixel", object, e->hash);
        it->second = vhash;
        object_ucode_[object] = ucode_ptr;
        // Fall through: rebuild from the object (or find the matching container)
      }
    }
    else
    {
      object_vhash_[object] = vhash;
      object_ucode_[object] = ucode_ptr;
    }
    // Object layout (CreateVertexShader / CreatePixelShader): a copy of the container's virtual part at +0x368 (VS) / +0x28 (PS), and the microcode (physical part) pointer at +0x20 (VS) / +0x18 (PS)
    const uint32_t copy = object + (vertex ? 0x368 : 0x28);
    const uint32_t flags = be32(base, copy);
    const uint32_t virtual_size = be32(base, copy + 4), physical_size = be32(base, copy + 8);
    const uint32_t ucode = ToVirtual(be32(base, object + (vertex ? 0x20 : 0x18)));

    if ((flags & 0xFFFFFF00u) != 0x102A1100u || bool(flags & 1) != vertex || !virtual_size || virtual_size + physical_size > (1u << 20) || !PlausibleAddress(ucode))
    {
      if (LogOnce(0xA000000000000000ull | object))
        REXLOG_WARN("[sst-native] {} shader object {:08X} not recognised (flags {:08X})", vertex ? "vertex" : "pixel", object, flags);

      return nullptr;
    }

    std::vector<uint8_t> container(size_t(virtual_size) + physical_size);
    std::memcpy(container.data(), GuestPtr(base, copy), virtual_size);
    std::memcpy(container.data() + virtual_size, GuestPtr(base, ucode), physical_size);
    ShaderEntry *e = shaders_->Register(container.data(), uint32_t(container.size()));
    shaders_->BindObject(object, e);
    ++stats_.shaders_from_objects;
    return e;
  }

  // Everything the CPU interpreter needs to run the bound vertex shader: the live microcode (vertex fetches patched at bind) and the VS state from the device mirror plus the shader's own literal constants. Returns why it failed, or nullptr.
  const char *Renderer::LoadVsState(const uint8_t *base, uint32_t dev, uint32_t vs_obj, const ShaderEntry *vs_entry, std::vector<uint32_t> &ucode, float *constants, uint32_t bools[8], uint32_t fetch[192], uint32_t loops[32], uint32_t *literals_out)
  {
    const std::vector<uint8_t> &ct = vs_entry->container;
    auto ct32 = [&](size_t at) -> uint32_t
    {
      return at + 4 <= ct.size() ? (uint32_t(ct[at]) << 24) | (uint32_t(ct[at + 1]) << 16) | (uint32_t(ct[at + 2]) << 8) | ct[at + 3] : 0;
    };

    const uint32_t ucode_bytes = ct32(size_t(ct32(0x18)) + 4);
    const uint32_t live_ucode = ToVirtual(be32(base, vs_obj + 0x20));

    if (!PlausibleAddress(live_ucode) || !ucode_bytes || ucode_bytes > 0x10000 || (ucode_bytes % 12))
      return "microcode not found";

    const uint32_t dwords = ucode_bytes / 4;
    ucode.resize(dwords);
    const uint8_t *uc = GuestPtr(base, live_ucode + vs_entry->ucode_offset);

    for (uint32_t i = 0; i < dwords; ++i)
    {
      uint32_t v;
      std::memcpy(&v, uc + 4 * i, 4);
      ucode[i] = __builtin_bswap32(v);
    }

    // Constants from the device mirror (big-endian)
    {
      const uint8_t *p = GuestPtr(base, dev + kDevVSConstants);

      for (uint32_t i = 0; i < 1024; ++i)
      {
        uint32_t v;
        std::memcpy(&v, p + 4 * i, 4);
        v = __builtin_bswap32(v);
        std::memcpy(&constants[i], &v, 4);
      }

      for (uint32_t i = 0; i < 8; ++i)
        bools[i] = be32(base, dev + kDevBools + 4 * i);

      for (uint32_t i = 0; i < 192; ++i)
        fetch[i] = be32(base, dev + kDevFetchConstants + 4 * i);
    }
    // Literal constants defined by the shader itself (HLSL `def`, e.g. c255 = (0, 1, 3, 0) in both morph shaders): D3D loads them into the constant file when the shader is bound, so the device's mirror doesn't have them. Container definition table: float4 entries {u16 register, u16 float count, u32 offset into the physical part}, 0-terminated; then int4 (loop constant) entries {u16 register (8992 + 4 * loop), u16 count, count x u32}, 0-terminated
    std::memset(loops, 0, 32 * sizeof(uint32_t));
    uint32_t literals = 0;
    const uint32_t virtual_size = ct32(4);
    const uint32_t table = ct32(0x14);

    if (table && table + 0x14 <= ct.size())
    {
      size_t at = size_t(table) + 0x14; // DefinitionTable::definitions
      for (; at + 8 <= ct.size() && ct32(at); at += 8)
      {
        const uint32_t w = ct32(at);
        const uint32_t reg = w >> 16, count = w & 0xFFFF;
        const size_t src = size_t(virtual_size) + ct32(at + 4);

        for (uint32_t i = 0; i < (count + 3) / 4; ++i)
        {
          if (reg + i >= 256 || src + 16 * (i + 1) > ct.size())
            break;

          for (uint32_t j = 0; j < 4; ++j)
          {
            const uint32_t v = ct32(src + 16 * i + 4 * j);
            std::memcpy(&constants[4 * (reg + i) + j], &v, 4);
          }

          ++literals;
        }
      }

      at += 4; // terminator
      for (; at + 4 <= ct.size() && ct32(at);)
      {
        const uint32_t w = ct32(at);
        const uint32_t reg = w >> 16, count = w & 0xFFFF;

        for (uint32_t i = 0; i < count && at + 8 + 4 * i <= ct.size(); ++i)
        {
          const uint32_t loop = (reg >= 8992 ? (reg - 8992) / 4 : 32) + i;

          if (loop < 32)
            loops[loop] = ct32(at + 4 + 4 * i);
        }

        at += 4 + 4 * size_t(count);
      }
    }

    if (literals_out)
      *literals_out = literals;

    return nullptr;
  }

  // Debug (--sst_native_check_vs): runs the draw's vertex shader on the CPU from the original microcode and looks for vertices far outside the view. Keys (shader, first vertex buffer) that are only *sometimes* extreme point at spikes; they are reported every status interval, the first few draws in detail.
  Renderer::DrawExtent Renderer::MeasureDraw(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, uint32_t first_vertex, uint32_t vertex_count)
  {
    DrawExtent e;
    // (Big batches too: the star background at 32:9 batches ~1200 vertices into one draw; unmeasured, it would not be fitted and would show stretched. 3D draws stop after a 4-vertex probe and big flat batches are sampled, so this stays cheap.)
    if (call.up_data || !vertex_count || vertex_count > 65536 || !vs_entry)
      return e;

    uint32_t bools[8], fetch[192], loops[32];
    check_constants_.resize(1024);

    if (LoadVsState(base, call.device, vs_obj, vs_entry, check_ucode_, check_constants_.data(), bools, fetch, loops, nullptr))
      return e;

    VsConstants vc;
    vc.float_constants = check_constants_.data();
    vc.bool_constants = bools;
    vc.loop_constants = loops;
    vc.fetch_constants = fetch;
    MemexportStats ignored;
    bool any = false, flat = true;
    float x0 = FLT_MAX, x1 = -FLT_MAX, y0 = FLT_MAX, y1 = -FLT_MAX;
    // Runs the shader for check_indices_ and folds the positions into the extent
    auto run = [&]() -> bool
    {
      const uint32_t n = uint32_t(check_indices_.size());
      check_positions_.resize(size_t(n) * 4);

      if (!memexport_->RunPositions(check_ucode_.data(), uint32_t(check_ucode_.size()), vc, check_indices_.data(), n, check_positions_.data(), &ignored))
        return false;

      for (uint32_t v = 0; v < n; ++v)
      {
        const float *p = &check_positions_[4 * size_t(v)];

        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[3]) || p[3] <= 1e-6f)
          continue;

        any = true;

        if (std::fabs(p[3] - 1.0f) > 1e-3f)
          flat = false;

        const float x = p[0] / p[3], y = p[1] / p[3];
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
      }

      return true;
    };

    // A few vertices first: most draws measured here are 3D (blended effects, particles - no depth writes), and one projected vertex (w != 1) already rules out fitting; only flat (2D) draws are measured in full
    constexpr uint32_t kProbe = 4;
    check_indices_.clear();

    for (uint32_t i = 0; i < std::min(vertex_count, kProbe); ++i)
      check_indices_.push_back(first_vertex + i);

    if (!run())
      return e;

    ++stats_.measured_draws;

    if (any && !flat)
    {
      ++stats_.measured_3d_early;
      e = {true, false, x0, x1, y0, y1};
      return e;
    }

    if (vertex_count > kProbe)
    {
      // Big flat batches (the star fields: up to ~1200 vertices) are sampled: whole quads (4 vertices) spread over the batch, the first and last included
      constexpr uint32_t kMaxMeasured = 512;
      check_indices_.clear();

      if (vertex_count <= kMaxMeasured)
      {
        for (uint32_t i = kProbe; i < vertex_count; ++i)
          check_indices_.push_back(first_vertex + i);
      }
      else
      {
        const uint32_t groups = (vertex_count + 3) / 4, want = kMaxMeasured / 4;

        for (uint32_t g = 0; g < want; ++g)
        {
          const uint32_t q = uint32_t((uint64_t(g) * (groups - 1)) / (want - 1));

          for (uint32_t k = 0; k < 4; ++k)
            if (q * 4 + k < vertex_count && q * 4 + k >= kProbe)
              check_indices_.push_back(first_vertex + q * 4 + k);
        }

        ++stats_.measured_sampled;
      }

      if (!check_indices_.empty() && !run())
        return e;
    }

    if (!any)
      return e;

    e = {true, flat, x0, x1, y0, y1};
    return e;
  }

  void Renderer::CheckVsPositions(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, uint32_t first_vertex, uint32_t vertex_count, uint32_t stream0, bool reads_morph, const float *host, uint32_t host_inputs)
  {
    if (call.up_data || vertex_count > 65536)
      return;

    uint32_t bools[8], fetch[192], loops[32];
    check_constants_.resize(1024);

    if (LoadVsState(base, call.device, vs_obj, vs_entry, check_ucode_, check_constants_.data(), bools, fetch, loops, nullptr))
      return;

    check_indices_.resize(vertex_count);

    for (uint32_t i = 0; i < vertex_count; ++i)
      check_indices_[i] = first_vertex + i;

    check_positions_.resize(size_t(vertex_count) * 4);
    VsConstants vc;
    vc.float_constants = check_constants_.data();
    vc.bool_constants = bools;
    vc.loop_constants = loops;
    vc.fetch_constants = fetch;
    MemexportStats ignored;

    if (!memexport_->RunPositions(check_ucode_.data(), uint32_t(check_ucode_.size()), vc, check_indices_.data(), vertex_count, check_positions_.data(), &ignored))
      return;

    uint32_t extreme = 0, first = 0, exported = 0;

    for (uint32_t v = 0; v < vertex_count; ++v)
    {
      const float *p = &check_positions_[4 * size_t(v)];

      if (std::isnan(p[0]) && std::isnan(p[1]) && std::isnan(p[2]) && std::isnan(p[3]))
        continue;

      ++exported;
      const bool bad = !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || !std::isfinite(p[3]) || std::max({std::fabs(p[0]), std::fabs(p[1]), std::fabs(p[2])}) > 1.0e6f || (p[3] > 1.0e-3f && (std::fabs(p[0]) > 64.0f * p[3] || std::fabs(p[1]) > 64.0f * p[3]));

      if (bad && !extreme++)
        first = v;
    }

    if (!exported)
      return;
    // On-screen jumps: compare with this mesh's previous frame. A vertex that moves far on screen while the rest of the mesh barely moves is a spike (camera cuts move everything, so they don't count)
    {
      // (The same mesh can be drawn several times a frame - shadow, main pass: compare like with like through the occurrence number.)
      const uint64_t base_key = vs_entry->hash ^ (uint64_t(stream0) << 24) ^ (uint64_t(first_vertex) << 52) ^ vertex_count;
      const uint64_t hkey = base_key ^ (uint64_t(clip_occurrence_[base_key]++) << 40);
      ClipHistory &h = clip_history_[hkey];
      std::vector<float> ndc(size_t(vertex_count) * 2, std::nanf(""));

      for (uint32_t v = 0; v < vertex_count; ++v)
      {
        const float *p = &check_positions_[4 * size_t(v)];

        if (std::isfinite(p[3]) && p[3] > 1e-3f)
        {
          ndc[2 * v] = p[0] / p[3];
          ndc[2 * v + 1] = p[1] / p[3];
        }
      }

      if (h.ndc.size() == ndc.size() && submitted_ - h.frame == 1)
      {
        std::vector<float> moves;
        moves.reserve(vertex_count);

        for (uint32_t v = 0; v < vertex_count; ++v)
        {
          const float dx = ndc[2 * v] - h.ndc[2 * v], dy = ndc[2 * v + 1] - h.ndc[2 * v + 1];

          if (std::isfinite(dx) && std::isfinite(dy))
            moves.push_back(std::sqrt(dx * dx + dy * dy));
        }

        if (!moves.empty())
        {
          ++stats_.check_jump_checks;
          std::vector<float> sorted = moves;
          std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
          const float median = sorted[sorted.size() / 2];
          const float limit = std::max(0.5f, 20.0f * median);
          uint32_t jumps = 0, first_jump = 0;

          for (uint32_t v = 0; v < vertex_count; ++v)
          {
            const float dx = ndc[2 * v] - h.ndc[2 * v], dy = ndc[2 * v + 1] - h.ndc[2 * v + 1];

            if (std::isfinite(dx) && std::isfinite(dy) && std::sqrt(dx * dx + dy * dy) > limit)
              if (!jumps++)
                first_jump = v;
          }

          if (jumps)
          {
            ++stats_.check_jump_draws;

            if (stats_.check_jump_logged < 30)
            {
              ++stats_.check_jump_logged;
              const float *p = &check_positions_[4 * size_t(first_jump)];
              std::string inputs;

              for (uint32_t i = 0; i < std::min<uint32_t>(host_inputs, 6); ++i)
              {
                const float *in = host + (size_t(first_jump) * host_inputs + i) * 4;
                uint32_t b[4];
                std::memcpy(b, in, 16);
                inputs += fmt::format(" in{}=({} {} {} {} | {:08X} {:08X} {:08X} {:08X})", i, in[0], in[1], in[2], in[3], b[0], b[1], b[2], b[3]);
              }

              REXLOG_WARN("[sst-native] vertex jump (CPU-run shader): VS {:016X} buffer {:08X}{} "
                          "draw {} vertices from {}: {} vertices jumped (median move {:.4f}); "
                          "vertex {} screen ({:.3f} {:.3f}) -> ({:.3f} {:.3f}), clip ({} {} {} {});{}",
                          vs_entry->hash, stream0, reads_morph ? " (face)" : "", vertex_count, first_vertex, jumps, median, first_vertex + first_jump, h.ndc[2 * first_jump], h.ndc[2 * first_jump + 1], ndc[2 * first_jump], ndc[2 * first_jump + 1], p[0], p[1], p[2], p[3], inputs);
            }
          }
        }
      }

      h.ndc.swap(ndc);
      h.frame = submitted_;

      if (clip_history_.size() > 8192)
        clip_history_.clear();
    }

    const uint64_t key = vs_entry->hash ^ (uint64_t(stream0) << 24);
    CheckEntry &e = check_entries_[key];
    e.vs = vs_entry->hash;
    e.stream = stream0;
    e.prim = call.prim;
    e.morph |= reads_morph;
    ++e.checked;
    ++stats_.check_draws;

    if (!extreme)
      return;

    ++e.flagged;
    ++stats_.check_flagged;
    // Detail: only once a key has also been seen sane (i.e. intermittent)
    if (e.flagged < e.checked && stats_.check_logged < 40)
    {
      ++stats_.check_logged;
      const float *p = &check_positions_[4 * size_t(first)];
      std::string inputs;

      for (uint32_t i = 0; i < std::min<uint32_t>(host_inputs, 4); ++i)
      {
        const float *h = host + (size_t(first) * host_inputs + i) * 4;
        uint32_t b[4];
        std::memcpy(b, h, 16);
        inputs += fmt::format(" in{}=({} {} {} {} | {:08X} {:08X} {:08X} {:08X})", i, h[0], h[1], h[2], h[3], b[0], b[1], b[2], b[3]);
      }

      REXLOG_WARN("[sst-native] spike? VS {:016X} prim {} {} draw {} vertices from {} (buffer "
                  "{:08X}{}): {} of {} vertices far outside the view, first vertex {} at clip "
                  "({} {} {} {});{} [key seen {} times, {} flagged]",
                  vs_entry->hash, call.prim, call.indexed ? "indexed" : "auto", vertex_count, first_vertex, stream0, reads_morph ? ", morph output" : "", extreme, exported, first_vertex + first, p[0], p[1], p[2], p[3], inputs, e.checked, e.flagged);
    }
  }

  // Morph consumers (faces): compare each vertex position with the same draw's previous frame. Faces move a little per frame; a vertex that suddenly jumps far compared with the mesh size is a spike. Logs what it jumped to and when the morph output it reads was last written.
  void Renderer::CheckMorphJumps(const ShaderEntry *vs_entry, const DrawCall &call, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout, const uint8_t *host, uint32_t host_inputs, uint32_t first_vertex, uint32_t vertex_count, uint32_t stream0)
  {
    // Was the morph output changed between the morph and this draw?
    if (auto it = morph_writes_.find(stream0); it != morph_writes_.end() && it->second.hash && it->second.frame == submitted_)
    {
      const uint64_t now = XXH3_64bits(physical_membase_ + stream0, it->second.size);

      if (now != it->second.hash)
      {
        ++stats_.morph_modified;

        if (stats_.morph_modified_logged++ < 10)
          REXLOG_WARN("[sst-native] morph output {:08X} ({} bytes) changed between the morph ({} "
                      "draws ago) and the draw reading it (VS {:016X})",
                      stream0, it->second.size, draw_seq_ - it->second.draw, vs_entry->hash);
      }
    }

    int pos = -1;

    for (const auto &d : layout)
      if (d.InputSlot == 0 && d.SemanticName && std::strcmp(d.SemanticName, "POSITION") == 0 && d.SemanticIndex == 0)
        pos = int(d.AlignedByteOffset / 16);

    if (pos < 0 || !host_inputs)
      return;

    const uint64_t key = vs_entry->hash ^ (uint64_t(stream0) << 24) ^ (uint64_t(first_vertex) << 52) ^ vertex_count;
    MorphHistory &h = morph_history_[key];
    std::vector<float> cur(size_t(vertex_count) * 3);
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (uint32_t v = 0; v < vertex_count; ++v)
    {
      const float *p = reinterpret_cast<const float *>(host) + (size_t(v) * host_inputs + pos) * 4;

      for (int j = 0; j < 3; ++j)
      {
        cur[3 * v + j] = p[j];
        lo[j] = std::min(lo[j], p[j]);
        hi[j] = std::max(hi[j], p[j]);
      }
    }

    const float extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-3f});

    if (h.positions.size() == cur.size() && submitted_ - h.frame <= 1)
    {
      uint32_t jumps = 0, first = 0;
      float worst = 0;

      for (uint32_t v = 0; v < vertex_count; ++v)
      {
        const float dx = cur[3 * v] - h.positions[3 * v], dy = cur[3 * v + 1] - h.positions[3 * v + 1], dz = cur[3 * v + 2] - h.positions[3 * v + 2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (d > 0.25f * h.extent)
        {
          if (!jumps++)
            first = v;

          worst = std::max(worst, d);
        }
      }

      ++stats_.morph_jump_checks;

      if (jumps)
      {
        ++stats_.morph_jump_draws;

        if (stats_.morph_jump_logged < 30)
        {
          ++stats_.morph_jump_logged;
          std::string writes;
          auto it = morph_writes_.find(stream0);

          if (it != morph_writes_.end())
            writes = fmt::format("output last written frame {} ({} frames ago), {} draws ago{}", it->second.frame, submitted_ - it->second.frame, draw_seq_ - it->second.draw, it->second.cached ? " (cached)" : "");
          else
            writes = "output never written by a morph";

          const float *a = &h.positions[3 * first];
          const float *b = &cur[3 * first];
          REXLOG_WARN("[sst-native] morph jump: VS {:016X} buffer {:08X} draw {} vertices from {}: "
                      "{} vertices moved > 25% of the mesh size {:.2f} (worst {:.2f}); vertex {} "
                      "({:.3f} {:.3f} {:.3f}) -> ({:.3f} {:.3f} {:.3f}); previous draw of this "
                      "mesh {} frames / {} draws ago; {}",
                      vs_entry->hash, stream0, vertex_count, first_vertex, jumps, h.extent, worst, first_vertex + first, a[0], a[1], a[2], b[0], b[1], b[2], submitted_ - h.frame, draw_seq_ - h.draw, writes);
        }
      }
    }

    h.positions.swap(cur);
    h.extent = extent;
    h.frame = submitted_;
    h.draw = draw_seq_;
    (void)call;
  }

  // Element indices for shaders that fetch with a computed index: run the bound vertex shader on the CPU over the draw's vertices and record what each full vfetch fetched (fetch_records_, by instruction address)
  bool Renderer::ComputeFetchRecords(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, uint32_t first_vertex, uint32_t vertex_count)
  {
    uint32_t bools[8], fetch[192], loops[32];
    check_constants_.resize(1024);

    if (LoadVsState(base, call.device, vs_obj, vs_entry, check_ucode_, check_constants_.data(), bools, fetch, loops, nullptr))
      return false;

    check_indices_.resize(vertex_count);

    for (uint32_t i = 0; i < vertex_count; ++i)
      check_indices_[i] = first_vertex + i;

    VsConstants vc;
    vc.float_constants = check_constants_.data();
    vc.bool_constants = bools;
    vc.loop_constants = loops;
    vc.fetch_constants = fetch;

    if (call.up_data)
    {
      // DrawVerticesUP / immediate mode: vertex fetch slot 95 is the caller's data (often in virtual memory, so it's passed as a host pointer).
      vc.override_data = GuestPtr(base, call.up_data);
      vc.override_size = call.up_stride * (call.start + call.count);
      vc.override_slot = 95;
    }

    return memexport_->RunFetchIndices(check_ucode_.data(), uint32_t(check_ucode_.size()), vc, check_indices_.data(), vertex_count, &fetch_records_);
  }

  void Renderer::RecordMemexportRanges(const uint8_t *base, uint32_t dev)
  {
    // Stream constants are VS float constants with fixed marker bits (xe_gpu_memexport_stream_t): dword0 bits 30-31 = 1, dword1 = 0x4B000000, dword2 bits 20-31 = 0x4B0, dword3 bits 23-31 = 0x96
    for (uint32_t c = 0; c < 256; ++c)
    {
      const uint32_t a = dev + kDevVSConstants + 16 * c;
      const uint32_t d0 = be32(base, a), d1 = be32(base, a + 4), d2 = be32(base, a + 8), d3 = be32(base, a + 12);

      if ((d0 >> 30) != 1 || d1 != 0x4B000000u || (d2 >> 20) != 0x4B0 || (d3 >> 23) != 0x96)
        continue;

      const uint32_t begin = (d0 & 0x3FFFFFFF) * 4;
      const uint32_t count = d3 & 0x7FFFFF;
      memexport_ranges_.push_back({begin, begin + count * 16}); // <= 16 bytes/element
    }
  }

  // On the console the GPU runs these shaders and writes their eM# exports to memory; the visible mesh draw then fetches those vertices. Here the shader is interpreted on the CPU (memexport.h) with the device's current state, writing guest memory directly before the dependent draw is recorded.
  void Renderer::RunMemexport(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry)
  {
    ++stats_.memexport_draws;
    const uint32_t dev = call.device;
    auto fail = [&](const char *why)
    {
      ++stats_.memexport_failed;
      RecordMemexportRanges(base, dev);

      if (LogOnce(0xC400000000000000ull ^ vs_entry->hash))
        REXLOG_WARN("[sst-native] memexport VS {:016X} not run: {}", vs_entry->hash, why);
    };

    if (!REXCVAR_GET(sst_native_memexport))
    {
      RecordMemexportRanges(base, dev);
      return;
    }

    if (call.up_data)
      return fail("DrawVerticesUP");

    if (call.count > kMaxDrawVertices)
      return fail("too many vertices");

    memexport_constants_.resize(256 * 4);
    uint32_t bools[8], fetch[192], loops[32], literals = 0;

    if (const char *why = LoadVsState(base, dev, vs_obj, vs_entry, memexport_ucode_, memexport_constants_.data(), bools, fetch, loops, &literals))
      return fail(why);

    const uint32_t dwords = uint32_t(memexport_ucode_.size());

    // Vertex indices (r0.x of each invocation)
    memexport_indices_.resize(call.count);

    if (call.indexed)
    {
      const uint32_t ib = be32(base, dev + kDevIndices);

      if (!ib || !GuestReadable(ib, 0x1C))
        return fail("no index buffer");

      const uint32_t common = be32(base, ib);
      const bool index32 = (common >> 31) & 1;
      const uint32_t index_endian = (common >> 29) & 3;
      const uint32_t ib_addr = ToVirtual(be32(base, ib + 0x18) & ~3u);

      if (!PlausibleAddress(ib_addr))
        return fail("bad index buffer");

      const uint32_t isz = index32 ? 4 : 2;
      const uint32_t first_byte = call.start * isz;
      const uint32_t aligned_begin = first_byte & ~3u;
      const uint32_t aligned_end = (first_byte + call.count * isz + 3) & ~3u;

      if (!GuestReadable(ib_addr + aligned_begin, aligned_end - aligned_begin))
        return fail("index buffer memory gone");
      std::vector<uint8_t> raw(aligned_end - aligned_begin);
      SwapVertexData(index_endian, GuestPtr(base, ib_addr + aligned_begin), raw.data(), raw.size());
      const uint8_t *src = raw.data() + (first_byte - aligned_begin);

      for (uint32_t i = 0; i < call.count; ++i)
      {
        uint32_t v = 0;
        std::memcpy(&v, src + isz * i, isz);
        memexport_indices_[i] = (v + call.base_vertex) & 0xFFFFFF;
      }
    }
    else
    {
      for (uint32_t i = 0; i < call.count; ++i)
        memexport_indices_[i] = call.start + i;
    }

    // Output streams (for the cache and the log): stream constants in c0-c255
    struct Stream
    {
      uint32_t begin, size, format;
    };

    Stream streams[4];
    uint32_t stream_count = 0;

    for (uint32_t c = 0; c < 256 && stream_count < 4; ++c)
    {
      uint32_t d[4];
      std::memcpy(d, &memexport_constants_[4 * c], 16);

      if ((d[0] >> 30) != 1 || d[1] != 0x4B000000u || (d[2] >> 20) != 0x4B0 || (d[3] >> 23) != 0x96)
        continue;

      const uint32_t format = (d[2] >> 8) & 0x3F;
      const uint32_t element = MemexportElementSize(format);
      streams[stream_count++] = {(d[0] << 2) & 0x1FFFFFFFu, (d[3] & 0x7FFFFF) * (element ? element : 16), format};
    }

    // Earlier background jobs this one depends on: its output, or inputs that are another morph's output
    if (!pending_memexports_.empty())
    {
      for (uint32_t i = 0; i < stream_count; ++i)
        WaitMemexports(streams[i].begin, streams[i].begin + streams[i].size);

      uint32_t slots[16];
      const uint32_t nslots = VertexFetchSlots(memexport_ucode_.data(), dwords, slots, 16);

      for (uint32_t i = 0; i < nslots; ++i)
      {
        const uint32_t phys = fetch[2 * slots[i]] & 0x1FFFFFFCu;
        const uint32_t size = fetch[2 * slots[i] + 1] & 0x03FFFFFCu;
        WaitMemexports(phys, phys + size);
      }
    }

    // Cache (checked by the job, off the render thread): identical shader, constants, indices, input vertex data and an untouched previous output -> nothing to do
    const bool use_cache = REXCVAR_GET(sst_native_memexport_cache) && stream_count == 1 && streams[0].size && uint64_t(streams[0].begin) + streams[0].size <= 0x20000000u;
    const uint64_t key = use_cache ? vs_entry->hash ^ (uint64_t(streams[0].begin) << 20) : 0;
    const uint64_t input_hash = 0;

    auto job = std::make_shared<MemexportJob>();
    job->ucode = memexport_ucode_;
    job->constants = memexport_constants_;
    std::memcpy(job->bools, bools, sizeof(bools));
    std::memcpy(job->loops, loops, sizeof(loops));
    std::memcpy(job->fetch, fetch, sizeof(fetch));
    job->indices.assign(memexport_indices_.begin(), memexport_indices_.begin() + call.count);

    if (key)
    {
      job->use_cache = true;
      job->hash_begin = streams[0].begin;
      job->hash_size = streams[0].size;

      if (auto it = memexport_cache_.find(key); it != memexport_cache_.end() && it->second.output_begin == streams[0].begin && it->second.output_size == streams[0].size)
      {
        job->has_previous = true;
        job->previous_input_hash = it->second.input_hash;
        job->previous_output_hash = it->second.output_hash;
      }
    }

    PendingMemexport pending;
    pending.job = job;
    pending.cache_key = key;
    pending.input_hash = input_hash;
    pending.vs_hash = vs_entry->hash;
    pending.count = call.count;
    pending.indexed = call.indexed;
    pending.literals = literals;
    pending.stream_count = stream_count;

    for (uint32_t i = 0; i < stream_count; ++i)
    {
      pending.begin[i] = streams[i].begin;
      pending.size[i] = streams[i].size;
      pending.format[i] = streams[i].format;
      // Consumers this frame read these ranges (checks in Draw)
      memexport_written_.push_back({streams[i].begin, streams[i].begin + streams[i].size});
    }

    // --sst_native_memexport_dump: the first 3 runs of each shader, run here (synchronously) so the output can be saved with the inputs
    if (const std::string dir = REXCVAR_GET(sst_native_memexport_dump); !dir.empty() && memexport_dumped_[vs_entry->hash] < 3)
    {
      const uint32_t n = memexport_dumped_[vs_entry->hash]++;
      std::vector<uint8_t> file;
      auto put = [&](const void *data, size_t size)
      {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        file.insert(file.end(), p, p + size);
      };
      auto put32 = [&](uint32_t v)
      { put(&v, 4); };
      // Format (little-endian): "SSTMX1\0\0", vs hash, dwords, ucode, 1024 floats, bools[8], loops[32], fetch[192], index count, indices,
      // input range count, {phys, size, bytes}..., output range count, {phys, size, bytes after the run}...
      put("SSTMX1\0\0", 8);
      put(&vs_entry->hash, 8);
      put32(uint32_t(job->ucode.size()));
      put(job->ucode.data(), job->ucode.size() * 4);
      put(job->constants.data(), 1024 * 4);
      put(job->bools, sizeof(job->bools));
      put(job->loops, sizeof(job->loops));
      put(job->fetch, sizeof(job->fetch));
      put32(uint32_t(job->indices.size()));
      put(job->indices.data(), job->indices.size() * 4);
      uint32_t slots[16];
      const uint32_t nslots = VertexFetchSlots(job->ucode.data(), uint32_t(job->ucode.size()), slots, 16);
      std::vector<std::pair<uint32_t, uint32_t>> inputs;

      for (uint32_t i = 0; i < nslots; ++i)
      {
        const uint32_t phys = job->fetch[2 * slots[i]] & 0x1FFFFFFCu, size = job->fetch[2 * slots[i] + 1] & 0x03FFFFFCu;

        if (size && uint64_t(phys) + size <= 0x20000000u && std::find(inputs.begin(), inputs.end(), std::make_pair(phys, size)) == inputs.end())
          inputs.push_back({phys, size});
      }

      put32(uint32_t(inputs.size()));

      for (const auto &[phys, size] : inputs)
      {
        put32(phys);
        put32(size);
        put(physical_membase_ + phys, size);
      }

      job->use_cache = false; // always run
      memexport_->Execute(*job);
      put32(stream_count);

      for (uint32_t i = 0; i < stream_count; ++i)
      {
        const uint32_t size = uint64_t(streams[i].begin) + streams[i].size <= 0x20000000u ? streams[i].size : 0;
        put32(streams[i].begin);
        put32(size);
        put(physical_membase_ + streams[i].begin, size);
      }

      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      std::ofstream(std::filesystem::path(dir) / fmt::format("{:016X}_{}.mx", vs_entry->hash, n), std::ios::binary).write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
      REXLOG_INFO("[sst-native] memexport dump: VS {:016X} run {} ({} vertices, {} KB) written to {}", vs_entry->hash, n, job->indices.size(), file.size() >> 10, dir);
      FinishMemexport(pending);
      return;
    }

    if (REXCVAR_GET(sst_native_memexport_async))
    {
      // Runs on the background thread; draws reading the output wait for it.
      pending.ticket = memexport_->Submit(job);
      pending_memexports_.push_back(std::move(pending));
      ++stats_.memexport_async;
      return;
    }

    memexport_->Execute(*job);
    FinishMemexport(pending);
  }

  // Waits for background memexport jobs whose output overlaps [begin, end).
  void Renderer::WaitMemexports(uint32_t begin, uint32_t end)
  {
    if (pending_memexports_.empty())
      return;

    uint64_t need = 0;

    for (const PendingMemexport &p : pending_memexports_)
      for (uint32_t i = 0; i < p.stream_count; ++i)
        if (begin < p.begin[i] + p.size[i] && end > p.begin[i])
          need = std::max(need, p.ticket);

    if (need)
    {
      if (memexport_->finished() < need)
      {
        const auto t0 = std::chrono::steady_clock::now();
        memexport_->Wait(need);
        ++stats_.memexport_waits;
        stats_.memexport_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      }
    }

    CollectMemexports(false);
  }

  // Finishes the jobs that are done (all of them, waiting, with `all`).
  void Renderer::CollectMemexports(bool all)
  {
    if (pending_memexports_.empty())
      return;

    if (all)
      memexport_->Wait(pending_memexports_.back().ticket);

    const uint64_t done = memexport_->finished();
    size_t n = 0;

    while (n < pending_memexports_.size() && pending_memexports_[n].ticket <= done)
      FinishMemexport(pending_memexports_[n++]);

    pending_memexports_.erase(pending_memexports_.begin(), pending_memexports_.begin() + n);
  }

  void Renderer::FinishMemexport(const PendingMemexport &p)
  {
    const MemexportJob &job = *p.job;
    MemexportStats &m = memexport_stats_;
    m.vertices += job.stats.vertices;
    m.elements += job.stats.elements;
    m.bad_address += job.stats.bad_address;
    m.bad_format += job.stats.bad_format;
    m.out_of_range += job.stats.out_of_range;
    m.fetch_oob += job.stats.fetch_oob;
    m.malformed += job.stats.malformed;
    m.per_vertex_draws += job.stats.per_vertex_draws;

    if (job.stats.elements)
    {
      m.last_format = job.stats.last_format;
      m.last_base = job.stats.last_base;
      m.last_count = job.stats.last_count;
    }

    stats_.memexport_ms += job.ms;
    {
      MemexportShaderCost &c = memexport_costs_[p.vs_hash];
      c.ms += job.ms;
      c.vertices += job.skipped ? 0 : job.indices.size();
      ++(job.skipped ? c.cached : c.runs);
    }

    if (job.skipped)
    {
      ++stats_.memexport_cached;
      return;
    }

    ++stats_.memexport_run;
    const uint64_t written = job.stats.elements;

    if (!job.ok || !written)
    {
      ++stats_.memexport_failed;

      if (LogOnce(0xC400000000000000ull ^ p.vs_hash))
        REXLOG_WARN("[sst-native] memexport VS {:016X}: {}", p.vs_hash, !job.ok ? "interpreter refused the shader" : "shader wrote nothing");

      return;
    }

    if (REXCVAR_GET(sst_native_diagnostics))
    {
      for (uint32_t i = 0; i < p.stream_count; ++i)
      {
        const bool hashable = uint64_t(p.begin[i]) + p.size[i] <= 0x20000000u;
        morph_writes_[p.begin[i]] = {submitted_, draw_seq_, false, p.size[i], hashable ? XXH3_64bits(physical_membase_ + p.begin[i], p.size[i]) : 0};
      }

      if (p.stream_count == 1 && p.format[0] >= 36 && p.format[0] <= 38 && uint64_t(p.begin[0]) + p.size[0] <= 0x20000000u)
      {
        // (Values read as 8in32-swapped floats, the endian this game uses.)
        const uint8_t *out = physical_membase_ + p.begin[0];
        uint32_t bad = 0, first_bad = UINT32_MAX;
        float first_value = 0;

        for (uint32_t i = 0; i < p.size[0] / 4; ++i)
        {
          uint32_t v;
          std::memcpy(&v, out + 4 * i, 4);
          v = __builtin_bswap32(v);
          float f;
          std::memcpy(&f, &v, 4);

          if (!std::isfinite(f) || std::fabs(f) > 1.0e5f)
          {
            if (!bad++)
              first_bad = i, first_value = f;
          }
        }

        if (bad)
        {
          ++stats_.memexport_bad_outputs;

          if (stats_.memexport_bad_logged++ < 8)
          {
            std::string weights;

            for (uint32_t c = 1; c <= 4; ++c)
              weights += fmt::format(" ({:.3f} {:.3f} {:.3f} {:.3f})", job.constants[4 * c], job.constants[4 * c + 1], job.constants[4 * c + 2], job.constants[4 * c + 3]);

            REXLOG_WARN("[sst-native] memexport VS {:016X} at {:08X}: {} implausible values of {} "
                        "(first: float {} = {}); vertices {}; c1-c4{}",
                        p.vs_hash, p.begin[0], bad, p.size[0] / 4, first_bad, first_value, p.count, weights);
          }
        }
      }
    }

    if (p.cache_key && job.input_hash)
    {
      MemexportCacheEntry &e = memexport_cache_[p.cache_key];
      e.input_hash = job.input_hash;
      e.output_begin = p.begin[0];
      e.output_size = p.size[0];
      e.output_hash = job.output_hash;
    }

    if (LogOnce(0xC500000000000000ull ^ p.vs_hash) || LogOnce(0xC600000000000000ull ^ (uint64_t(p.begin[0]) << 8)))
      REXLOG_INFO("[sst-native] memexport VS {:016X}: {} vertices ({}) -> {} elements, format {} "
                  "at {:08X} ({} streams, {} literal constants, c255 {} {} {} {}); {:.3f} ms;{}",
                  p.vs_hash, p.count, p.indexed ? "indexed" : "auto", written, job.stats.last_format, job.stats.last_base, p.stream_count, p.literals, job.constants[1020], job.constants[1021], job.constants[1022], job.constants[1023], job.ms, DescribeMicrocode(job.ucode.data(), uint32_t(job.ucode.size())));
  }

  void Renderer::Draw(const uint8_t *base, const DrawCall &call)
  {
    PhaseClock clk(prof_, kPhOther);
    ++stats_.draws;
    ++draw_seq_;
    ++stats_.prims[call.prim];

    if (!call.count)
      return;

    const uint32_t dev = call.device;

    // Memory-export (morph) passes: no rendering, they write vertices a later draw reads. Run them whatever the render target.
    const uint32_t vs_obj = be32(base, dev + kDevVS), ps_obj = be32(base, dev + kDevPS);
    clk.Switch(kPhShaders);
    ShaderEntry *vs_entry = ResolveShader(base, vs_obj, true);
    clk.Switch(kPhOther);

    if (rt_trace_.active)
      TraceDraw(base, dev, call, vs_entry && vs_entry->memexport);

    if (vs_entry && vs_entry->memexport)
    {
      guest_watch::ConstantUse(guest_watch::ShaderKey(vs_obj, be32(base, vs_obj + 0x20)), 4096); // the stream constants are found by scanning c0-c255
      clk.Switch(kPhMemexport);
      RunMemexport(base, call, vs_obj, vs_entry);
      return;
    }

    // Render target
    const uint32_t rt0 = be32(base, dev + kDevRT0);

    if (!rt0)
    {
      ++stats_.skip_rt;
      return;
    }

    const uint32_t rt_size = be32(base, rt0 + 0x24);
    ++stats_.render_targets[(uint64_t(rt0) << 32) | rt_size];

    if (LogOnce(0x9000000000000000ull | rt0))
      REXLOG_INFO("[sst-native] render target {:08X}: size word {:08X} (~{}x{}), depth {:08X}, "
                  "viewport scale {} {} {} offset {} {} {}, VTE {:08X}",
                  rt0, rt_size, (rt_size >> 18) + 1, ((rt_size >> 3) & 0x7FFF) + 1, be32(base, dev + kDevDepth), RegF(base, dev, PA_CL_VPORT_XSCALE), RegF(base, dev, PA_CL_VPORT_YSCALE), RegF(base, dev, PA_CL_VPORT_ZSCALE), RegF(base, dev, PA_CL_VPORT_XOFFSET), RegF(base, dev, PA_CL_VPORT_YOFFSET), RegF(base, dev, PA_CL_VPORT_ZOFFSET), Reg(base, dev, PA_CL_VTE_CNTL));
    const bool rt_mode = RenderTargetsEnabled();
    const bool rt_selected = rt_mode || RenderTargetSelected(base, dev, rt_size);

    if (REXCVAR_GET(sst_native_rt) == "auto")
      (rt_selected ? main_rts_ : other_rts_).insert(rt0);

    if (!rt_selected)
    {
      ++stats_.skip_rt;
      return;
    }

    // Primitive type
    D3D_PRIMITIVE_TOPOLOGY topology;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
    bool rect_list = false;
    bool convert = false; // TRIFAN / QUADLIST -> indexed triangle list
    switch (call.prim)
    {
    case 1:
      topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      break;

    case 2:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;

    case 3:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;

    case 4:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;

    case 6:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;

    case 5: // TRIFAN

    case 0xD: // QUADLIST
      if (!REXCVAR_GET(sst_native_quads))
      {
        ++stats_.skip_prim;
        return;
      }

      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      convert = true;
      break;

    case 8: // RECTLIST: 3 corners per rectangle, 4th added on the CPU
      if (!rt_mode)
      {
        ++stats_.skip_prim;
        return;
      }

      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      rect_list = true;
      break;

    default: // others: later
      ++stats_.skip_prim;
      return;
    }

    // Shaders. Objects the Create*Shader hooks didn't see are rebuilt from the object itself (container copy + microcode).
    clk.Switch(kPhShaders);
    ShaderEntry *ps_entry = ps_obj ? ResolveShader(base, ps_obj, false) : nullptr;
    clk.Switch(kPhOther);

    if (!vs_entry || vs_entry->memexport || (ps_obj && !ps_entry))
    {
      ++stats_.skip_shader;

      if (stats_.skip_shader_logged++ < 8)
        REXLOG_INFO("[sst-native] skipped draw: VS {:08X} ({}) PS {:08X} ({}) prim {} count {}", vs_obj, !vs_entry ? "unknown" : vs_entry->memexport ? "memexport"
                                                                                                                                                     : "ok",
                    ps_obj, !ps_obj ? "none" : ps_entry ? "ok"
                                                        : "unknown",
                    call.prim, call.count);

      return;
    }

    // Debug: --sst_native_trace_vs / --sst_native_skip_vs
    {
      auto parse = [](const std::string &text) -> uint64_t
      {
        return text.empty() ? 0 : std::strtoull(text.c_str(), nullptr, 16);
      };
      static std::string trace_text, skip_text;
      static uint64_t trace_hash = 0, skip_hash = 0;

      if (const std::string t = REXCVAR_GET(sst_native_trace_vs); t != trace_text)
        trace_text = t, trace_hash = parse(t);

      if (const std::string t = REXCVAR_GET(sst_native_skip_vs); t != skip_text)
        skip_text = t, skip_hash = parse(t);

      if (skip_hash && vs_entry->hash == skip_hash)
      {
        ++stats_.skip_cull;
        return;
      }

      trace_draw_ = trace_hash && vs_entry->hash == trace_hash && traced_draws_ < 40;

      if (trace_draw_)
      {
        ++traced_draws_;
        draw_textures_.clear();
      }
    }

    clk.Switch(kPhInputs);

    // Vertex inputs, from the vertex shader's *live* fetch instructions: the Xbox D3D patches format / offset / stride / fetch constant / swizzle into the microcode when it binds the declaration (draw-time flush), so this is exactly what the GPU would fetch. Each input is decoded on the CPU (DecodeElement) into 4 x 32 bits; swizzle changes the patch made relative to the original instruction become the component order.
    const uint32_t live_ucode = ToVirtual(be32(base, vs_obj + 0x20));

    if (!PlausibleAddress(live_ucode) || vs_entry->container.size() < 16)
    {
      ++stats_.skip_decl;
      return;
    }

    const uint32_t virtual_size = (uint32_t(vs_entry->container[4]) << 24) | (uint32_t(vs_entry->container[5]) << 16) | (uint32_t(vs_entry->container[6]) << 8) | vs_entry->container[7];
    struct FetchStream
    {
      uint32_t slot;   // fetch constant slot (2-dword units)
      uint32_t stride; // bytes
    };

    FetchStream fetch_streams[kMaxStreams];
    uint32_t fetch_stream_count = 0;
    auto stream_index = [&](uint32_t slot, uint32_t stride) -> int
    {
      for (uint32_t i = 0; i < fetch_stream_count; ++i)
        if (fetch_streams[i].slot == slot && fetch_streams[i].stride == stride)
          return int(i);

      if (fetch_stream_count == kMaxStreams)
        return -1;

      fetch_streams[fetch_stream_count] = {slot, stride};
      return int(fetch_stream_count++);
    };

    std::vector<sst::xenos::VertexInput> inputs = vs_entry->vertex_inputs;
    std::sort(inputs.begin(), inputs.end(), [](const auto &x, const auto &y)
              { return x.address < y.address; });
    struct HostElement
    {
      FetchElement fetch;
      int stream;          // -1: reads zeros
      uint32_t fetch_addr; // instruction index of the controlling full vfetch
    };

    // Shaders whose full vfetches don't fetch with the plain vertex index (r0.x) - e.g. particle quads reading one record per 4 vertices - get their element indices from running the shader on the CPU
    bool computed_fetch = false;
    uint32_t full_fetch_addr = 0;
    std::vector<HostElement> host_elements;
    int position_element = -1;  // host element holding POSITION (RECTLIST corner detection)
    std::string capture_detail; // F9 capture: inputs of this draw
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    layout.reserve(inputs.size());
    host_elements.reserve(inputs.size());
    const uint32_t vs_spec = 0; // R11G11B10 normals are decoded on the CPU
    uint64_t layout_hash_input[64];
    uint32_t layout_words = 0;
    int full_stream = -1; // the last full vfetch (mini fetches share its constant)
    // The live fetch instructions. D3D's bind-time patch rewrites whole vfetch instructions - destination register included - and may assign the declaration's elements to the fetch slots in a different order than the container lists them. So each input is matched to the live instruction that writes the same register as the original instruction did, not to the one at the same address
    struct LiveFetch
    {
      uint32_t addr, l0, l1, l2; // live (patched)
      uint32_t o0, o1;           // original (container)
    };

    std::vector<LiveFetch> live_fetches;
    live_fetches.reserve(inputs.size());
    auto container_dword = [&](size_t at) -> uint32_t
    {
      if (at + 4 > vs_entry->container.size())
        return 0;

      const uint8_t *p = vs_entry->container.data() + at;
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    };

    for (const auto &in : inputs)
    {
      const uint32_t at = live_ucode + vs_entry->ucode_offset + 12u * in.address;
      const size_t orig = size_t(virtual_size) + vs_entry->ucode_offset + 12u * in.address;
      live_fetches.push_back({in.address, be32(base, at), be32(base, at + 4), be32(base, at + 8), container_dword(orig), container_dword(orig + 4)});
    }

    auto written_mask = [](uint32_t swizzle_word)
    {
      uint32_t m = 0;

      for (int i = 0; i < 4; ++i)
        if (((swizzle_word >> (3 * i)) & 7) != 7)
          m |= 1u << i;

      return m;
    };

    bool remapped = false;

    for (const auto &in : inputs)
    {
      D3D12_INPUT_ELEMENT_DESC d = {};
      d.SemanticName = UsageSemantic(in.usage);
      d.SemanticIndex = in.usage_index;
      d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

      if (!d.SemanticName)
        continue;

      const LiveFetch *self = nullptr;

      for (const LiveFetch &lf : live_fetches)
        if (lf.addr == in.address)
          self = &lf;

      if (!self)
        continue;

      const uint32_t o1 = self->o1 ? self->o1 : self->l1; // original swizzle
      // Live instruction writing the original destination register
      const LiveFetch *match = self;

      if (self->o0 && (self->o0 & 0x1F) == 0)
      {
        const uint32_t dst = (self->o0 >> 12) & 0x3F, want = written_mask(o1);
        const bool self_ok = (self->l0 & 0x1F) == 0 && ((self->l0 >> 12) & 0x3F) == dst && (written_mask(self->l1) & want);

        if (!self_ok)
        {
          const LiveFetch *best = nullptr;
          int best_score = -1;

          for (const LiveFetch &lf : live_fetches)
          {
            if ((lf.l0 & 0x1F) != 0 || ((lf.l0 >> 12) & 0x3F) != dst)
              continue;

            const int score = __builtin_popcount(written_mask(lf.l1) & want);

            if (score > best_score)
              best = &lf, best_score = score;
          }

          if (best && best != self)
          {
            match = best;
            remapped = true;
          }
        }
      }

      const uint32_t l0 = match->l0, l1 = match->l1, l2 = match->l2;
      // Left unpatched by D3D (format 0, stride 0): laid out from the declaration below
      const bool unpatched = (l0 & 0x1F) == 0 && ((l1 >> 16) & 0x3F) == 0 && (l2 & 0xFF) == 0;
      // Its full fetch (a mini fetch uses the last full fetch before it)
      const LiveFetch *full = nullptr;

      if ((l0 & 0x1F) == 0 && !((l1 >> 30) & 1))
      {
        full = match;
      }
      else
      {
        for (const LiveFetch &lf : live_fetches)
          if (lf.addr < match->addr && (lf.l0 & 0x1F) == 0 && !((lf.l1 >> 30) & 1) && (!full || lf.addr > full->addr))
            full = &lf;
      }

      if (full && !unpatched)
      {
        const uint32_t slot = ((full->l0 >> 20) & 0x1F) * 3 + ((full->l0 >> 25) & 3);
        full_stream = stream_index(slot, (full->l2 & 0xFF) * 4);
        full_fetch_addr = full->addr;
        // Source register bits 5-10 (+ relative bit 11), source swizzle 30-31
        if (((full->l0 >> 5) & 0x7F) != 0 || ((full->l0 >> 30) & 3) != 0)
          computed_fetch = true;
      }
      else
      {
        full_stream = -1;
      }

      HostElement h;
      h.stream = -1;
      h.fetch_addr = 0;
      // A vertex fetch D3D left unpatched (format 0, stride 0 - as compiled): seen on the movie quad (CriMovieControl, DrawVerticesUP). Its layout comes from the bound vertex declaration instead
      bool from_decl = false;

      if (unpatched)
      {
        DeclInput di;

        if (FindDeclInput(base, dev, in.usage, in.usage_index, di))
        {
          FetchElement &f = h.fetch;
          f.format = di.format;
          f.offset = di.offset;
          f.out = !UsageIsUint(in.usage) ? FetchElement::Out::kFloat : in.usage == 2 ? FetchElement::Out::kUint
                                                                                     : FetchElement::Out::kFloatBits;
          // Input component c reads element component c; missing ones read 0 (w: 1), as D3D's own patch fills them
          for (int c = 0; c < 4; ++c)
            f.sel[c] = uint8_t(c < int(di.components) ? c : c == 3 ? 5
                                                                   : 4);

          h.stream = stream_index(95 - di.stream, di.stride);
          h.fetch_addr = in.address;
          from_decl = h.stream >= 0;

          if (from_decl && LogOnce(0xCD00000000000000ull ^ vs_entry->hash ^ in.address))
            REXLOG_INFO("[sst-native] VS {:016X} {}{}: unpatched vertex fetch, layout from the "
                        "vertex declaration (format {}, offset {}, stride {}, stream {})",
                        vs_entry->hash, d.SemanticName, d.SemanticIndex, di.format, di.offset, di.stride, di.stream);
        }
      }

      if (!from_decl && (l0 & 0x1F) == 0)
      {                                               // vertex fetch opcode
        const int32_t offset = int32_t(l2 << 1) >> 9; // bits 8-30, signed, in dwords
        FetchElement &f = h.fetch;
        f.format = (l1 >> 16) & 0x3F;
        f.is_signed = (l1 >> 12) & 1;
        f.normalized = !((l1 >> 13) & 1);
        f.no_zero = (l1 >> 14) & 1;
        f.exp_adjust = int32_t(l1 << 2) >> 26;
        // The shader was translated from the original instruction: destination i reads input component o_i there, and hardware component l_i after the patch, so input component o_i must hold hardware component l_i
        bool conflict = false;
        uint8_t set = 0;

        for (int i = 0; i < 4; ++i)
        {
          const uint32_t o = (o1 >> (3 * i)) & 7, l = (l1 >> (3 * i)) & 7;

          if (o >= 4)
            continue; // the shader doesn't read the input for this destination
          // Patched destination 6 (invalid) / 7 (keep: the register's previous value, zero for a fetch into a fresh register) read as 0
          const uint8_t hw = uint8_t(l >= 6 ? 4 : l);

          if ((set >> o) & 1)
            conflict |= f.sel[o] != hw;

          f.sel[o] = hw;
          set |= uint8_t(1u << o);
        }

        if (conflict && LogOnce(0xE100000000000000ull | (uint64_t(o1) << 24) | (l1 & 0xFFF)))
          REXLOG_WARN("[sst-native] {}{}: conflicting swizzles (original {:03X} patched {:03X})", d.SemanticName, d.SemanticIndex, o1 & 0xFFF, l1 & 0xFFF);

        f.out = !UsageIsUint(in.usage) ? FetchElement::Out::kFloat : in.usage == 2 ? FetchElement::Out::kUint
                                                                                   : FetchElement::Out::kFloatBits;

        if (DeclFormatSize(f.format) && offset >= 0 && full_stream >= 0)
        {
          f.offset = uint32_t(offset) * 4;
          h.stream = full_stream;
          h.fetch_addr = full_fetch_addr;
        }

        if (capture_active_)
          capture_detail += fmt::format(" {}{}[live {:08X} {:08X} {:08X} orig {:08X} fmt {} sel {}{}{}{} "
                                        "off {}]",
                                        d.SemanticName, d.SemanticIndex, l0, l1, l2, o1, f.format, f.sel[0], f.sel[1], f.sel[2], f.sel[3], offset * 4);

        if (LogOnce(0xE200000000000000ull | (uint64_t(in.usage) << 40) | (uint64_t(l1 & 0x3FFFFFFF))))
          REXLOG_INFO("[sst-native] vertex input {}{}: format {} {} {} exp {} swizzle {:03X}->{:03X} "
                      "(sel {}{}{}{}) offset {} stride {}",
                      d.SemanticName, d.SemanticIndex, f.format, f.is_signed ? "signed" : "unsigned", f.normalized ? "norm" : "int", f.exp_adjust, o1 & 0xFFF, l1 & 0xFFF, f.sel[0], f.sel[1], f.sel[2], f.sel[3], offset * 4, (l2 & 0xFF) * 4);
      }

      if (h.stream < 0 && LogOnce(0xE000000000000000ull | (uint64_t(vs_entry->hash & 0xFFFFFFFF) << 16) | in.address))
        REXLOG_WARN("[sst-native] VS {:016X} input {}{} at {}: unusable fetch {:08X} {:08X} {:08X}", vs_entry->hash, d.SemanticName, d.SemanticIndex, in.address, l0, l1, l2);

      d.Format = UsageIsUint(in.usage) ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;

      if (h.stream >= 0)
      {
        d.InputSlot = 0;
        d.AlignedByteOffset = UINT(16 * host_elements.size());

        if (in.usage == 0 && position_element < 0)
          position_element = int(host_elements.size());

        host_elements.push_back(h);
      }
      else
      {
        d.InputSlot = kZeroInputSlot;
        d.AlignedByteOffset = 0;
      }

      layout.push_back(d);

      if (layout_words + 1 < std::size(layout_hash_input))
        layout_hash_input[layout_words++] = (uint64_t(in.usage) << 56) | (uint64_t(in.usage_index) << 48) | (uint64_t(d.Format) << 40) | (uint64_t(d.InputSlot) << 32) | d.AlignedByteOffset;
    }

    if (remapped)
    {
      ++stats_.fetch_remapped;

      if (LogOnce(0xCC00000000000000ull ^ vs_entry->hash))
        REXLOG_INFO("[sst-native] VS {:016X}: the bound declaration moved inputs to other fetch "
                    "slots; inputs matched to the live fetches by destination register",
                    vs_entry->hash);
    }

    clk.Switch(kPhOther);

    // Render state from the device's register mirror
    const uint32_t depthcontrol = Reg(base, dev, RB_DEPTHCONTROL);
    const uint32_t colorcontrol = Reg(base, dev, RB_COLORCONTROL);
    const uint32_t mode = Reg(base, dev, PA_SU_SC_MODE_CNTL);
    // Render targets: host textures for this draw's colour / depth surfaces
    HostRT *color_rt = nullptr;
    HostRT *depth_rt = nullptr;
    uint32_t target_w = width_, target_h = height_;

    if (rt_mode)
    {
      // What the viewport covers (for sizing targets drawn bigger than the surface).
      uint32_t vw = 0, vh = 0;

      if (Reg(base, dev, PA_CL_VTE_CNTL) & 1)
      {
        const float xs = std::fabs(RegF(base, dev, PA_CL_VPORT_XSCALE));
        const float ys = std::fabs(RegF(base, dev, PA_CL_VPORT_YSCALE));
        const float xo = RegF(base, dev, PA_CL_VPORT_XOFFSET), yo = RegF(base, dev, PA_CL_VPORT_YOFFSET);

        if (std::isfinite(xs + ys + xo + yo) && xs < 8192 && ys < 8192)
        {
          vw = uint32_t(std::max(0.0f, std::ceil(xo + xs)));
          vh = uint32_t(std::max(0.0f, std::ceil(yo + ys)));
        }
      }

      const uint32_t ds_surface = be32(base, dev + kDevDepth);
      const Scale2 rs = SurfaceScale(base, dev, rt0, ds_surface);

      if (shadows_off_ && rs == shadow_scale_ && (shadow_surfaces_.count(rt0) || shadow_surfaces_.count(ds_surface)))
      {
        ++stats_.skip_cull; // VIDEO > SHADOWS OFF: nothing drawn into the shadow map
        return;
      }

      const uint32_t samples = (scene_surfaces_.count(rt0) || scene_surfaces_.count(ds_surface)) ? msaa_ : 1;
      color_rt = GetHostRT(base, rt0, false, rs, samples, vw, vh);

      if (!color_rt)
      {
        ++stats_.skip_rt;
        return;
      }

      if (bloom_off_ && BloomSurface(*color_rt, rt0))
      {
        ++stats_.skip_cull; // VIDEO > BLOOM OFF: the glow / bloom targets stay black
        return;
      }

      const uint32_t ds = be32(base, dev + kDevDepth);

      if (ds && (depthcontrol & 3))
      { // stencil or depth test on
        depth_rt = GetHostRT(base, ds, true, rs, samples, vw, vh);

        if (depth_rt && (depth_rt->width != color_rt->width || depth_rt->height != color_rt->height || depth_rt->samples != color_rt->samples))
        {
          if (LogOnce(0xAC00000000000000ull ^ (uint64_t(rt0) << 24) ^ ds))
            REXLOG_INFO("[sst-native] colour target {:08X} ({}x{}) and depth {:08X} ({}x{}) differ "
                        "in size: drawing without depth",
                        rt0, color_rt->width, color_rt->height, ds, depth_rt->width, depth_rt->height);
          depth_rt = nullptr;
        }
      }

      target_w = color_rt->width;
      target_h = color_rt->height;
    }

    const bool has_depth = rt_mode ? depth_rt != nullptr : be32(base, dev + kDevDepth) != 0;
    const Scale2 draw_scale = color_rt ? color_rt->scale : output_scale_[target_];

    uint32_t ps_spec = 0;
    const bool alpha_test = (colorcontrol >> 3) & 1;
    const uint32_t alpha_func = colorcontrol & 7;

    if (alpha_test && alpha_func == 0)
    { // NEVER
      ++stats_.skip_cull;
      return;
    }

    if (alpha_test && alpha_func != 7)
      ps_spec |= sst::xenos::kSpecAlphaTest;

    if ((mode & 3) == 3)
    { // both faces culled
      ++stats_.skip_cull;
      return;
    }

    // Shader variants (from the exe's shader pack, or compiled on worker threads; skip until ready). urgent: a draw waits for them, so
    // they jump the compile queue.
    const ShaderVariant *vs = shaders_->GetVariant(vs_entry, vs_spec, true);
    const ShaderVariant *ps = ps_entry ? shaders_->GetVariant(ps_entry, ps_spec, true) : nullptr;
    const bool debug_shading = REXCVAR_GET(sst_native_debug_shading) && debug_ps_;

    if (!vs || (ps_entry && !ps))
    {
      ++stats_.skip_pending;
      return;
    }

    // Bisect: a hidden draw still runs in full, it just writes no colour (so nothing else changes)
    bool bisect_hide = false;

    if (ps_entry && REXCVAR_GET(sst_native_debug_bisect))
    {
      ShaderBisect &bisect = Bisect();

      if (!bisect.active && bisect.recent.size() < 4096)
        bisect.recent.insert(ps_entry->hash);

      bisect_hide = bisect.Hidden(ps_entry->hash);
    }

    // Pipeline
    PipelineKey key;
    std::memset(&key, 0, sizeof(key));

    if (debug_shading && ps)
      ps = debug_ps_.get();

    key.vs = vs;
    key.ps = ps;
    key.layout_hash = XXH3_64bits(layout_hash_input, layout_words * sizeof(uint64_t));
    key.blend = Reg(base, dev, RB_BLENDCONTROL0);

    if (REXCVAR_GET(sst_native_skip_blended) && ps && (key.blend & 0xFFFF) != 0x0001 && (Reg(base, dev, RB_COLOR_MASK) & 0xF))
    {
      ++stats_.skip_cull;
      return;
    }

    key.color_mask = ps && !bisect_hide ? uint8_t(Reg(base, dev, RB_COLOR_MASK) & 0xF) : 0;

    if (LogOnce(0xD000000000000000ull | (uint64_t(key.color_mask) << 32) | key.blend))
      REXLOG_INFO("[sst-native] new state: colour mask {:X} blend {:08X} depth {:08X} colour "
                  "control {:08X} mode {:08X}",
                  key.color_mask, key.blend, depthcontrol, colorcontrol, mode);
    key.depth_enable = has_depth && ((depthcontrol >> 1) & 1);
    key.depth_write = key.depth_enable && ((depthcontrol >> 2) & 1);
    key.depth_func = key.depth_enable ? uint8_t((depthcontrol >> 4) & 7) : 7;
    key.cull = rect_list ? 0 : uint8_t(mode & 3);
    key.rtv_format = rt_mode ? uint8_t(color_rt->format) : 0;
    key.dsv = rt_mode ? (depth_rt ? 2 : 1) : 0;
    // MSAA: log2(samples) in bits 4-7 (0 without MSAA, so other keys are unchanged)
    if (rt_mode && color_rt->samples > 1)
      key.dsv |= uint8_t(std::countr_zero(color_rt->samples) << 4);

    key.front_ccw = ((mode >> 2) & 1) ? 0 : 1;
    // Polygon offset (the shadow pass's depth bias; without it surfaces shadow themselves in a dotted pattern). PA_SU_SC_MODE_CNTL bits 11-13 enable it for front / back / both; the device keeps PA_SU_POLY_OFFSET_FRONT_SCALE, _FRONT_OFFSET, _BACK_SCALE, _BACK_OFFSET at dev+0x2A50..+0x2A5C. Units as Xenia: scale in 1/16 subpixels, offset in float24 depth ULPs
    if (key.depth_enable && (mode & (7u << 11)))
    {
      const bool front = (mode >> 11) & 1, para = (mode >> 13) & 1;
      const uint32_t at = dev + ((front || para) ? 0x2A50 : 0x2A58);
      float scale, offset;
      const uint32_t sv = be32(base, at), ov = be32(base, at + 4);
      std::memcpy(&scale, &sv, 4);
      std::memcpy(&offset, &ov, 4);

      if (std::isfinite(scale) && std::isfinite(offset))
      {
        int32_t bias = int32_t(std::ceil(std::fabs(offset) * float(1u << 21))) << 3;
        key.depth_bias = offset < 0 ? -bias : bias;
        key.slope_bias = scale * (1.0f / 16.0f);

        if (LogOnce(0xAE00000000000000ull ^ (uint64_t(sv) << 20) ^ ov))
          REXLOG_INFO("[sst-native] polygon offset: scale {} offset {} (mode {:08X}) -> depth bias "
                      "{} slope {}",
                      scale, offset, mode, key.depth_bias, key.slope_bias);
      }
    }

    key.topology_type = uint8_t(topology_type);

    clk.Switch(kPhIndices);

    // (the geometry cache records copies)
    if (!recording_)
      BeginFrame();

    // Indices, decoded with the index buffer's own swap mode: DrawIndexedVertices puts (Common >> 29) & 3 into the index DMA's swap field (0 none, 1 8in16, 2 8in32, 3 16in32), so 16-bit indices are not always 8in16
    std::vector<uint32_t> converted;    // TRIFAN / QUADLIST
    std::vector<uint32_t> rect_indices; // RECTLIST (4 vertices per rectangle)
    std::vector<uint32_t> indices;      // little-endian, as the GPU reads them
    const uint8_t *ib_src = nullptr;    // guest index data (late-write check)
    size_t ib_src_bytes = 0;
    bool index32 = false;
    uint32_t vmin = 0, vmax = 0;
    GeoEntry *ientry = nullptr; // geometry cache entry of the index data
    uint64_t ihash = 0;
    bool index_hit = false;

    if (call.indexed)
    {
      const uint32_t ib = be32(base, dev + kDevIndices);

      if (!ib || call.count > kMaxDrawIndices || !GuestReadable(ib, 0x1C))
      {
        ++stats_.skip_data;
        return;
      }

      const uint32_t common = be32(base, ib);
      index32 = (common >> 31) & 1;
      const uint32_t index_endian = (common >> 29) & 3;
      const uint32_t ib_addr = ToVirtual(be32(base, ib + 0x18) & ~3u);

      if (!PlausibleAddress(ib_addr))
      {
        ++stats_.skip_data;
        return;
      }

      if (LogOnce(0xC100000000000000ull | (index32 ? 4 : 0) | index_endian))
        REXLOG_INFO("[sst-native] index buffer: {}-bit, swap mode {} (Common {:08X})", index32 ? 32 : 16, index_endian, common);
      // Swap whole dwords around the referenced range, then read little-endian
      const uint32_t isz = index32 ? 4 : 2;
      const uint32_t first_byte = call.start * isz;
      const uint32_t aligned_begin = first_byte & ~3u;
      const uint32_t aligned_end = (first_byte + call.count * isz + 3) & ~3u;
      // Freed by the game while this draw waited (a model loading): skip it
      if (!GuestReadable(ib_addr + aligned_begin, aligned_end - aligned_begin))
      {
        ++stats_.skip_data;

        if (LogOnce(0xC200000000000000ull))
          REXLOG_WARN("[sst-native] index data at {:08X} is no longer mapped (freed while the draw was queued): draws like this are skipped", ib_addr + aligned_begin);

        return;
      }

      ib_src = GuestPtr(base, ib_addr + aligned_begin);
      ib_src_bytes = aligned_end - aligned_begin;
      const uint32_t restart = index32 ? 0xFFFFFFFFu : 0xFFFFu;
      const bool strip = call.prim == 6 || call.prim == 3;
      // Geometry cache (see the vertex data step): converted indices and their range stay in video memory while the guest index data is unchanged
      if (!convert && GeoCacheUsable())
      {
        const uint64_t words[5] = {ib_addr, call.start, call.count, (uint64_t(index32) << 8) | (uint64_t(index_endian) << 4) | uint64_t(strip), 0x1D1D1D1Dull};
        ientry = &geo_entries_[XXH3_64bits(words, sizeof(words))];

        if (ientry->dynamic)
        {
          ientry = nullptr;
        }
        else if (GeoTrusted(*ientry))
        {
          ihash = ientry->hash;
          index_hit = true;
          vmin = ientry->vmin;
          vmax = ientry->vmax;
          ++stats_.geo_trusted;
        }
        else
        {
          ihash = XXH3_64bits(ib_src, ib_src_bytes);

          if (ientry->hash == ihash && ientry->generation == geo_generation_ && ientry->size)
          {
            index_hit = true;
            vmin = ientry->vmin;
            vmax = ientry->vmax;
            GeoChecked(*ientry, true);
          }
          else if (ientry->hash && ientry->hash != ihash && ++ientry->changes >= 2)
          {
            ientry->dynamic = true;
            ientry = nullptr;
          }
          else
          {
            GeoChecked(*ientry, false);
          }
        }
      }

      if (!index_hit)
      {
        std::vector<uint8_t> raw(aligned_end - aligned_begin);
        SwapVertexData(index_endian, GuestPtr(base, ib_addr + aligned_begin), raw.data(), raw.size());
        const uint8_t *src = raw.data() + (first_byte - aligned_begin);
        indices.resize(call.count);

        for (uint32_t i = 0; i < call.count; ++i)
        {
          if (index32)
          {
            std::memcpy(&indices[i], src + 4 * i, 4);
          }
          else
          {
            uint16_t v;
            std::memcpy(&v, src + 2 * i, 2);
            indices[i] = v;
          }
        }

        vmin = UINT32_MAX;
        vmax = 0;

        for (uint32_t v : indices)
        {
          if (strip && v == restart)
            continue;

          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
        }
      } // !index_hit
      if (vmin > vmax || vmax - vmin >= kMaxDrawVertices)
      {
        ++stats_.skip_data;
        return;
      }

      if (strip)
        key.strip_cut = index32 ? 2 : 1;
    }
    else
    {
      vmin = 0; // relative to call.start
      vmax = call.count - 1;

      if (call.count > kMaxDrawVertices)
      {
        ++stats_.skip_data;
        return;
      }
    }

    clk.Switch(kPhPipeline);
    const bool ui_like = (!key.depth_enable || key.depth_func == 7) && vmax >= vmin && vmax - vmin < 256; // no depth test (or ALWAYS)
    ID3D12PipelineState *pso = GetPipeline(key, layout, vs, ps, ui_like);

    if (!pso)
    {
      ++stats_.skip_pipeline;
      return;
    }

    if (!recording_)
      BeginFrame();

    clk.Switch(kPhVertices);
    // Immediate-mode data (BeginVertices/EndVertices): the game's particle helper writes one record per sprite but draws 4 quad-list vertices per record, so its shaders must derive the record index from the vertex index in a way the instruction scan above doesn't see (most likely by rewriting r0 before the vfetch). These draws are few and small: always take the element indices from running the shader on the CPU
    if (call.immediate && !host_elements.empty())
      computed_fetch = true;

    // Vertex data: resolve each fetch constant, then decode the referenced vertex range into one host stream (16 bytes per input)
    const uint32_t first_vertex = call.indexed ? call.base_vertex + vmin : call.start;
    const uint32_t vertex_count = vmax - vmin + 1;
    struct ResolvedStream
    {
      const uint8_t *src;
      uint32_t endian;
      uint32_t size;     // bytes
      uint32_t phys = 0; // guest physical address (vertex buffers)
    };

    ResolvedStream resolved[kMaxStreams] = {};
    bool reads_morph = false;

    for (uint32_t i = 0; i < fetch_stream_count; ++i)
    {
      const FetchStream &fs = fetch_streams[i];
      uint32_t size;

      if (call.up_data && fs.slot == 95)
      {
        // DrawVerticesUP: stream 0 data comes from the caller's buffer
        if (!PlausibleAddress(call.up_data) || !GuestReadable(call.up_data, call.up_stride * (call.start + call.count)))
        {
          ++stats_.skip_data;
          return;
        }

        resolved[i] = {GuestPtr(base, call.up_data), 2, 0};
        size = call.up_stride * (call.start + call.count);
      }
      else
      {
        const uint32_t dw0 = be32(base, dev + kDevFetchConstants + 8 * fs.slot);
        const uint32_t dw1 = be32(base, dev + kDevFetchConstants + 8 * fs.slot + 4);
        const uint32_t phys = dw0 & 0x1FFFFFFCu;
        size = dw1 & 0x03FFFFFCu;

        if ((dw0 & 3) != 3 || !phys)
        {
          ++stats_.skip_data;

          if (LogOnce(0xF000000000000000ull | fs.slot))
            REXLOG_WARN("[sst-native] fetch constant {} is not a vertex buffer: {:08X} {:08X}", fs.slot, dw0, dw1);

          return;
        }

        if (!GuestPhysicalReadable(phys, size))
        {
          ++stats_.skip_data;

          if (LogOnce(0xC300000000000000ull))
            REXLOG_WARN("[sst-native] vertex data at {:08X} is no longer mapped (freed while the draw was queued): draws like this are skipped", phys);

          return;
        }

        WaitMemexports(phys, phys + size); // background morph output this draw reads
        for (const auto &[mb, me] : memexport_ranges_)
        {
          if (phys < me && phys + size > mb)
          {
            ++stats_.skip_memexport;
            return;
          }
        }
        // Diagnostics for draws reading morph output: is it this frame's, and does the draw stay inside what the morph wrote?
        {
          const uint64_t read_begin = uint64_t(phys) + uint64_t(first_vertex) * fs.stride;
          const uint64_t read_end = uint64_t(phys) + (uint64_t(first_vertex) + vertex_count) * fs.stride;
          bool overlaps_now = false, inside = false;

          for (const auto &[mb, me] : memexport_written_)
          {
            if (read_begin < me && read_end > mb)
            {
              overlaps_now = true;
              inside |= read_begin >= mb && read_end <= me;
            }
          }

          if (overlaps_now)
          {
            reads_morph = true;
            ++stats_.morph_consumers;

            if (!inside)
            {
              ++stats_.morph_consumer_overrun;

              if (LogOnce(0xC700000000000000ull ^ (uint64_t(phys) << 16) ^ read_end))
                REXLOG_WARN("[sst-native] draw reads past morph output: fetch {:08X} stride {} "
                            "vertices {}+{} (bytes {:08X}-{:08X})",
                            phys, fs.stride, first_vertex, vertex_count, uint32_t(read_begin), uint32_t(read_end));
            }
          }
          else
          {
            for (const auto &[mb, me] : memexport_written_prev_)
            {
              if (read_begin < me && read_end > mb)
              {
                ++stats_.morph_consumer_stale;

                if (LogOnce(0xC800000000000000ull ^ (uint64_t(phys) << 16)))
                  REXLOG_WARN("[sst-native] draw reads morph output written last frame, not "
                              "this one (yet): fetch {:08X} stride {} vertices {}+{}",
                              phys, fs.stride, first_vertex, vertex_count);
                break;
              }
            }
          }
        }

        resolved[i] = {physical_membase_ + phys, dw1 & 3, 0, phys};

        if (LogOnce(0xC200000000000000ull | (dw1 & 3)))
          REXLOG_INFO("[sst-native] vertex buffer swap mode {} (fetch {:08X} {:08X})", dw1 & 3, dw0, dw1);
      }

      resolved[i].size = size;
      const uint64_t end = (uint64_t(first_vertex) + vertex_count) * fs.stride;

      if (!fs.stride || (end > size && !computed_fetch))
      {
        ++stats_.skip_data;

        if (LogOnce(0xB000000000000000ull | fs.slot))
          REXLOG_WARN("[sst-native] draw reads past vertex buffer (fetch {} size {}, vertices {}+{} "
                      "stride {})",
                      fs.slot, size, first_vertex, vertex_count, fs.stride);
        return;
      }

      // The game thread's copy of the start of this buffer from when the draw was issued (UI sprites: the game rewrites their vertices
      // between draws, and read live a frame later they hold another sprite's). Used when it covers every vertex this draw reads.
      // (not for morph output: the renderer writes that itself, after the game thread copied the buffer)
      if (resolved[i].phys && !computed_fetch && !reads_morph)
      {
        for (uint32_t c = 0; c < draw_vertex_copy_count_; ++c)
        {
          const GuestCopy &g = draw_vertex_copies_[c];

          if (g.begin == resolved[i].phys && uint64_t(g.end) - g.begin >= end)
          {
            resolved[i].src = g.data;
            resolved[i].size = std::min(size, g.end - g.begin);
            ++stats_.vertex_copies_used;
            break;
          }
        }
      }
    }

    auto mark_vertices_changed = [&]
    {
      for (uint32_t i = 0; i < fetch_stream_count; ++i)
        if (resolved[i].phys)
          guest_watch::VerticesChanged(resolved[i].phys, uint32_t(std::min<uint64_t>((uint64_t(first_vertex) + vertex_count) * fetch_streams[i].stride, 1u << 20)), submitted_ + 1);
    };

    if (!host_elements.empty())
    {
      const uint32_t host_stride = uint32_t(16 * host_elements.size());
      // Geometry cache: decoded vertices stay in video memory while the guest data they came from is unchanged (checked by hash on every use). Data that keeps changing (morph output, dynamic buffers) is marked dynamic and decoded every time as before
      GeoEntry *ventry = nullptr;
      uint64_t vhash = 0;
      bool vhit = false;

      if (GeoCacheUsable() && !computed_fetch && !call.up_data && !reads_morph && !rect_list)
      {
        uint64_t words[4 * kMaxStreams + 8 * 16 + 4];
        uint32_t n = 0;
        words[n++] = first_vertex;
        words[n++] = vertex_count;
        words[n++] = host_stride;

        for (const HostElement &h : host_elements)
        {
          if (n + 3 > std::size(words))
            break;

          const FetchElement &f = h.fetch;
          words[n++] = (uint64_t(f.format) << 32) | (uint64_t(f.is_signed) << 24) | (uint64_t(f.normalized) << 16) | (uint64_t(f.no_zero) << 8) | uint64_t(uint8_t(f.out));
          words[n++] = (uint64_t(uint32_t(f.exp_adjust)) << 32) | f.offset;
          words[n++] = (uint64_t(f.sel[0]) << 56) | (uint64_t(f.sel[1]) << 48) | (uint64_t(f.sel[2]) << 40) | (uint64_t(f.sel[3]) << 32) | uint32_t(h.stream);
        }

        for (uint32_t i = 0; i < fetch_stream_count; ++i)
        {
          const uint64_t phys = resolved[i].phys;

          if (n + 2 <= std::size(words))
          {
            words[n++] = (phys << 8) | resolved[i].endian;
            words[n++] = fetch_streams[i].stride;
          }
        }

        const uint64_t key = XXH3_64bits(words, n * sizeof(uint64_t));
        ventry = &geo_entries_[key];

        if (ventry->dynamic)
        {
          ventry = nullptr;
          ++stats_.geo_dynamic;
          mark_vertices_changed();
        }
        else if (GeoTrusted(*ventry))
        {
          vhash = ventry->hash;
          vhit = true;
          ++stats_.geo_trusted;
        }
        else
        {
          // One hashing state, reused (creating one per draw allocated)
          static thread_local XXH3_state_t *st = XXH3_createState();
          XXH3_64bits_reset(st);

          for (uint32_t i = 0; i < fetch_stream_count; ++i)
            XXH3_64bits_update(st, resolved[i].src + size_t(first_vertex) * fetch_streams[i].stride, size_t(vertex_count) * fetch_streams[i].stride);

          vhash = XXH3_64bits_digest(st);

          if (ventry->hash == vhash && ventry->generation == geo_generation_ && ventry->size)
          {
            vhit = true;
            GeoChecked(*ventry, true);
          }
          else
          {
            if (ventry->hash && ventry->hash != vhash)
              mark_vertices_changed(); // the game rewrites this data: the game thread copies it with the draws from now on

            if (ventry->hash && ventry->hash != vhash && ++ventry->changes >= 2)
            {
              ventry->dynamic = true; // keeps changing: stop caching it
              ventry = nullptr;
            }
            else
            {
              GeoChecked(*ventry, false);
            }
          }
        }
      }

      if (vhit)
      {
        ++stats_.geo_vertex_hits;
        capture_host_ = nullptr;
        const D3D12_VERTEX_BUFFER_VIEW vbv = {GeoAddress(*ventry), vertex_count * host_stride, host_stride};
        list_->IASetVertexBuffers(0, 1, &vbv);
      }
      else
      {
        const Upload up = Allocate(size_t(vertex_count) * host_stride, 16);

        if (!up.cpu)
        {
          ++stats_.skip_data;
          return;
        }

        uint32_t *out = reinterpret_cast<uint32_t *>(up.cpu);

        if (computed_fetch && ComputeFetchRecords(base, call, vs_obj, vs_entry, first_vertex, vertex_count))
        {
          ++stats_.computed_fetch_draws;

          for (uint32_t v = 0; v < vertex_count; ++v)
          {
            for (const HostElement &h : host_elements)
            {
              const FetchStream &fs = fetch_streams[h.stream];
              const ResolvedStream &rs = resolved[h.stream];
              auto it = fetch_records_.find(h.fetch_addr);
              const int32_t record = it != fetch_records_.end() && v < it->second.size() ? it->second[v] : -1;
              const uint64_t at = uint64_t(record) * fs.stride;

              if (record >= 0 && at + h.fetch.offset + DeclFormatSize(h.fetch.format) <= rs.size)
              {
                DecodeElement(h.fetch, rs.endian, rs.src + at, out);
              }
              else
              {
                out[0] = out[1] = out[2] = out[3] = 0;
              }

              out += 4;
            }
          }

          if (LogOnce(0xCB00000000000000ull ^ vs_entry->hash))
            REXLOG_INFO("[sst-native] VS {:016X} fetches with a computed index (e.g. one record per "
                        "particle quad); element indices taken from running it on the CPU",
                        vs_entry->hash);
        }
        else
        {
          if (computed_fetch && LogOnce(0xCC00000000000000ull ^ vs_entry->hash))
            REXLOG_WARN("[sst-native] VS {:016X}: running it on the CPU for its fetch indices "
                        "failed; vertices fetched in order instead",
                        vs_entry->hash);

          for (uint32_t v = 0; v < vertex_count; ++v)
          {
            for (const HostElement &h : host_elements)
            {
              const FetchStream &fs = fetch_streams[h.stream];
              const ResolvedStream &rs = resolved[h.stream];
              DecodeElement(h.fetch, rs.endian, rs.src + size_t(first_vertex + v) * fs.stride, out);
              out += 4;
            }
          }
        }

        if (reads_morph && REXCVAR_GET(sst_native_diagnostics))
          CheckMorphJumps(vs_entry, call, layout, up.cpu, uint32_t(host_elements.size()), first_vertex, vertex_count, fetch_stream_count ? uint32_t(resolved[0].src - physical_membase_) : 0);

        if (reads_morph && REXCVAR_GET(sst_native_diagnostics))
        {
          const float *f = reinterpret_cast<const float *>(up.cpu);
          const size_t n = size_t(vertex_count) * host_elements.size();
          uint32_t bad = 0;
          size_t first = 0;

          for (size_t i = 0; i < n; ++i)
          {
            if (host_elements[i % host_elements.size()].fetch.out != FetchElement::Out::kFloat)
              continue;

            for (uint32_t j = 0; j < 4; ++j)
              if (!std::isfinite(f[4 * i + j]) || std::fabs(f[4 * i + j]) > 1.0e5f)
              {
                if (!bad++)
                  first = 4 * i + j;
              }
          }

          if (bad)
          {
            ++stats_.morph_consumer_bad;

            if (stats_.morph_consumer_bad_logged++ < 8)
              REXLOG_WARN("[sst-native] morph consumer VS {:016X}: {} implausible input values "
                          "(first at vertex {} input {} = {}), vertices {}+{}",
                          vs_entry->hash, bad, first / (4 * host_elements.size()), (first / 4) % host_elements.size(), f[first], first_vertex, vertex_count);
          }
        }

        if (REXCVAR_GET(sst_native_check_late_writes))
        {
          LateCheck lc;
          lc.vs = vs_entry->hash;
          lc.prim = call.prim;
          lc.count = call.count;
          lc.morph = reads_morph;
          lc.draw = draw_seq_;

          if (ib_src)
            lc.ranges.push_back({ib_src, ib_src_bytes, XXH3_64bits(ib_src, ib_src_bytes), true});

          for (uint32_t i = 0; i < fetch_stream_count; ++i)
          {
            const uint8_t *p = resolved[i].src + size_t(first_vertex) * fetch_streams[i].stride;
            const size_t n = size_t(vertex_count) * fetch_streams[i].stride;
            lc.ranges.push_back({p, n, XXH3_64bits(p, n), false});
          }

          late_checks_.push_back(std::move(lc));
        }

        if (REXCVAR_GET(sst_native_check_vs_all) || (REXCVAR_GET(sst_native_check_vs) && reads_morph))
          CheckVsPositions(base, call, vs_obj, vs_entry, first_vertex, vertex_count, fetch_stream_count ? uint32_t(resolved[0].src - physical_membase_) : 0, reads_morph, reinterpret_cast<const float *>(up.cpu), uint32_t(host_elements.size()));
        // Sample of what the first few indexed 3D draws feed the GPU, to check positions / indices / matrices by eye
        if (call.indexed && stats_.sample_logged < 6 && !host_elements.empty() && !indices.empty())
        {
          ++stats_.sample_logged;
          const float *f = reinterpret_cast<const float *>(up.cpu);
          std::string idx;

          for (uint32_t i = 0; i < std::min<uint32_t>(9, call.count); ++i)
            idx += fmt::format(" {}", indices[i]);

          std::string verts;

          for (uint32_t v = 0; v < std::min<uint32_t>(3, vertex_count); ++v)
          {
            const float *p = f + size_t(v) * host_elements.size() * 4;
            verts += fmt::format(" ({:.3f} {:.3f} {:.3f} {:.3f})", p[0], p[1], p[2], p[3]);
          }

          float c[16];

          for (int i = 0; i < 16; ++i)
          {
            const uint32_t w = be32(base, dev + kDevVSConstants + 4 * i);
            std::memcpy(&c[i], &w, 4);
          }

          REXLOG_INFO("[sst-native] sample draw: VS {:016X} prim {} base {} start {} count {} "
                      "range {}-{}; indices{}; first input of first vertices{}; c0-c3 "
                      "[{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} "
                      "{:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}]",
                      vs_entry->hash, call.prim, call.base_vertex, call.start, call.count, vmin, vmax, idx, verts, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
        }

        if (trace_draw_)
        {
          const float *f = reinterpret_cast<const float *>(up.cpu);
          std::string streams, verts;

          for (uint32_t i = 0; i < fetch_stream_count; ++i)
            streams += fmt::format(" [slot {} phys {:08X} stride {} size {}{}]", fetch_streams[i].slot, resolved[i].phys, fetch_streams[i].stride, resolved[i].size, resolved[i].phys && resolved[i].src != physical_membase_ + resolved[i].phys ? " copy" : "");

          for (uint32_t v = 0; v < std::min<uint32_t>(3, vertex_count); ++v)
          {
            verts += " {";

            for (size_t e = 0; e < std::min<size_t>(host_elements.size(), 8); ++e)
            {
              const float *p = f + (size_t(v) * host_elements.size() + e) * 4;
              verts += fmt::format(" ({:.4g} {:.4g} {:.4g} {:.4g})", p[0], p[1], p[2], p[3]);
            }

            verts += " }";
          }

          // The vertex shader run on the CPU: where the vertices end up (clip space)
          std::string clip = " (CPU run failed)";
          {
            uint32_t bools[8], fetch[192], loops[32];
            check_constants_.resize(1024);

            if (!LoadVsState(base, call.device, vs_obj, vs_entry, check_ucode_, check_constants_.data(), bools, fetch, loops, nullptr) && vertex_count <= 65536)
            {
              check_indices_.resize(vertex_count);

              for (uint32_t v = 0; v < vertex_count; ++v)
                check_indices_[v] = first_vertex + v;

              check_positions_.resize(size_t(vertex_count) * 4);
              VsConstants vc;
              vc.float_constants = check_constants_.data();
              vc.bool_constants = bools;
              vc.loop_constants = loops;
              vc.fetch_constants = fetch;
              MemexportStats ignored;

              if (memexport_->RunPositions(check_ucode_.data(), uint32_t(check_ucode_.size()), vc, check_indices_.data(), vertex_count, check_positions_.data(), &ignored))
              {
                float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, w0 = 1e30f, w1 = -1e30f;
                uint32_t bad = 0;

                for (uint32_t v = 0; v < vertex_count; ++v)
                {
                  const float *p = &check_positions_[4 * size_t(v)];

                  if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[3]) || p[3] <= 1e-6f)
                  {
                    ++bad;
                    continue;
                  }

                  x0 = std::min(x0, p[0] / p[3]), x1 = std::max(x1, p[0] / p[3]);
                  y0 = std::min(y0, p[1] / p[3]), y1 = std::max(y1, p[1] / p[3]);
                  w0 = std::min(w0, p[3]), w1 = std::max(w1, p[3]);
                }

                const float *p = check_positions_.data();
                clip = fmt::format(" screen x {:.3g}..{:.3g} y {:.3g}..{:.3g} w {:.3g}..{:.3g}, {} vertices non-finite or behind; vertex 0 clip ({:.4g} {:.4g} {:.4g} {:.4g}); bone constants c35 ({:.4g} {:.4g} {:.4g} {:.4g})", x0, x1, y0, y1, w0, w1, bad, p[0], p[1], p[2], p[3], check_constants_[35 * 4], check_constants_[35 * 4 + 1], check_constants_[35 * 4 + 2], check_constants_[35 * 4 + 3]);
              }
            }
          }

          REXLOG_INFO("[sst-native] trace VS {:016X} PS {:016X}: frame {} prim {} indexed {} start {} count {} base {} vertices {}+{} (index range {}-{}); streams{}; textures{}; first vertices{}; CPU-run{}", vs_entry->hash, ps_entry ? ps_entry->hash : 0, submitted_ + 1, call.prim, call.indexed, call.start, call.count, call.base_vertex, first_vertex, vertex_count, vmin, vmax, streams, draw_textures_, verts, clip);
        }

        capture_host_ = capture_active_ ? reinterpret_cast<const float *>(up.cpu) : nullptr;
        D3D12_VERTEX_BUFFER_VIEW vbv = {up.gpu, vertex_count * host_stride, host_stride};

        if (rect_list)
        {
          // RECTLIST: each 3 vertices are corners of a rectangle; the GPU makes the 4th. Find the right-angle corner c (by the positions) and add the opposite corner a + b - c (all float inputs, which the full-screen passes' vertex shaders pass through or transform linearly).
          const uint32_t rects = vertex_count / 3;
          const uint32_t floats = host_stride / 4;
          const Upload ex = Allocate(size_t(std::max(rects, 1u)) * 4 * host_stride, 16);

          if (!ex.cpu || !rects)
          {
            ++stats_.skip_data;
            return;
          }

          const float *src = reinterpret_cast<const float *>(up.cpu);
          float *dst = reinterpret_cast<float *>(ex.cpu);
          rect_indices.clear();

          for (uint32_t r = 0; r < rects; ++r)
          {
            const float *v[3] = {src + size_t(3 * r) * floats, src + size_t(3 * r + 1) * floats, src + size_t(3 * r + 2) * floats};
            uint32_t corner = 0;

            if (position_element >= 0)
            {
              float best = FLT_MAX;

              for (uint32_t i = 0; i < 3; ++i)
              {
                const float *p = v[i] + 4 * position_element;
                const float *q1 = v[(i + 1) % 3] + 4 * position_element;
                const float *q2 = v[(i + 2) % 3] + 4 * position_element;
                const float d = std::fabs((q1[0] - p[0]) * (q2[0] - p[0]) + (q1[1] - p[1]) * (q2[1] - p[1]));

                if (d < best)
                  best = d, corner = i;
              }
            }

            const uint32_t a = (corner + 1) % 3, b = (corner + 2) % 3;
            float *out = dst + size_t(4 * r) * floats;
            std::memcpy(out, v[0], 3 * size_t(floats) * 4);
            float *v3 = out + 3 * size_t(floats);

            for (size_t e = 0; e < host_elements.size(); ++e)
            {
              for (uint32_t j = 0; j < 4; ++j)
              {
                const size_t k = 4 * e + j;
                v3[k] = host_elements[e].fetch.out == FetchElement::Out::kUint ? v[corner][k] : v[a][k] + v[b][k] - v[corner][k];
              }
            }

            rect_indices.insert(rect_indices.end(), {4 * r, 4 * r + 1, 4 * r + 2, 4 * r + a, 4 * r + b, 4 * r + 3});
          }

          ++stats_.rects_converted;
          vbv = {ex.gpu, rects * 4 * host_stride, host_stride};
        }

        list_->IASetVertexBuffers(0, 1, &vbv);

        if (ventry)
        {
          ventry->hash = vhash;
          GeoStore(*ventry, up, vertex_count * host_stride);
        }
      }
    }

    const D3D12_VERTEX_BUFFER_VIEW zero_vbv = {zero_buffer_->GetGPUVirtualAddress(), 256, 0};
    list_->IASetVertexBuffers(kZeroInputSlot, 1, &zero_vbv);

    // Index buffer (swapped / converted)
    uint32_t draw_count = call.count;
    bool draw_indexed = call.indexed;

    if (rect_list)
    {
      if (rect_indices.empty() || call.indexed)
      {
        ++stats_.skip_prim;
        return;
      }

      const Upload up = Allocate(rect_indices.size() * 4, 16);

      if (!up.cpu)
        return;

      std::memcpy(up.cpu, rect_indices.data(), rect_indices.size() * 4);
      const D3D12_INDEX_BUFFER_VIEW ibv = {up.gpu, UINT(rect_indices.size() * 4), DXGI_FORMAT_R32_UINT};
      list_->IASetIndexBuffer(&ibv);
      draw_count = uint32_t(rect_indices.size());
      draw_indexed = true;
    }
    else if (convert)
    {
      // Source indices relative to first_vertex
      auto source = [&](uint32_t i) -> uint32_t
      {
        return call.indexed ? indices[i] - vmin : i;
      };

      if (call.prim == 5)
      {
        for (uint32_t i = 2; i < call.count; ++i)
          converted.insert(converted.end(), {source(0), source(i - 1), source(i)});
      }
      else
      {
        for (uint32_t q = 0; q + 3 < call.count; q += 4)
          converted.insert(converted.end(), {source(q), source(q + 1), source(q + 2), source(q), source(q + 2), source(q + 3)});
      }

      if (converted.empty())
        return;

      const Upload up = Allocate(converted.size() * 4, 16);

      if (!up.cpu)
        return;

      std::memcpy(up.cpu, converted.data(), converted.size() * 4);
      const D3D12_INDEX_BUFFER_VIEW ibv = {up.gpu, UINT(converted.size() * 4), DXGI_FORMAT_R32_UINT};
      list_->IASetIndexBuffer(&ibv);
      draw_count = uint32_t(converted.size());
      draw_indexed = true;
    }
    else if (call.indexed && index_hit)
    {
      ++stats_.geo_index_hits;
      const D3D12_INDEX_BUFFER_VIEW ibv = {GeoAddress(*ientry), UINT(size_t(call.count) * (index32 ? 4 : 2)), index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
      list_->IASetIndexBuffer(&ibv);
    }
    else if (call.indexed)
    {
      const size_t bytes = size_t(call.count) * (index32 ? 4 : 2);
      const Upload up = Allocate(bytes, 16);

      if (!up.cpu)
        return;

      if (index32)
      {
        std::memcpy(up.cpu, indices.data(), bytes);
      }
      else
      {
        for (uint32_t i = 0; i < call.count; ++i)
        {
          const uint16_t v = uint16_t(indices[i]);
          std::memcpy(up.cpu + 2 * i, &v, 2);
        }
      }

      const D3D12_INDEX_BUFFER_VIEW ibv = {up.gpu, UINT(bytes), index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
      list_->IASetIndexBuffer(&ibv);

      if (ientry)
      {
        ientry->hash = ihash;
        ientry->vmin = vmin;
        ientry->vmax = vmax;
        GeoStore(*ientry, up, uint32_t(bytes));
      }
    }

    float cap_half_x = 0, cap_half_y = 0; // (F9 capture)
    clk.Switch(kPhConstants);
    // 11. Constants: device mirror (big-endian float4 x 256 per stage) + shared. Identical constant files (very common between draws) are uploaded once per frame. Only the registers the shaders read (most use a fraction of the 256)
    // The game thread compares only these constant bytes of the device block for this shader object (native.cpp, DeviceDelta)
    // (Keyed by object and microcode pointer, both read from the draw's snapshot; only for shaders resolved here.)
    if (const uint32_t obj = be32(base, dev + kDevVS); obj && vs_entry)
      guest_watch::ConstantUse(guest_watch::ShaderKey(obj, be32(base, obj + 0x20)), ConstantBytes(vs_entry));

    if (const uint32_t obj = be32(base, dev + kDevPS); obj && ps_entry)
      guest_watch::ConstantUse(guest_watch::ShaderKey(obj, be32(base, obj + 0x18)), ConstantBytes(ps_entry));

    const D3D12_GPU_VIRTUAL_ADDRESS vsc_gpu = UploadConstants(GuestPtr(base, dev + kDevVSConstants), ConstantBytes(vs_entry));
    const D3D12_GPU_VIRTUAL_ADDRESS psc_gpu = UploadConstants(GuestPtr(base, dev + kDevPSConstants), ps ? ConstantBytes(ps_entry) : 16);
    const Upload shared = Allocate(512);

    if (!vsc_gpu || !psc_gpu || !shared.cpu)
      return;

    {
      uint32_t sc[80] = {}; // [76] (c19.x): g_SstDepthFlip

      for (int i = 0; i < 16; ++i)
      {
        sc[i] = kPlaceholder2D;
        sc[16 + i] = kPlaceholder3D;
        sc[32 + i] = kPlaceholderCube;
        sc[48 + i] = 0; // sampler
      }

      clk.Switch(kPhTextures);
      draw_composite_ = false;
      draw_textures_.clear();
      BindTextures(base, dev, vs_entry, ps ? ps_entry : nullptr, sc);
      clk.Switch(kPhUiFit);

      // Aspect ratio: UI draws (into the full-size UI / final targets) keep their proportions in a centred 16:9 box.
      // 2D draws (w = 1, measured on the CPU) into full-size targets - UI and the 2D layers of menus drawn into the 3D scene's target - are fitted, except full-width layers (backgrounds, fades, full-screen copies), which stretch to fill the screen. Draws that cannot be measured: the UI targets' draws that don't sample a full-screen copy.
      //  - a single quad over the whole width (background, fade, full-screen copy) stretches;
      //  - a layer touching one edge of the 16:9 frame (menu backgrounds, bars) keeps that edge on the screen's edge and its other edge fitted;
      //  - in the 3D scene's target (menus drawn with 3D characters) fitted 2D is clipped to the 16:9 box, as on a 16:9 screen (parked elements).
      bool ui_fit = false, box_clip = false;
      float edge_a = 0.0f, edge_b = 0.0f; // edge-touching layers: clip x' = a x + b (else 0)
      if ((ui_fit_x_ != 1.0f || ui_fit_y_ != 1.0f) && REXCVAR_GET(sst_native_ui_fit) && color_rt && !color_rt->depth && color_rt->width * 10 >= width_ * 9 && !shadow_surfaces_.count(rt0))
      {
        // Draws that test and write depth are 3D (never measured: running every 3D draw's vertex shader on the CPU costs a lot of frame time). Small ones (a quad) are measured anyway: 2D sprites drawn with depth (e.g. character select's loading ring) are flat and get fitted
        const bool depth_test = (depthcontrol & 6) == 6; // Z_ENABLE + Z_WRITE_ENABLE
        const bool measure = !depth_test || vertex_count <= 6;
        const DrawExtent ext = measure ? MeasureDraw(base, call, vs_obj, vs_entry, first_vertex, vertex_count) : DrawExtent{};

        if (depth_test && scene_surfaces_.count(rt0) && vertex_count <= 6 && vs_entry && LogOnce(0xD100000000000000ull ^ vs_entry->hash ^ (uint64_t(ext.ok) << 40) ^ (uint64_t(ext.flat) << 41)))
          REXLOG_INFO("[sst-native] aspect: small depth-tested draw in the 3D target (vs {:016X}, "
                      "{} vertices, {}, {}) - {}",
                      vs_entry->hash, vertex_count, call.up_data ? "user memory" : "vertex buffer", ext.ok ? (ext.flat ? "flat" : "3D") : "not measured", ext.ok && ext.flat ? "fitted" : "not fitted");

        if (depth_test && !(ext.ok && ext.flat))
        {
          ui_fit = false;
        }
        else if (ext.ok)
        {
          const bool touch_l = ext.x0 >= -1.02f && ext.x0 <= -0.98f;
          const bool touch_r = ext.x1 >= 0.98f && ext.x1 <= 1.02f;
          // (A quad reaching well past the frame was laid out for the wider screen - wide layouts, scene_widen.h - and is fitted like the rest.)
          const bool within = ext.x0 >= -1.05f && ext.x1 <= 1.05f;
          const bool full = (ext.x0 <= -0.98f && ext.x1 >= 0.98f && vertex_count <= 6 && within) || (touch_l && touch_r);
          ui_fit = ext.flat && !full;
          // (Single quads only - a curved shape stretched this way would no longer meet the fitted pieces next to it.)
          if (ui_fit && ui_fit_x_ < 1.0f && (touch_l || touch_r) && vertex_count <= 6)
          {
            const float k = ui_fit_x_;

            if (touch_l)
            {
              edge_a = (ext.x1 * k + 1.0f) / (ext.x1 + 1.0f);
              edge_b = edge_a - 1.0f;
            }
            else
            {
              edge_a = (1.0f - ext.x0 * k) / (1.0f - ext.x0);
              edge_b = 1.0f - edge_a;
            }
          }
          // (No clipping to the 16:9 box in the 3D scene's target any more: the wide layouts draw backgrounds there beyond the box, and elements waiting off-screen are moved off the wider screen instead - hud_anchor.h.)
        }
        else
        {
          // Not measurable (e.g. drawn from user memory - the game's animated sprites, like character select's loading ring): UI targets' draws are fitted; in the 3D scene's target only those without depth testing (2D - 3D effects there test depth)
          const bool z_test = (depthcontrol & 2) != 0;
          ui_fit = !draw_composite_ && (!scene_surfaces_.count(rt0) || (call.up_data && !z_test));

          if (scene_surfaces_.count(rt0) && vs_entry && vertex_count <= 64 && LogOnce(0xD000000000000000ull ^ vs_entry->hash ^ (uint64_t(z_test) << 40)))
            REXLOG_INFO("[sst-native] aspect: unmeasured draw in the 3D target (vs {:016X}, "
                        "prim {}, {} vertices, {}, depth {:08X}) - {}",
                        vs_entry->hash, call.prim, vertex_count, call.up_data ? "user memory" : "vertex buffer", depthcontrol, ui_fit ? "fitted" : "not fitted");
        }
        // F9 capture: every fitted draw (aspect-ratio debugging)
        if (capture_active_ && (ui_fit || (ext.ok && vertex_count <= 64)))
          REXLOG_INFO("[sst-native] fit{}: draw {} rt {:08X}{} prim {} verts {} vs {:016X} depthctl "
                      "{:08X} measured {} flat {} x {:.3f}..{:.3f} y {:.3f}..{:.3f} edge {:.3f} "
                      "box {} composite {} viewport x {:.1f}+{:.1f} y {:.1f}+{:.1f} vte {:08X}{}",
                      ui_fit ? "" : " (NOT fitted)", draw_seq_, rt0, scene_surfaces_.count(rt0) ? " (scene)" : "", call.prim, vertex_count, vs_entry ? vs_entry->hash : 0, depthcontrol, ext.ok, ext.flat, ext.x0, ext.x1, ext.y0, ext.y1, edge_a, box_clip, draw_composite_, RegF(base, dev, PA_CL_VPORT_XOFFSET), RegF(base, dev, PA_CL_VPORT_XSCALE), RegF(base, dev, PA_CL_VPORT_YOFFSET), RegF(base, dev, PA_CL_VPORT_YSCALE), Reg(base, dev, PA_CL_VTE_CNTL), draw_textures_);
      }

      if (capture_active_ && color_rt && color_rt->width * 10 < width_ * 9 && vertex_count <= 64)
        REXLOG_INFO("[sst-native] fit (small target {}x{}): draw {} rt {:08X} prim {} verts {} vs "
                    "{:016X} depthctl {:08X}{}",
                    color_rt->width, color_rt->height, draw_seq_, rt0, call.prim, vertex_count, vs_entry ? vs_entry->hash : 0, depthcontrol, draw_textures_);
      const float fit_cx = float(width_) * 0.5f, fit_cy = float(height_) * 0.5f;
      clk.Switch(kPhConstants);
      const uint32_t vs_bools = be32(base, dev + kDevBools);
      const uint32_t ps_bools = be32(base, dev + kDevBools + 16);
      sc[64] = (vs_bools & 0xFFFF) | (ps_bools << 16);
      sc[65] = 0; // g_SwappedTexcoords: the stream swap already matches Xenos order
      float half_x = 0, half_y = 0;
      float alpha_ref = RegF(base, dev, RB_ALPHA_REF);
      std::memcpy(&sc[68], &alpha_ref, 4);
      // g_VertexIndexBase: SV_VertexID + this = the Xenos vertex index (r0.x)
      sc[69] = first_vertex;
      // Viewport (needed for the half-pixel offset too)
      const uint32_t vte = Reg(base, dev, PA_CL_VTE_CNTL);
      float xs = RegF(base, dev, PA_CL_VPORT_XSCALE), xo = RegF(base, dev, PA_CL_VPORT_XOFFSET);
      float ys = RegF(base, dev, PA_CL_VPORT_YSCALE), yo = RegF(base, dev, PA_CL_VPORT_YOFFSET);
      float zs = RegF(base, dev, PA_CL_VPORT_ZSCALE), zo = RegF(base, dev, PA_CL_VPORT_ZOFFSET);
      D3D12_VIEWPORT vp = {0, 0, float(target_w), float(target_h), 0, 1};

      if ((vte & 1) && xs != 0 && ys != 0 && std::isfinite(xs) && std::isfinite(ys))
      {
        vp.TopLeftX = xo - std::fabs(xs);
        vp.Width = 2 * std::fabs(xs);
        vp.TopLeftY = yo - std::fabs(ys);
        vp.Height = 2 * std::fabs(ys);
        vp.MinDepth = std::clamp(zo, 0.0f, 1.0f);
        vp.MaxDepth = std::clamp(zo + zs, 0.0f, 1.0f);

        // A negative z scale (the game's reversed depth) gives an inverted depth range (MinDepth > MaxDepth), which Intel GPUs
        // evaluate too coarsely: eyes and moustaches drawn just in front of the face fail the depth test. Instead the vertex shader
        // outputs w - z (g_SstDepthFlip, XenosRecomp under SST_RECOMP) and the range is the normal way round: same window depth
        if (zs < 0 && REXCVAR_GET(sst_native_depth_flip_vs))
        {
          std::swap(vp.MinDepth, vp.MaxDepth);
          sc[76] = 1;
        }

        if (ys > 0 && LogOnce(0xC000000000000001ull))
          REXLOG_WARN("[sst-native] positive viewport y scale ({}): image may be flipped", ys);
      }
      else if (LogOnce(0xC000000000000000ull | vte))
      {
        REXLOG_INFO("[sst-native] viewport transform off (PA_CL_VTE_CNTL {:08X}), using full "
                    "target",
                    vte);
      }

      if (REXCVAR_GET(sst_native_half_pixel) && vp.Width > 0 && vp.Height > 0)
      {
        half_x = 1.0f / vp.Width; // half a guest pixel in clip space
        half_y = -1.0f / vp.Height;
      }

      std::memcpy(&sc[66], &half_x, 4);
      std::memcpy(&sc[67], &half_y, 4);
      // Internal resolution: host pixels = guest pixels x the target's scale. Pixel shaders that read their screen position get it in guest pixels
      const float inv_scale[2] = {1.0f / draw_scale.x, 1.0f / draw_scale.y};
      std::memcpy(&sc[70], inv_scale, 8); // g_InvRenderScale (c17.zw)
      // g_ClipTransform (c18): clip-space scale + offset, applied in the vertex shader so UI moved outside the 16:9 box (ultrawide HUD) is not clipped. UI draws: positions scaled by ui_fit_ about the frame's centre
      {
        float ct[4] = {1.0f, 1.0f, 0.0f, 0.0f};

        if (ui_fit && vp.Width > 0 && vp.Height > 0)
        {
          const float hw = vp.Width * 0.5f, hh = vp.Height * 0.5f;
          const float vcx = vp.TopLeftX + hw, vcy = vp.TopLeftY + hh;
          ct[0] = ui_fit_x_;
          ct[1] = ui_fit_y_;
          ct[2] = (vcx - fit_cx) * (ui_fit_x_ - 1.0f) / hw;
          ct[3] = (vcy - fit_cy) * (1.0f - ui_fit_y_) / hh;

          if (edge_a != 0.0f)
            ct[0] = edge_a, ct[2] = edge_b; // (full-frame viewport)
        }

        std::memcpy(&sc[72], ct, sizeof(ct));
      }

      vp.TopLeftX *= draw_scale.x;
      vp.TopLeftY *= draw_scale.y;
      vp.Width *= draw_scale.x;
      vp.Height *= draw_scale.y;
      cap_half_x = half_x;
      cap_half_y = half_y;
      std::memcpy(shared.cpu, sc, sizeof(sc));
      list_->RSSetViewports(1, &vp);
      // Scissor: SetScissorRect (sub_822FD8D8) keeps PA_SC_WINDOW_SCISSOR_TL/BR at dev+0x28C4/+0x28C8 (x | y << 16): the viewport rectangle, intersected with the scissor rectangle when scissor testing is on (dev+0x2E50)
      {
        D3D12_RECT rect = {0, 0, LONG(target_w), LONG(target_h)};

        if (REXCVAR_GET(sst_native_scissor))
        {
          const uint32_t tl = be32(base, dev + kDevScissorTL), br = be32(base, dev + kDevScissorBR);
          const LONG x0 = LONG(tl & 0x7FFF), y0 = LONG((tl >> 16) & 0x7FFF);
          const LONG x1 = LONG(br & 0x7FFF), y1 = LONG((br >> 16) & 0x7FFF);

          if (x1 > x0 && y1 > y0)
          {
            rect = {std::min(x0, LONG(target_w)), std::min(y0, LONG(target_h)), std::min(x1, LONG(target_w)), std::min(y1, LONG(target_h))};
            const bool partial = x0 > 0 || y0 > 0 || x1 < LONG(target_w) || y1 < LONG(target_h);

            if (partial)
            {
              ++stats_.scissored_draws;

              if (LogOnce(0xE000000000000000ull | (uint64_t(tl) << 32 | br) >> 4) && stats_.scissor_logged < 16)
              {
                ++stats_.scissor_logged;
                REXLOG_INFO("[sst-native] scissor {}x{}-{}x{} (prim {}, {} vertices)", x0, y0, x1, y1, call.prim, call.count);
              }
            }
          }
        }
        // UI scissor rectangles (clipped text boxes) follow the fit. A side on the frame's edge (a scissor around the whole frame - which the 3D scene's 1280x736 target makes smaller than the target - or a tile of it) goes to the target's edge instead: wide layouts and edge-anchored HUD draw beyond the 16:9 box
        if (ui_fit && edge_a == 0.0f)
        {
          const LONG fw = std::min(LONG(target_w), LONG(width_));
          const LONG fh = std::min(LONG(target_h), LONG(height_));
          auto fx = [&](LONG v)
          {
            return fit_cx + (float(v) - fit_cx) * ui_fit_x_;
          };

          auto fy = [&](LONG v)
          {
            return fit_cy + (float(v) - fit_cy) * ui_fit_y_;
          };

          rect = {rect.left <= 0 ? 0 : LONG(std::floor(fx(rect.left))), rect.top <= 0 ? 0 : LONG(std::floor(fy(rect.top))), rect.right >= fw ? LONG(target_w) : LONG(std::ceil(fx(rect.right))), rect.bottom >= fh ? LONG(target_h) : LONG(std::ceil(fy(rect.bottom)))};
        }

        rect = Scaled(rect, draw_scale);
        list_->RSSetScissorRects(1, &rect);
      }
    }

    clk.Switch(kPhRecord);
    const float blend_factor[4] = {RegF(base, dev, RB_BLEND_RED), RegF(base, dev, RB_BLEND_RED + 1), RegF(base, dev, RB_BLEND_RED + 2), RegF(base, dev, RB_BLEND_RED + 3)};
    list_->OMSetBlendFactor(blend_factor);

    if (rt_mode)
    {
      SetRTState(*color_rt, D3D12_RESOURCE_STATE_RENDER_TARGET);

      if (depth_rt)
        SetRTState(*depth_rt, D3D12_RESOURCE_STATE_DEPTH_WRITE);

      const uint64_t pair = (uint64_t(color_rt->view + 1) << 32) | (depth_rt ? depth_rt->view + 1 : 0);

      if (pair != pass_pair_)
      {
        const uint32_t rt0 = be32(base, dev + kDevRT0);
        MarkPass(rt0, color_rt->width, color_rt->height, scene_surfaces_.count(rt0) ? 1 : shadow_surfaces_.count(rt0) ? 2
                                                                                                                      : 0);
        pass_pair_ = pair;
      }

      if (pair != bound_targets_)
      {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtRtv(color_rt->view);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth_rt ? RtDsv(depth_rt->view) : rtv;
        list_->OMSetRenderTargets(1, &rtv, FALSE, depth_rt ? &dsv : nullptr);
        bound_targets_ = pair;
      }

      last_color_rt_ = color_rt;
    }

    list_->SetPipelineState(pso);
    list_->SetGraphicsRootConstantBufferView(kRootVSConstants, vsc_gpu);
    list_->SetGraphicsRootConstantBufferView(kRootVSConstantsRaw, vsc_gpu);
    list_->SetGraphicsRootConstantBufferView(kRootPSConstants, psc_gpu);
    list_->SetGraphicsRootConstantBufferView(kRootPSConstantsRaw, psc_gpu);
    list_->SetGraphicsRootConstantBufferView(kRootShared, shared.gpu);
    list_->IASetPrimitiveTopology(topology);

    // RenderDoc / PIX: name the draw by its shaders and state (Event Browser)
    if (REXCVAR_GET(sst_native_debug_markers))
    {
      const std::string label = fmt::format("vs {:016X} ps {:016X} depth {:08X} colour {:08X} blend {:08X} bias {}", vs_entry ? vs_entry->hash : 0, ps_entry ? ps_entry->hash : 0, depthcontrol, colorcontrol, key.blend, key.depth_bias);
      const std::wstring wide(label.begin(), label.end());
      list_->SetMarker(0, wide.c_str(), UINT((wide.size() + 1) * sizeof(wchar_t)));
    }

    NoteDraw(vs_entry ? vs_entry->hash : 0, ps_entry ? ps_entry->hash : 0);

    if (draw_indexed)
      list_->DrawIndexedInstanced(draw_count, 1, 0, convert || rect_list ? 0 : -int32_t(vmin), 0);
    else
      list_->DrawInstanced(draw_count, 1, 0, 0);

    ++stats_.drawn;

    if (!ps)
      ++stats_.depth_only;

    if (capture_active_ && !host_elements.empty() && !call.up_data)
    {
      if (const float *hv = capture_host_; hv)
      {
        capture_detail += " | host vertex 0:";

        for (size_t i = 0; i < host_elements.size(); ++i)
          capture_detail += fmt::format(" ({} {} {} {})", hv[4 * i], hv[4 * i + 1], hv[4 * i + 2], hv[4 * i + 3]);
      }

      if (fetch_stream_count)
      {
        const uint8_t *r = resolved[0].src + size_t(first_vertex) * fetch_streams[0].stride;
        capture_detail += " | stream 0 raw:";

        for (int i = 0; i < 4; ++i)
          capture_detail += fmt::format(" {:08X}", be32raw(r + 4 * i));
      }

      capture_detail_ = std::move(capture_detail);
    }

    if (capture_active_ && !host_elements.empty() && !call.up_data)
      CaptureDraw(base, call, vs_obj, vs_entry, vs, layout, key.layout_hash, first_vertex, vertex_count, fetch_stream_count ? uint32_t(resolved[0].src - physical_membase_) : 0, reads_morph, cap_half_x, cap_half_y);
  }

  // F9 capture: GPU vertex positions (stream output) vs the CPU
  // For one frame, every drawn draw is drawn a second time as a point list into a stream-output buffer with a pipeline that has no rasterizer, capturing SV_Position exactly as the GPU computed it from the translated shader. The original microcode is run on the CPU over the same vertices; at the end of the frame both are compared. Disagreeing draws are translation (or GPU input) bugs; agreeing draws with vertices far outside the view are data

  bool Renderer::EnsureCaptureResources()
  {
    if (so_buffer_)
      return true;

    const size_t total = kSoDataBytes + kSoCounterBytes;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT}, up = {D3D12_HEAP_TYPE_UPLOAD}, rb = {D3D12_HEAP_TYPE_READBACK};

    if (FAILED(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&so_buffer_))) || FAILED(device_->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&so_readback_))))
    {
      so_buffer_.Reset();
      return false;
    }

    bd.Width = kSoCounterBytes;

    if (FAILED(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&so_zero_))))
    {
      so_buffer_.Reset();
      return false;
    }

    void *p = nullptr;
    so_zero_->Map(0, nullptr, &p);
    std::memset(p, 0, kSoCounterBytes);
    so_zero_->Unmap(0, nullptr);
    return true;
  }

  ID3D12PipelineState *Renderer::GetSoPipeline(const ShaderVariant *vs, uint64_t layout_hash, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout)
  {
    const uint64_t key = XXH3_64bits(&vs, sizeof(vs)) ^ layout_hash;

    if (auto it = so_pipelines_.find(key); it != so_pipelines_.end())
      return it->second.Get();

    static const D3D12_SO_DECLARATION_ENTRY kEntry = {0, "SV_Position", 0, 0, 4, 0};
    static const UINT kStride = 16;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = root_signature_.Get();
    d.VS = {vs->dxil.data(), vs->dxil.size()};
    d.StreamOutput.pSODeclaration = &kEntry;
    d.StreamOutput.NumEntries = 1;
    d.StreamOutput.pBufferStrides = &kStride;
    d.StreamOutput.NumStrides = 1;
    d.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = FALSE;
    d.InputLayout = {layout.data(), UINT(layout.size())};
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = D3D12Presenter::kGuestOutputFormat;
    d.DSVFormat = kDepthFormat;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));

    if (FAILED(hr))
    {
      if (LogOnce(0xCA00000000000000ull ^ key))
        REXLOG_WARN("[sst-native] capture: stream-output pipeline failed ({:08X})", uint32_t(hr));

      so_pipelines_.emplace(key, nullptr);
      return nullptr;
    }

    return so_pipelines_.emplace(key, std::move(pso)).first->second.Get();
  }

  void Renderer::CaptureDraw(const uint8_t *base, const DrawCall &call, uint32_t vs_obj, const ShaderEntry *vs_entry, const ShaderVariant *vs, const std::vector<D3D12_INPUT_ELEMENT_DESC> &layout, uint64_t layout_hash, uint32_t first_vertex, uint32_t vertex_count, uint32_t stream0, bool reads_morph, float half_x, float half_y)
  {
    if (captured_.size() >= kSoMaxDraws || so_used_ + size_t(vertex_count) * 16 > kSoDataBytes)
      return;

    ID3D12PipelineState *pso = GetSoPipeline(vs, layout_hash, layout);

    if (!pso)
      return;

    CapturedDraw c;
    c.vs_hash = vs_entry->hash;
    c.prim = call.prim;
    c.count = call.count;
    c.indexed = call.indexed;
    c.first_vertex = first_vertex;
    c.vertex_count = vertex_count;
    c.stream0 = stream0;
    c.morph = reads_morph;
    c.half_x = half_x;
    c.half_y = half_y;
    c.so_offset = so_used_;
    c.seq = draw_seq_;
    c.detail = std::move(capture_detail_);

    // CPU: the original microcode over the same vertices
    uint32_t bools[8], fetch[192], loops[32];
    check_constants_.resize(1024);

    if (!LoadVsState(base, call.device, vs_obj, vs_entry, check_ucode_, check_constants_.data(), bools, fetch, loops, nullptr))
    {
      std::vector<uint32_t> idx(vertex_count);

      for (uint32_t i = 0; i < vertex_count; ++i)
        idx[i] = first_vertex + i;

      c.cpu.resize(size_t(vertex_count) * 4);
      VsConstants vc;
      vc.float_constants = check_constants_.data();
      vc.bool_constants = bools;
      vc.loop_constants = loops;
      vc.fetch_constants = fetch;
      MemexportStats ignored;

      if (!memexport_->RunPositions(check_ucode_.data(), uint32_t(check_ucode_.size()), vc, idx.data(), vertex_count, c.cpu.data(), &ignored))
        c.cpu.clear();
    }

    // GPU: same pipeline inputs, drawn as points into the stream-output buffer
    const D3D12_GPU_VIRTUAL_ADDRESS so = so_buffer_->GetGPUVirtualAddress();
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW view = {so + c.so_offset, UINT64(vertex_count) * 16, so + kSoDataBytes + captured_.size() * 16};
    list_->SOSetTargets(0, 1, &view);
    list_->SetPipelineState(pso);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    NoteDraw(0, 2);
    list_->DrawInstanced(vertex_count, 1, 0, 0);
    list_->SOSetTargets(0, 0, nullptr);
    so_used_ += size_t(vertex_count) * 16;
    captured_.push_back(std::move(c));
  }

  void Renderer::FinishCapture()
  {
    const uint8_t *data = nullptr;
    const D3D12_RANGE range = {0, kSoDataBytes + kSoCounterBytes};

    if (FAILED(so_readback_->Map(0, &range, reinterpret_cast<void **>(const_cast<uint8_t **>(&data)))))
    {
      REXLOG_WARN("[sst-native] capture: readback failed");
      return;
    }

    uint32_t mismatched = 0, gpu_nan = 0, compared = 0;
    uint64_t vertices = 0;
    struct Reach
    {
      float extent;
      size_t index;
    };

    std::vector<Reach> reach;

    for (size_t d = 0; d < captured_.size(); ++d)
    {
      const CapturedDraw &c = captured_[d];
      uint32_t filled;
      std::memcpy(&filled, data + kSoDataBytes + d * 16, 4);
      const uint32_t n = std::min<uint32_t>(filled / 16, c.vertex_count);
      vertices += n;
      const float *g = reinterpret_cast<const float *>(data + c.so_offset);
      uint32_t bad = 0, first_bad = 0, nan = 0;
      float worst = 0, extent = 0;

      for (uint32_t v = 0; v < n; ++v)
      {
        const float *gp = g + 4 * size_t(v);
        const bool gfinite = std::isfinite(gp[0]) && std::isfinite(gp[1]) && std::isfinite(gp[2]) && std::isfinite(gp[3]);

        if (!gfinite)
          ++nan;

        if (gfinite && gp[3] > 1e-3f)
          extent = std::max({extent, std::fabs(gp[0] / gp[3]), std::fabs(gp[1] / gp[3])});

        if (c.cpu.empty())
          continue;

        const float *cp = &c.cpu[4 * size_t(v)];

        if (std::isnan(cp[0]) && std::isnan(cp[3]))
          continue; // no position export on the CPU
        // The translated shader adds the half-pixel offset to xy
        const float cx = cp[0] + c.half_x * cp[3], cy = cp[1] + c.half_y * cp[3];
        const bool cfinite = std::isfinite(cx) && std::isfinite(cy) && std::isfinite(cp[2]) && std::isfinite(cp[3]);
        float diff;

        if (gfinite != cfinite)
        {
          diff = INFINITY;
        }
        else if (!gfinite)
        {
          diff = 0;
        }
        else
        {
          const float scale = std::max({1.0f, std::fabs(cp[3]), std::fabs(cx), std::fabs(cy)});
          diff = std::max({std::fabs(gp[0] - cx), std::fabs(gp[1] - cy), std::fabs(gp[2] - cp[2]), std::fabs(gp[3] - cp[3])}) / scale;
        }

        if (diff > 1e-3f)
        {
          if (!bad++)
            first_bad = v;

          worst = std::max(worst, diff);
        }
      }

      if (!c.cpu.empty())
        ++compared;

      if (nan)
        ++gpu_nan;

      reach.push_back({extent, d});

      if (bad)
      {
        ++mismatched;

        if (mismatched <= 40 && capture_mismatch_logged_++ < 200)
        {
          const float *gp = g + 4 * size_t(first_bad);
          const float *cp = &c.cpu[4 * size_t(first_bad)];
          REXLOG_WARN("[sst-native] capture MISMATCH: draw #{} VS {:016X} prim {} {} {} vertices "
                      "from {} (buffer {:08X}{}): {} of {} vertices differ (worst {:.4g}); "
                      "vertex {}: GPU ({} {} {} {}) CPU ({} {} {} {});{}",
                      c.seq, c.vs_hash, c.prim, c.indexed ? "indexed" : "auto", c.vertex_count, c.first_vertex, c.stream0, c.morph ? ", face" : "", bad, n, worst, c.first_vertex + first_bad, gp[0], gp[1], gp[2], gp[3], cp[0] + c.half_x * cp[3], cp[1] + c.half_y * cp[3], cp[2], cp[3], c.detail);
        }
      }
    }

    if (capture_quiet_)
    {
      if (mismatched && capture_frames_logged_++ < 300)
        REXLOG_WARN("[sst-native] capture: frame {}: {} of {} draws differ between GPU and CPU", submitted_, mismatched, compared);

      const D3D12_RANGE none = {0, 0};
      so_readback_->Unmap(0, &none);
      return;
    }

    std::sort(reach.begin(), reach.end(), [](const Reach &a, const Reach &b)
              { return a.extent > b.extent; });
    REXLOG_INFO("[sst-native] capture: {} draws, {} vertices from the GPU; {} compared with the "
                "CPU, {} differ; {} draws with non-finite GPU positions. Draws reaching farthest "
                "outside the view (|x/w| or |y/w|):",
                captured_.size(), vertices, compared, mismatched, gpu_nan);

    for (size_t i = 0; i < reach.size() && i < 25; ++i)
    {
      const CapturedDraw &c = captured_[reach[i].index];
      REXLOG_INFO("[sst-native]   {:.1f}: draw #{} VS {:016X} prim {} {} vertices from {} "
                  "(buffer {:08X}{})",
                  reach[i].extent, c.seq, c.vs_hash, c.prim, c.vertex_count, c.first_vertex, c.stream0, c.morph ? ", face" : "");
    }

    const D3D12_RANGE none = {0, 0};
    so_readback_->Unmap(0, &none);
  }

  // textures

  const Renderer::TextureUsage &Renderer::GetTextureUsage(const ShaderEntry *entry)
  {
    auto [it, fresh] = texture_usage_.try_emplace(entry);

    if (!fresh)
      return it->second;

    TextureUsage &u = it->second;
    const std::vector<uint8_t> &ct = entry->container;
    auto ct32 = [&](size_t at) -> uint32_t
    {
      return at + 4 <= ct.size() ? (uint32_t(ct[at]) << 24) | (uint32_t(ct[at + 1]) << 16) | (uint32_t(ct[at + 2]) << 8) | ct[at + 3] : 0;
    };

    const uint32_t vsz = ct32(4), shader_off = ct32(0x18);
    const uint32_t phys_off = ct32(shader_off), bytes = ct32(size_t(shader_off) + 4);

    if (!bytes || size_t(vsz) + phys_off + bytes > ct.size())
      return u;

    std::vector<uint32_t> ucode(bytes / 4);

    for (size_t i = 0; i < ucode.size(); ++i)
      ucode[i] = ct32(size_t(vsz) + phys_off + 4 * i);

    u.mask = TextureFetchUsage(ucode.data(), uint32_t(ucode.size()), u.uses);

    for (uint32_t i = 0; i < 32; ++i)
    {
      if (!((u.mask >> i) & 1))
        continue;

      if (i >= 16 && LogOnce(0xD000000000000000ull ^ entry->hash))
        REXLOG_WARN("[sst-native] shader {:016X} samples fetch constant {} (>= 16, not bound)", entry->hash, i);

      if (u.uses[i].mixed_dimensions && LogOnce(0xD100000000000000ull ^ entry->hash))
        REXLOG_WARN("[sst-native] shader {:016X} samples fetch constant {} with several "
                    "dimensions",
                    entry->hash, i);
    }

    return u;
  }

  void Renderer::BindTextures(const uint8_t *base, uint32_t dev, const ShaderEntry *vs_entry, const ShaderEntry *ps_entry, uint32_t *sc)
  {
    if (!textures_)
      return;

    const TextureCache::AllocateFn allocate = [this](size_t size)
    {
      const Upload u = Allocate(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      return TextureCache::UploadSpace{u.cpu, u.resource, u.offset};
    };

    bool textured = false, placeholder = false;

    for (const ShaderEntry *e : {vs_entry, ps_entry})
    {
      if (!e)
        continue;

      const TextureUsage &u = GetTextureUsage(e);

      for (uint32_t i = 0; i < 16; ++i)
      {
        if (!((u.mask >> i) & 1))
          continue;

        const TextureFetchUse &use = u.uses[i];
        uint32_t fetch[6];

        for (uint32_t k = 0; k < 6; ++k)
          fetch[k] = be32(base, dev + kDevFetchConstants + 24 * i + 4 * k);

        const uint32_t dimension = use.dimension ? use.dimension - 1u : 1u;

        if (!composite_pages_.empty() && composite_pages_.count((fetch[1] >> 12) & 0x1FFFF))
          draw_composite_ = true;

        if (capture_active_ || trace_draw_)
          draw_textures_ += fmt::format(" t{}={:08X}:{}x{} fmt {} dim {} fetchdim {}{}", i, fetch[1] & ~0xFFFu, (fetch[2] & 0x1FFF) + 1, ((fetch[2] >> 13) & 0x1FFF) + 1, fetch[1] & 0x3F, (fetch[5] >> 9) & 3, dimension, composite_pages_.count((fetch[1] >> 12) & 0x1FFFF) ? "c" : "");

        const uint32_t slot = textures_->GetTexture(list_.Get(), allocate, fetch, dimension);

        if (slot != TextureCache::kNone)
        {
          textured = true;

          if (dimension == 2)
            sc[16 + i] = slot;
          else if (dimension == 3)
            sc[32 + i] = slot;
          else
            sc[i] = slot;
        }
        else
        {
          placeholder = true;
          // Stacked texture sampled as 3D: the shadow map array (not rendered yet)
          const bool stacked = ((fetch[5] >> 9) & 3) == 1 && ((fetch[1] >> 10) & 1);

          if (dimension == 2 && stacked)
          {
            sc[16 + i] = kPlaceholder3DBlack;
            ++stats_.shadow_placeholders;
          }
        }

        sc[48 + i] = textures_->GetSampler(fetch, use);
      }
    }

    if (textured)
      ++stats_.textured_draws;

    if (placeholder)
      ++stats_.texture_placeholders;
  }

  void Renderer::LogStats(uint64_t frame)
  {
    auto &s = stats_;
    LogProfile();

    if (pipeline_workers_)
    {
      REXLOG_INFO("[sst-native]   pipelines: {} ready ({} from the pipeline cache), {} compiling, "
                  "{:.1f} ms of background work this period, draws skipped while compiling {}, "
                  "UI-like pipelines built at once {} ({:.1f} ms), cache file {} KB",
                  s.pipelines_created, s.pipelines_from_cache, pending_pipelines_.size(), s.pipeline_ms, s.skip_pipeline_pending, s.pipelines_urgent, s.pipeline_urgent_ms, pipeline_workers_->saved_bytes >> 10);
      pipeline_workers_->SaveInBackground();
    }

    if (textures_)
      REXLOG_INFO("[sst-native] {}; draws with textures {} (placeholders {}, shadow map "
                  "stand-ins {}); scissored draws {}",
                  textures_->StatusLine(), s.textured_draws, s.texture_placeholders, s.shadow_placeholders, s.scissored_draws);
    REXLOG_INFO("[sst-native] frame {}: {} draws, {} drawn ({} depth-only); skipped: shader {} "
                "pending {} fetch {} prim {} rt {} pipeline {} data {} cull {} memexport-target {}; {} clears; "
                "pipelines {} ({} failed); shaders rebuilt from objects {}; upload {:.1f} MB; resolves {}; vertex buffers from draw-time copies {}",
                frame, s.draws, s.drawn, s.depth_only, s.skip_shader, s.skip_pending, s.skip_decl, s.skip_prim, s.skip_rt, s.skip_pipeline, s.skip_data, s.skip_cull, s.skip_memexport, s.clears, s.pipelines_created, s.pipelines_failed, s.shaders_from_objects, double(s.upload_bytes) / (1 << 20), s.resolves, s.vertex_copies_used);
    if (s.measured_draws)
      REXLOG_INFO("[sst-native] ui fit: {} draws measured on the CPU ({} 3D, stopped after the probe; {} big batches sampled)", s.measured_draws, s.measured_3d_early, s.measured_sampled);
    std::string prims;

    for (auto &[p, n] : s.prims)
      prims += fmt::format(" {:X}:{}", p, n);

    std::string clears;

    for (auto &[f, n] : s.clear_flags)
      clears += fmt::format(" {:X}:{}", f, n);

    REXLOG_INFO("[sst-native]   prims{} | clear flags{}", prims, clears);
    const MemexportStats &m = memexport_stats_;
    REXLOG_INFO("[sst-native]   memexport: {} draws, {} run ({} in the background, render thread "
                "waited {} times), constants: {} files uploaded, {} reused",
                s.memexport_draws, s.memexport_run, s.memexport_async, s.memexport_waits, s.constants_uploaded, s.constants_reused);
    REXLOG_INFO("[sst-native]   render targets: {} host targets made, resolves {} done / {} "
                "skipped, rectangle lists {}, frames shown from the front-buffer resolve {}",
                s.host_rts, s.resolves_done, s.resolves_skipped, s.rects_converted, s.presented_from_resolve);
    REXLOG_INFO("[sst-native]   geometry cache: vertex hits {}, index hits {} ({} trusted without "
                "re-hashing), stored {} ({:.1f} MB), "
                "dynamic (decoded each time) {}, flushes {}; {} keys, {:.0f} of {} MB used",
                s.geo_vertex_hits, s.geo_index_hits, s.geo_trusted, s.geo_stores, double(s.geo_store_bytes) / (1 << 20), s.geo_dynamic, s.geo_flushes, geo_entries_.size(), double(geo_offset_) / (1 << 20), geo_pool_size_ >> 20);
    REXLOG_INFO("[sst-native]   memexport: {} draws, {} run ({:.2f} ms total, {:.3f} ms/frame), {} "
                "cached, {} not run; totals: {} vertices, {} elements, bad address {}, bad format "
                "{}, out of range {}, fetch out of bounds {}, malformed {}, per-vertex {}",
                s.memexport_draws, s.memexport_run, s.memexport_ms, s.memexport_ms / double(std::max<uint64_t>(1, frame - last_stats_frame_)), s.memexport_cached, s.memexport_failed, m.vertices, m.elements, m.bad_address, m.bad_format, m.out_of_range, m.fetch_oob, m.malformed, m.per_vertex_draws);

    if (!memexport_costs_.empty())
    {
      std::vector<std::pair<uint64_t, MemexportShaderCost>> costs(memexport_costs_.begin(), memexport_costs_.end());
      std::sort(costs.begin(), costs.end(), [](const auto &a, const auto &b)
                { return a.second.ms > b.second.ms; });
      std::string text;

      for (size_t i = 0; i < costs.size() && i < 6; ++i)
      {
        const MemexportShaderCost &c = costs[i].second;
        text += fmt::format(" {:016X}: {:.1f} ms, {} runs ({} cached), {:.0f} ns/vertex;", costs[i].first, c.ms, c.runs, c.cached, c.vertices ? 1e6 * c.ms / double(c.vertices) : 0.0);
      }

      REXLOG_INFO("[sst-native]   memexport by shader:{}", text);
      memexport_costs_.clear();
    }

    if (s.fetch_remapped)
      REXLOG_INFO("[sst-native]   draws whose inputs sat in other fetch slots (matched by "
                  "register): {}",
                  s.fetch_remapped);

    if (s.computed_fetch_draws)
      REXLOG_INFO("[sst-native]   draws fetching with a computed index (CPU-resolved): {}", s.computed_fetch_draws);

    if (s.shader_rebinds)
      REXLOG_INFO("[sst-native]   shader objects reused for a different shader: {}", s.shader_rebinds);

    REXLOG_INFO("[sst-native]   morph checks: {} draws with implausible output; consumers {} "
                "(reading past the output {}, stale {}, implausible inputs {}); jump checks {}, "
                "draws with jumping vertices {}; output changed before use {}",
                s.memexport_bad_outputs, s.morph_consumers, s.morph_consumer_overrun, s.morph_consumer_stale, s.morph_consumer_bad, s.morph_jump_checks, s.morph_jump_draws, s.morph_modified);

    if (morph_history_.size() > 4096)
      morph_history_.clear();

    last_stats_frame_ = frame;

    if (REXCVAR_GET(sst_native_check_late_writes))
      REXLOG_INFO("[sst-native]   late writes: {} draws checked, {} data ranges changed after the "
                  "draw ({} of them index data)",
                  s.late_checked, s.late_changed, s.late_changed_index);

    if (REXCVAR_GET(sst_native_check_vs) || REXCVAR_GET(sst_native_check_vs_all))
    {
      REXLOG_INFO("[sst-native]   VS check: {} on-screen jump checks, {} draws with a vertex "
                  "jumping alone",
                  s.check_jump_checks, s.check_jump_draws);
      std::vector<const CheckEntry *> sometimes;

      for (const auto &[k, e] : check_entries_)
        if (e.flagged && e.flagged < e.checked)
          sometimes.push_back(&e);

      std::sort(sometimes.begin(), sometimes.end(), [](auto *a, auto *b)
                { return a->flagged > b->flagged; });
      REXLOG_INFO("[sst-native]   VS check: {} draws run on the CPU, {} with far-out vertices; {} "
                  "shader/buffer pairs only sometimes far out:",
                  s.check_draws, s.check_flagged, sometimes.size());

      for (size_t i = 0; i < sometimes.size() && i < 12; ++i)
        REXLOG_INFO("[sst-native]     VS {:016X} buffer {:08X} prim {}{}: {} of {} draws", sometimes[i]->vs, sometimes[i]->stream, sometimes[i]->prim, sometimes[i]->morph ? " (morph output)" : "", sometimes[i]->flagged, sometimes[i]->checked);

      check_entries_.clear();
    }

    std::vector<std::pair<uint64_t, uint64_t>> rts(s.render_targets.begin(), s.render_targets.end());
    std::sort(rts.begin(), rts.end(), [](auto &a, auto &b)
              { return a.second > b.second; });
    std::string rt_text;

    for (size_t i = 0; i < rts.size() && i < 6; ++i)
    {
      const uint32_t size = uint32_t(rts[i].first);
      rt_text += fmt::format(" {:08X}[{:08X} ~{}x{}]:{}", uint32_t(rts[i].first >> 32), size, (size >> 18) + 1, ((size >> 3) & 0x7FFF) + 1, rts[i].second);
    }

    REXLOG_INFO("[sst-native]   render target 0 (object[size word]:draws){}", rt_text);
    // Per-interval counters
    const uint64_t created = s.pipelines_created, failed = s.pipelines_failed, from_cache = s.pipelines_from_cache;
    const uint64_t logged = s.skip_shader_logged, clear_logged = s.clear_logged, sample_logged = s.sample_logged, bad_logged = s.memexport_bad_logged, consumer_bad_logged = s.morph_consumer_bad_logged, check_logged = s.check_logged, jump_logged = s.morph_jump_logged, modified_logged = s.morph_modified_logged, check_jump_logged = s.check_jump_logged, late_logged = s.late_logged, rebinds_logged = s.shader_rebinds_logged;
    s = Stats{};
    s.clear_logged = clear_logged;
    s.sample_logged = sample_logged;
    s.memexport_bad_logged = bad_logged;
    s.check_logged = check_logged;
    s.morph_jump_logged = jump_logged;
    s.morph_modified_logged = modified_logged;
    s.check_jump_logged = check_jump_logged;
    s.late_logged = late_logged;
    s.shader_rebinds_logged = rebinds_logged;
    s.morph_consumer_bad_logged = consumer_bad_logged;
    s.pipelines_created = created;
    s.pipelines_failed = failed;
    s.pipelines_from_cache = from_cache;
    s.skip_shader_logged = logged;
  }
}