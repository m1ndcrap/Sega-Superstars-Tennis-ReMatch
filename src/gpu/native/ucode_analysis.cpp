// Native renderer: microcode analysis

#include "gpu/native/ucode_analysis.h"
#include "gpu/native/cpu_shader.h"
#include <fmt/format.h>
#include <algorithm>

namespace sst::native
{
  using cpu_shader::ReadCf;
  namespace ucode = rex::graphics::ucode;
  namespace xenos = rex::graphics::xenos;

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

      if (op != Op::kExec && op != Op::kExecEnd && op != Op::kCondExec && op != Op::kCondExecEnd && op != Op::kCondExecPred && op != Op::kCondExecPredEnd && op != Op::kCondExecPredClean && op != Op::kCondExecPredCleanEnd)
        continue;

      const auto &e = *reinterpret_cast<const ucode::ControlFlowExecInstruction *>(&ci);
      cf_end = std::min(cf_end, 2 * e.address());

      for (uint32_t i = 0; i < e.count() && 3 * size_t(e.address() + i) + 3 <= dwords; ++i)
      {
        if ((e.sequence() >> (2 * i)) & 1)
          continue; // fetch

        const auto &a = *reinterpret_cast<const ucode::AluInstruction *>(&ucode[3 * (e.address() + i)]);
        const bool scalar_const_op = ucode::GetAluScalarOpcodeInfo(a.scalar_opcode()).operand_count == 2;

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

      if (op != Op::kExec && op != Op::kExecEnd && op != Op::kCondExec && op != Op::kCondExecEnd && op != Op::kCondExecPred && op != Op::kCondExecPredEnd && op != Op::kCondExecPredClean && op != Op::kCondExecPredCleanEnd)
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

      if (op != Op::kExec && op != Op::kExecEnd && op != Op::kCondExec && op != Op::kCondExecEnd && op != Op::kCondExecPred && op != Op::kCondExecPredEnd && op != Op::kCondExecPredClean && op != Op::kCondExecPredCleanEnd)
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

        if (fop != FOp::kTextureFetch && fop != FOp::kGetTextureWeights && fop != FOp::kGetTextureComputedLod && fop != FOp::kGetTextureGradients)
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
          s += fmt::format(" vfetch r{} f{} c{} +{} /{} swz{:03o};", v.dest(), uint32_t(v.data_format()), v.fetch_constant_index(), v.offset(), v.stride(), v.dest_swizzle());
        }
        else
        {
          const auto &a = *reinterpret_cast<const ucode::AluInstruction *>(ins);
          s += fmt::format(" {}/{} {}{}.{:X};", ucode::GetAluVectorOpcodeInfo(a.vector_opcode()).name, ucode::GetAluScalarOpcodeInfo(a.scalar_opcode()).name, a.is_export() ? "e" : "r", a.vector_dest(), a.vector_write_mask());
        }
      }

      if (op == Op::kExecEnd)
        break;
    }

    return s;
  }
}