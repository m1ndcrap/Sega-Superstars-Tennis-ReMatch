// sst_rematch - D3D trace layer

#include "gpu/native/native.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

REXCVAR_DEFINE_BOOL(sst_d3d_trace, false, "SST",
                    "Trace Xbox D3D calls (pass-through hooks) and log per-frame stats");

REXCVAR_DEFINE_INT32(sst_d3d_trace_calls, 4, "SST",
                     "Log arguments of the first N calls of each traced D3D function");

REXCVAR_DEFINE_INT32(sst_d3d_trace_every, 600, "SST",
                     "Print a D3D call summary every N frames while tracing");

REXCVAR_DEFINE_BOOL(sst_dump_shaders, false, "SST",
                    "Dump every unique shader container passed to Create*Shader");

namespace
{
  // guest memory helpers

  // Guest data is big-endian. On Windows the 0xE0000000+ physical window sits 0x1000 bytes further into host memory (64KB allocation granularity); the recompiled code adds this via REX_PHYS_HOST_OFFSET and so must we
  inline const uint8_t *GuestPtr(const uint8_t *base, uint32_t addr)
  {
    return base + addr + rex::memory::detail::PhysicalHostOffset(addr);
  }

  inline uint32_t be32(const uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, GuestPtr(base, addr), 4);
    return std::byteswap(v);
  }

  // traced function table

  enum class Arg : uint8_t
  {
    None,  // only count
    Dev,   // r3 = device, log r4..r8
    DevF1, // r3 = device, log r4..r8 and f1
    NoDev  // r3.. are plain arguments
  };

  struct Fn
  {
    uint32_t addr;
    const char *name;
    Arg args;
  };

  // Order matters only for the summary print-out
  constexpr Fn kFns[] = {
      // shaders
      {0x82302210, "CreateVertexShader", Arg::NoDev}, // receives 0x102A1101 containers
      {0x82302100, "CreatePixelShader", Arg::NoDev},  // receives 0x102A1100 containers
      {0x82300988, "SetVertexShader", Arg::Dev},      // verified at runtime, see OnSetShader
      {0x82300688, "SetPixelShader", Arg::Dev},
      {0x82300308, "SetVertexShaderConstantF", Arg::Dev},
      {0x823003E8, "SetPixelShaderConstantF", Arg::Dev},
      // geometry
      {0x82300CA8, "CreateVertexDeclaration", Arg::NoDev},
      {0x82300BA0, "SetVertexDeclaration", Arg::Dev},
      {0x822FD9D8, "SetStreamSource", Arg::Dev},
      {0x822FDAF8, "SetIndices", Arg::Dev},
      {0x8230C870, "DrawIndexedVertices", Arg::Dev},
      {0x8230C488, "DrawVertices", Arg::Dev},
      {0x8230BF90, "BeginVertices", Arg::Dev},
      {0x8230C430, "EndVertices", Arg::Dev},
      {0x8230C440, "DrawVerticesUP_wrapper", Arg::Dev},
      // textures / surfaces
      {0x82307588, "SetTexture", Arg::Dev},
      {0x822FCEE8, "SetSamplerState_A", Arg::Dev},
      {0x822FD078, "SetSamplerState_B", Arg::Dev},
      {0x82307340, "CreateSurface", Arg::NoDev},
      {0x82306FC8, "GetSurfaceLevel", Arg::NoDev},
      {0x82307080, "LockRect", Arg::NoDev},
      {0x82305680, "UnlockRect", Arg::NoDev},
      {0x8230D418, "Release", Arg::None},
      // targets / frame
      {0x822FE220, "SetRenderTarget", Arg::Dev},
      {0x822FE588, "SetDepthStencilSurface", Arg::Dev},
      {0x822FE0E8, "SetViewport", Arg::Dev},
      {0x8230A888, "Clear", Arg::DevF1},
      {0x823095F0, "ClearF", Arg::DevF1},
      {0x8230B0B8, "Resolve", Arg::DevF1},
      {0x82311120, "Resolve_variant", Arg::Dev},
      {0x8230F178, "SetPredication", Arg::Dev},
      {0x8230F780, "Tiling_A", Arg::DevF1},
      {0x8230F2E8, "Tiling_B", Arg::Dev},
      {0x8230EAA8, "KickOff", Arg::Dev},
      {0x82300D30, "PreSwap", Arg::Dev},
      {0x8230EAB0, "Swap", Arg::Dev},
      // render states (r4 = Xbox hardware enum value)
      {0x822FBCF0, "RS_ZEnable", Arg::Dev},
      {0x822FBD30, "RS_ZWriteEnable", Arg::Dev},
      {0x822FBD60, "RS_ZFunc", Arg::Dev},
      {0x822FB600, "RS_AlphaTestEnable", Arg::Dev},
      {0x822FBAC8, "RS_AlphaFunc", Arg::Dev},
      {0x822FBA60, "RS_AlphaRef", Arg::Dev},
      {0x822FB638, "RS_AlphaBlendEnable", Arg::Dev},
      {0x822FB758, "RS_SrcBlend", Arg::Dev},
      {0x822FB7E8, "RS_DestBlend", Arg::Dev},
      {0x822FB6C8, "RS_BlendOp", Arg::Dev},
      {0x822FB9C8, "RS_SeparateAlphaBlendEnable", Arg::Dev},
      {0x822FB5A0, "RS_CullMode", Arg::Dev},
      {0x822FB5D0, "RS_PA_SU_SC_other", Arg::Dev},
      {0x822FC190, "RS_DepthBias", Arg::Dev},
      {0x822FC0C8, "RS_SlopeScaleDepthBias", Arg::Dev},
      {0x822FC070, "RS_ClipPlaneEnable", Arg::Dev},
      {0x822FC288, "RS_ColorWriteEnable", Arg::Dev},
  };

  constexpr size_t kNumFns = std::size(kFns);

  constexpr size_t IndexOf(uint32_t addr)
  {
    for (size_t i = 0; i < kNumFns; ++i)
      if (kFns[i].addr == addr)
        return i;

    return kNumFns;
  }

  enum Stage : int
  {
    kVS = 0,
    kPS = 1
  };

  struct Stats
  {
    std::array<std::atomic<uint64_t>, kNumFns> total{};
    std::array<std::atomic<uint64_t>, kNumFns> frame{};
    std::array<std::atomic<uint32_t>, kNumFns> logged{};
    std::atomic<uint64_t> frames{0};
    std::atomic<uint32_t> device{0};

    std::mutex mtx; // guards everything below

    std::map<std::string, uint64_t> histo;            // "Name = value" -> count
    std::set<uint64_t> shader_hashes[2];              // unique containers per stage
    uint64_t shader_magic_mismatch[2] = {0, 0};       // container stage != function stage
    std::unordered_map<uint32_t, int> shader_objects; // guest object -> Stage
    uint64_t set_shader_ok[2] = {0, 0};
    uint64_t set_shader_wrong[2] = {0, 0};
    uint64_t set_shader_unknown[2] = {0, 0};
    std::map<std::string, uint64_t> tex_kinds;
  };

  Stats &S()
  {
    static Stats s;
    return s;
  }

  bool Tracing() { return REXCVAR_GET(sst_d3d_trace) || REXCVAR_GET(sst_dump_shaders); }

  void Histo(const std::string &key)
  {
    std::lock_guard lock(S().mtx);
    ++S().histo[key];
  }

  void LogCall(size_t i, const PPCContext &ctx)
  {
    const Fn &fn = kFns[i];

    switch (fn.args)
    {
    case Arg::None:
      return;

    case Arg::Dev:
      REXLOG_INFO("[d3d] {}(dev={:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})", fn.name,
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
      return;

    case Arg::DevF1:
      REXLOG_INFO("[d3d] {}(dev={:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, f1={})", fn.name,
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                  ctx.f1.f64);
      return;

    case Arg::NoDev:
      REXLOG_INFO("[d3d] {}({:08X}, {:08X}, {:08X}, {:08X}, {:08X})", fn.name, ctx.r3.u32,
                  ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
      return;
    }
  }

  void OnEnter(size_t i, const PPCContext &ctx)
  {
    auto &s = S();
    s.total[i].fetch_add(1, std::memory_order_relaxed);
    s.frame[i].fetch_add(1, std::memory_order_relaxed);

    if (!REXCVAR_GET(sst_d3d_trace))
      return;

    const Fn &fn = kFns[i];

    if (fn.args == Arg::Dev || fn.args == Arg::DevF1)
    {
      uint32_t expected = 0;

      if (!s.device.compare_exchange_strong(expected, ctx.r3.u32) && expected != ctx.r3.u32)
      {
        if (s.logged[i].load() == 0)
          REXLOG_WARN("[d3d] {}: r3={:08X} is not the first device seen ({:08X})", fn.name,
                      ctx.r3.u32, expected);
      }
    }

    if (s.logged[i].fetch_add(1) < static_cast<uint32_t>(REXCVAR_GET(sst_d3d_trace_calls)))
      LogCall(i, ctx);
  }

  // shaders
  void OnCreateShader(int fn_stage, const PPCContext &ctx, const uint8_t *base)
  {
    const uint32_t addr = ctx.r3.u32;
    const uint32_t magic = be32(base, addr);

    if ((magic & 0xFFFFFF00u) != 0x102A1100u)
    {
      REXLOG_WARN("[d3d] Create shader: {:08X} is not a shader container (magic {:08X})", addr,
                  magic);
      return;
    }

    const int stage = (magic & 1) ? kVS : kPS; // 0x102A1101 = VS, 0x102A1100 = PS
    const uint32_t size = be32(base, addr + 4) + be32(base, addr + 8);

    if (size == 0 || size > (1u << 20))
      return;

    // FNV-1a 64 over the container: identifies unique shaders for the dump
    const uint8_t *p = GuestPtr(base, addr);
    uint64_t h = 0xcbf29ce484222325ull;

    for (uint32_t k = 0; k < size; ++k)
      h = (h ^ p[k]) * 0x100000001b3ull;

    bool fresh;

    {
      std::lock_guard lock(S().mtx);

      if (stage != fn_stage)
        ++S().shader_magic_mismatch[fn_stage];

      fresh = S().shader_hashes[stage].insert(h).second;
    }

    if (fresh && REXCVAR_GET(sst_dump_shaders))
    {
      auto dir = rex::filesystem::GetExecutableFolder() / "d3d_trace" / "shaders";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      char name[48];
      std::snprintf(name, sizeof(name), "%s_%016llX.bin", stage == kVS ? "VS" : "PS", static_cast<unsigned long long>(h));
      std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char *>(p), size);
    }
  }

  // Called after the original Create*Shader returns its object in r3
  void OnShaderCreated(int stage, const PPCContext &ctx)
  {
    if (!ctx.r3.u32)
      return;

    std::lock_guard lock(S().mtx);
    S().shader_objects[ctx.r3.u32] = stage;
  }

  // Set*Shader(dev, obj): check obj was created by the matching stage
  void OnSetShader(int fn_stage, const PPCContext &ctx)
  {
    const uint32_t obj = ctx.r4.u32;

    if (!obj)
      return;

    std::lock_guard lock(S().mtx);
    auto &s = S();
    auto it = s.shader_objects.find(obj);

    if (it == s.shader_objects.end())
      ++s.set_shader_unknown[fn_stage];
    else if (it->second == fn_stage)
      ++s.set_shader_ok[fn_stage];
    else if (s.set_shader_wrong[fn_stage]++ == 0)
      REXLOG_WARN("[d3d] Set{}Shader got a {} object ({:08X}) - names are swapped",
                  fn_stage == kVS ? "Vertex" : "Pixel", it->second == kVS ? "vertex" : "pixel", obj);
  }

  // textures
  void OnSetTexture(const PPCContext &ctx, const uint8_t *base)
  {
    const uint32_t tex = ctx.r5.u32;

    if (!tex)
      return;

    // Fetch constant words as SetTexture reads them (tex+0x1C .. tex+0x30)
    uint32_t fc[6];

    for (int k = 0; k < 6; ++k)
      fc[k] = be32(base, tex + 0x1C + 4 * k);

    const uint32_t type = fc[0] & 3; // 2 = texture fetch constant
    const uint32_t tiled = (fc[0] >> 31) & 1;
    const uint32_t format = fc[1] & 0x3F;
    const uint32_t dim = (fc[5] >> 9) & 3; // 0=1D 1=2D 2=3D/stacked 3=cube
    const uint32_t w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
    char key[64];
    std::snprintf(key, sizeof(key), "type=%u fmt=0x%02X dim=%u tiled=%u", type, format, dim, tiled);
    std::lock_guard lock(S().mtx);
    auto &n = S().tex_kinds[key];

    if (n++ == 0 && REXCVAR_GET(sst_d3d_trace))
      REXLOG_INFO("[d3d] new texture kind {} (2D size {}x{}) first at {:08X}: "
                  "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  key, w, h, tex, fc[0], fc[1], fc[2], fc[3], fc[4], fc[5]);
  }

  // frame summary
  void PrintSummary(uint64_t frame)
  {
    auto &s = S();
    REXLOG_INFO("[d3d] ===== frame {} : calls this frame (total) =====", frame);

    for (size_t i = 0; i < kNumFns; ++i)
    {
      const uint64_t t = s.total[i].load();

      if (t)
        REXLOG_INFO("[d3d]   {:<30} {:>6} ({})", kFns[i].name, s.frame[i].load(), t);
    }

    std::lock_guard lock(s.mtx);
    REXLOG_INFO("[d3d]   unique shader containers: VS {} PS {} (stage mismatches: VS fn {} PS fn {})",
                s.shader_hashes[kVS].size(), s.shader_hashes[kPS].size(),
                s.shader_magic_mismatch[kVS], s.shader_magic_mismatch[kPS]);
    REXLOG_INFO("[d3d]   SetVertexShader objects: ok {} wrong {} unknown {}", s.set_shader_ok[kVS],
                s.set_shader_wrong[kVS], s.set_shader_unknown[kVS]);
    REXLOG_INFO("[d3d]   SetPixelShader  objects: ok {} wrong {} unknown {}", s.set_shader_ok[kPS],
                s.set_shader_wrong[kPS], s.set_shader_unknown[kPS]);

    for (auto &[k, n] : s.histo)
      REXLOG_INFO("[d3d]   value {:<40} x{}", k, n);

    for (auto &[k, n] : s.tex_kinds)
      REXLOG_INFO("[d3d]   texture {:<36} x{}", k, n);
  }

  void OnSwap()
  {
    auto &s = S();
    const uint64_t frame = s.frames.fetch_add(1) + 1;
    const int every = REXCVAR_GET(sst_d3d_trace_every);

    if (REXCVAR_GET(sst_d3d_trace) && every > 0 && frame % static_cast<uint64_t>(every) == 0)
      PrintSummary(frame);

    for (auto &f : s.frame)
      f.store(0, std::memory_order_relaxed);
  }

  // Arguments for the native renderer, taken before the original call.
  sst::native::HookArgs CaptureArgs(const PPCContext &ctx)
  {
    return {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.f1.f64, ctx.r9.u32, ctx.r10.u32};
  }

}

// hook definitions
#define SST_HOOK(ADDR, BEFORE, AFTER, NATIVE_BEFORE, NATIVE_AFTER)      \
  extern "C" void __imp__sub_##ADDR(PPCContext &__restrict, uint8_t *); \
  extern "C" REX_FUNC(sub_##ADDR)                                       \
  {                                                                     \
    const bool tracing = Tracing();                                     \
    if (tracing)                                                        \
    {                                                                   \
      constexpr size_t kIdx = IndexOf(0x##ADDR);                        \
      static_assert(kIdx < kNumFns, "address missing from kFns");       \
      OnEnter(kIdx, ctx);                                               \
      BEFORE;                                                           \
    }                                                                   \
    NATIVE_BEFORE;                                                      \
    __imp__sub_##ADDR(ctx, base);                                       \
    NATIVE_AFTER;                                                       \
    if (tracing)                                                        \
    {                                                                   \
      AFTER;                                                            \
    }                                                                   \
  }
#define SST_TRACE_EX(ADDR, BEFORE, AFTER) SST_HOOK(ADDR, BEFORE, AFTER, (void)0, (void)0)
#define SST_TRACE(ADDR) SST_TRACE_EX(ADDR, (void)0, (void)0)
#define SST_TRACE_VALUE(ADDR, NAME) \
  SST_TRACE_EX(ADDR, Histo(std::string(NAME " = ") + std::to_string(ctx.r4.u32)), (void)0)

SST_HOOK(82302210, OnCreateShader(kVS, ctx, base), OnShaderCreated(kVS, ctx), const uint32_t native_container = ctx.r3.u32, sst::native::OnCreateShader(base, native_container, ctx.r3.u32))
SST_HOOK(82302100, OnCreateShader(kPS, ctx, base), OnShaderCreated(kPS, ctx), const uint32_t native_container = ctx.r3.u32, sst::native::OnCreateShader(base, native_container, ctx.r3.u32))
SST_TRACE_EX(82300988, OnSetShader(kVS, ctx), (void)0)
SST_TRACE_EX(82300688, OnSetShader(kPS, ctx), (void)0)
SST_TRACE(82300308)
SST_TRACE(823003E8)
SST_TRACE(82300CA8)
SST_TRACE(82300BA0)
SST_HOOK(822FD9D8, (void)0, (void)0, const sst::native::HookArgs native_args = CaptureArgs(ctx), sst::native::OnSetStreamSource(native_args))
SST_TRACE(822FDAF8)
SST_HOOK(8230C870, Histo(std::string("DrawIndexedVertices prim = ") + std::to_string(ctx.r4.u32)), (void)0, const sst::native::HookArgs native_args = CaptureArgs(ctx), sst::native::OnDrawIndexed(base, native_args))
SST_HOOK(8230C488, Histo(std::string("DrawVertices prim = ") + std::to_string(ctx.r4.u32)), (void)0, const sst::native::HookArgs native_args = CaptureArgs(ctx), sst::native::OnDraw(base, native_args))
SST_HOOK(8230BF90, Histo(std::string("BeginVertices prim = ") + std::to_string(ctx.r4.u32)), (void)0, const sst::native::HookArgs native_args = CaptureArgs(ctx), sst::native::OnBeginVertices(native_args, ctx.r3.u32))
SST_HOOK(8230C430, (void)0, (void)0, const sst::native::HookArgs native_args = CaptureArgs(ctx), sst::native::OnEndVertices(base, native_args))
SST_HOOK(8230C440, Histo(std::string("DrawVerticesUP_wrapper prim = ") + std::to_string(ctx.r4.u32)), (void)0, const sst::native::HookArgs native_args = CaptureArgs(ctx), (sst::native::OnDrawUPDone(), sst::native::OnDrawUP(base, native_args)))
SST_TRACE_EX(82307588, OnSetTexture(ctx, base), (void)0)
SST_TRACE(822FCEE8)
SST_TRACE(822FD078)
SST_TRACE(82307340)
SST_TRACE(82306FC8)
SST_TRACE(82307080)
SST_TRACE(82305680)
SST_TRACE(8230D418)
SST_TRACE(822FE220)
SST_TRACE(822FE588)
SST_TRACE(822FE0E8)
SST_HOOK(8230A888, (void)0, (void)0, const sst::native::HookArgs native_args = CaptureArgs(ctx), sst::native::OnClear(base, native_args))
SST_TRACE(823095F0)
// Resolve: the native renderer acts before the original call (the device still holds the render-target state the resolve reads)
SST_HOOK(8230B0B8, Histo(std::string("Resolve flags = ") + std::to_string(ctx.r4.u32)), (void)0, sst::native::OnResolve(base, CaptureArgs(ctx)), (void)0)
SST_HOOK(82311120, (void)0, (void)0, sst::native::OnResolveVariant(base, CaptureArgs(ctx)), (void)0)
SST_HOOK(8230F178, (void)0, (void)0, sst::native::OnSetPredication(base, CaptureArgs(ctx)), (void)0)
SST_TRACE(8230F780)
SST_TRACE(8230F2E8)
SST_TRACE(8230EAA8)
SST_TRACE(82300D30)
SST_HOOK(8230EAB0, OnSwap(), (void)0, sst::native::OnSwap(), (void)0)
SST_TRACE_VALUE(822FBCF0, "ZEnable")
SST_TRACE_VALUE(822FBD30, "ZWriteEnable")
SST_TRACE_VALUE(822FBD60, "ZFunc")
SST_TRACE_VALUE(822FB600, "AlphaTestEnable")
SST_TRACE_VALUE(822FBAC8, "AlphaFunc")
SST_TRACE_VALUE(822FBA60, "AlphaRef")
SST_TRACE_VALUE(822FB638, "AlphaBlendEnable")
SST_TRACE_VALUE(822FB758, "SrcBlend")
SST_TRACE_VALUE(822FB7E8, "DestBlend")
SST_TRACE_VALUE(822FB6C8, "BlendOp")
SST_TRACE_VALUE(822FB9C8, "SeparateAlphaBlendEnable")
SST_TRACE_VALUE(822FB5A0, "CullMode")
SST_TRACE_VALUE(822FB5D0, "PA_SU_SC_other")
SST_TRACE(822FC190)
SST_TRACE(822FC0C8)
SST_TRACE_VALUE(822FC070, "ClipPlaneEnable")
SST_TRACE_VALUE(822FC288, "ColorWriteEnable")