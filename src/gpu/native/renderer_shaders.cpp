// Native renderer: the bound shaders (objects the hooks missed are rebuilt) and the CPU runs of a draw's vertex shader (debug checks, computed fetch indices)

#include "gpu/native/draw_context.h"
#include "gpu/native/renderer_debug.h"

namespace sst::native
{
  using namespace renderer_detail;

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
      {
        if (in.address < skip.size())
          skip[in.address] = true;
      }

      for (uint32_t i = 0; i * 12 + 12 <= bytes; ++i)
      {
        if (!skip[i] && std::memcmp(a + 12 * i, b + 12 * i, 12) != 0)
          return false;
      }

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
          REXLOG_WARN("[sst-native] {} shader object {:08X}: same header but different microcode (was {:016X}); rebuilding it from the object", vertex ? "vertex" : "pixel", object, e->hash);
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
          REXLOG_WARN("[sst-native] {} shader object {:08X} now holds a different shader (was {:016X}); rebuilding it from the object", vertex ? "vertex" : "pixel", object, e->hash);

        it->second = vhash;
        object_ucode_[object] = ucode_ptr;
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
          {
            if (q * 4 + k < vertex_count && q * 4 + k >= kProbe)
              check_indices_.push_back(first_vertex + q * 4 + k);
          }
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
}