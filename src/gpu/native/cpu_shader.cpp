// Derived from Xenia's ShaderInterpreter and memexport translators (Copyright 2022 Ben Vanik. All rights reserved). Released under the BSD license (ReXGlue SDK adaptation by Tom Clay, 2026).

#include "gpu/native/cpu_shader.h"
#include "gpu/native/cpu_shader_values.h"
#include "gpu/native/vertex_format.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace sst::native::cpu_shader
{
  namespace
  {
    using V = ucode::AluVectorOpcode;
    using S = ucode::AluScalarOpcode;

    // D3D9: 0 * x = +0. Branch-free (the operands are zero often and unpredictably).
    inline float Mul(float a, float b)
    {
      const uint32_t keep = 0u - uint32_t((a != 0.0f) & (b != 0.0f));
      return AsFloat(AsUint(a * b) & keep);
    }

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

        if (std::isgreaterequal(v[0], v[1]) && std::isgreaterequal(v[0], v[2]) && std::isgreaterequal(v[0], v[3]))
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
        r[0] = float(o[0][0] == o[1][0] || o[0][1] == o[1][1] || o[0][2] == o[1][2] || o[0][3] == o[1][3]);
        return true;

      case V::kKillGt:
        r[0] = float(std::isgreater(o[0][0], o[1][0]) || std::isgreater(o[0][1], o[1][1]) || std::isgreater(o[0][2], o[1][2]) || std::isgreater(o[0][3], o[1][3]));
        return true;

      case V::kKillGe:
        r[0] = float(std::isgreaterequal(o[0][0], o[1][0]) || std::isgreaterequal(o[0][1], o[1][1]) || std::isgreaterequal(o[0][2], o[1][2]) || std::isgreaterequal(o[0][3], o[1][3]));
        return true;

      case V::kKillNe:
        r[0] = float(o[0][0] != o[1][0] || o[0][1] != o[1][1] || o[0][2] != o[1][2] || o[0][3] != o[1][3]);
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
        if (ps == -FLT_MAX || !std::isfinite(ps) || !std::isfinite(so[1]) || std::islessequal(so[1], 0.0f))
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
  }

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

        break;
      }

      case Op::kCondJmp:
      {
        const auto &c = *reinterpret_cast<const ucode::ControlFlowCondJmpInstruction *>(&ci);

        if (!c.is_unconditional() && c.is_predicated())
          return true;

        break;
      }

      case Op::kLoopEnd:
      {
        const auto &l = *reinterpret_cast<const ucode::ControlFlowLoopEndInstruction *>(&ci);

        if (l.is_predicated_break())
          return true;

        break;
      }

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

            if (!v.is_mini_fetch()) // a mini fetch's src field is unused (it reuses the full fetch's address)
              use(v.src(), v.is_src_relative());
          }
          else
          {
            const auto &a = *reinterpret_cast<const ucode::AluInstruction *>(ins);

            if (a.is_predicated())
              return true;

            // a0-relative constants are fine: operands are gathered per lane. Exports write export registers, not temps; also the scalar result: counting e62 here made every batch clear 64 registers. Only registers actually written (non-zero write mask) or read (operands the opcodes use) count:
            // unused fields hold arbitrary register numbers (a retain_prev scalar dest of r63 made every batch clear all 64 registers)
            const ucode::AluVectorOpcodeInfo &vinfo = ucode::GetAluVectorOpcodeInfo(a.vector_opcode());
            const ucode::AluScalarOpcodeInfo &sinfo = ucode::GetAluScalarOpcodeInfo(a.scalar_opcode());

            if (!a.is_export())
            {
              if (a.GetVectorOpResultWriteMask())
                use(a.vector_dest(), a.is_vector_dest_relative());

              if (a.GetScalarOpResultWriteMask())
                use(a.scalar_dest(), a.is_scalar_dest_relative());
            }

            for (uint32_t s = 1; s <= 2; ++s)
              if (vinfo.operand_components_used[s - 1] && a.src_is_temp(s))
                use(ucode::AluInstruction::src_temp_reg(a.src_reg(s)), ucode::AluInstruction::is_src_temp_relative(a.src_reg(s)));

            // src3: the vector op's third operand, or the scalar op's operand
            if ((vinfo.operand_components_used[2] || (a.scalar_opcode() != S::kRetainPrev && sinfo.operand_count == 1)) && a.src_is_temp(3))
              use(ucode::AluInstruction::src_temp_reg(a.src_reg(3)), ucode::AluInstruction::is_src_temp_relative(a.src_reg(3)));

            if (sinfo.operand_count == 2)
              use(a.scalar_const_reg_op_src_temp_reg(), false);
          }
        }

        break;
      }

      default:
        break;
      }
    }

    *temp_count = temps;
    return false;
  }

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

        if (op == Op::kCondExec || op == Op::kCondExecEnd || op == Op::kCondExecPredClean || op == Op::kCondExecPredCleanEnd)
        {
          const auto &cond = *reinterpret_cast<const ucode::ControlFlowCondExecInstruction *>(&ci);
          run = cond.condition() == BoolConstant(cond.bool_address());
        }
        else if (op == Op::kCondExecPred || op == Op::kCondExecPredEnd)
        {
          const auto &cond = *reinterpret_cast<const ucode::ControlFlowCondExecPredInstruction *>(&ci);
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

        // An *End exec ends the shader even when its condition skipped it.
        if (ucode::DoesControlFlowOpcodeEndShader(op))
          return;

        break;
      }

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
        break;
      }

      case Op::kLoopEnd:
      {
        if (!state_.loop_depth)
          break;

        const auto &le = *reinterpret_cast<const ucode::ControlFlowLoopEndInstruction *>(&ci);
        const uint32_t d = state_.loop_depth - 1;
        const uint32_t it = ++state_.loop_iterators[d];

        if (it < state_.loops[d].count && (!le.is_predicated_break() || le.condition() != pred_[0]))
        {
          cf_next = le.address();
          break;
        }

        --state_.loop_depth;
        break;
      }

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
        break;
      }

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
        break;
      }

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

    // Gathers source operand `src` (1-3) of every lane into out[comp][lane], components in the order given by `comps` (already swizzled by the caller when it needs a subset). With `ptrs`: ptrs[comp] points at the values - straight at the temp register when the operand has no modifiers (temps are stored with denormals already flushed, see StoreTemp), else at out[comp]
    auto gather = [&](uint32_t src, bool scalar_const_op, float (*out)[kLanes], const uint32_t *comps, uint32_t ncomps, uint32_t need = 0xF, const float **ptrs = nullptr)
    {
      if (ptrs)
        for (uint32_t j = 0; j < ncomps; ++j)
          ptrs[j] = out[j];

      const uint32_t L = lanes_; // (a local here: through the capture the compiler can't rule out aliasing, which blocks vectorizing)
      const uint32_t reg = instr.src_reg(src);
      const uint32_t neg = uint32_t(instr.src_negate(src)) << 31;

      // Two-operand scalar ops (c#.w with r#.x) always read a constant here: src3_sel is then part of the temp register number.
      if (!scalar_const_op && instr.src_is_temp(src))
      {
        const uint32_t t = TempIndex(ucode::AluInstruction::src_temp_reg(reg), ucode::AluInstruction::is_src_temp_relative(reg));
        const bool absolute = ucode::AluInstruction::is_src_temp_value_absolute(reg);
        const uint32_t abs_mask = ~(uint32_t(absolute) << 31);

        for (uint32_t j = 0; j < ncomps; ++j)
        {
          if (!((need >> j) & 1))
            continue;

          const float *p = T(t, comps[j]);

          if (ptrs && !absolute && !neg)
          {
            ptrs[j] = p;
            continue;
          }

          float *o = out[j];

          // Abs and negate (the stored value is already denormal-flushed), branch-free (vectorizes)
          for (uint32_t l = 0; l < L; ++l)
            o[l] = AsFloat((AsUint(p[l]) & abs_mask) ^ neg);
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

    // Vector operation.
    const V vop = instr.vector_opcode();
    const ucode::AluVectorOpcodeInfo &vinfo = ucode::GetAluVectorOpcodeInfo(vop);
    const uint32_t vmask = instr.GetVectorOpResultWriteMask();

    if (vmask || vinfo.changed_state)
    {
      // Component-wise operations only need the written components.
      const bool componentwise = !vinfo.changed_state && (vop == V::kAdd || vop == V::kMul || vop == V::kMad || vop == V::kMax || vop == V::kMin);
      const uint32_t need = componentwise ? vmask : 0xF;
      float ops[3][4][kLanes];
      const float *in[3][4] = {};

      for (uint32_t i = 0; i < 3; ++i)
      {
        if (!vinfo.operand_components_used[i])
          continue;

        const uint32_t swz = instr.src_swizzle(1 + i);
        uint32_t comps[4];

        for (uint32_t j = 0; j < 4; ++j)
          comps[j] = ucode::AluInstruction::GetSwizzledComponentIndex(swz, j);

        gather(1 + i, false, ops[i], comps, 4, need, componentwise ? in[i] : nullptr);
      }

      if (componentwise)
      {
        for (uint32_t j = 0; j < 4; ++j)
        {
          if (!((vmask >> j) & 1))
            continue;

          const float *a = in[0][j], *b = in[1][j], *c = in[2][j];
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

    // Scalar operation.
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
      // c#.w and r#.x (the temp is never relative).
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

    const uint32_t smask = instr.GetScalarOpResultWriteMask();

    // (The scalar result is only read when written somewhere.)
    if (smask)
    {
      if (instr.scalar_clamp())
        for (uint32_t l = 0; l < L; ++l)
          sr[l] = Saturate(ps_[l]);
      else
        std::memcpy(sr, ps_, L * sizeof(float));
    }

    if (instr.vector_clamp())
      for (uint32_t j = 0; j < 4; ++j)
        if ((vmask >> j) & 1)
          for (uint32_t l = 0; l < L; ++l)
            vr[j][l] = Saturate(vr[j][l]);

    if (instr.is_export())
    {
      const uint32_t reg = instr.vector_dest();
      const uint32_t c0 = instr.GetConstant0WriteMask(), c1 = instr.GetConstant1WriteMask();
      const uint32_t mask = vmask | smask | c0 | c1;

      // Per component: the vector result, the scalar result or a constant 0 / 1.
      const float *src[4];
      float fill[4];

      for (uint32_t j = 0; j < 4; ++j)
      {
        const uint32_t bit = 1u << j;
        src[j] = (vmask & bit) ? vr[j] : (smask & bit) ? sr
                                                       : nullptr;
        fill[j] = (c1 & bit) ? 1.0f : 0.0f;
      }

      auto value = [&](uint32_t j, uint32_t l)
      { return src[j] ? src[j][l] : fill[j]; };

      if (reg == uint32_t(ucode::ExportRegister::kExportAddress))
      {
        Flush(); // a new address ends the previous export

        for (uint32_t j = 0; j < 4; ++j)
          for (uint32_t l = 0; l < L; ++l)
            address_[j][l] = AsUint(value(j, l));

        address_valid_ = true;
      }
      else if (reg >= uint32_t(ucode::ExportRegister::kExportData0) && reg <= uint32_t(ucode::ExportRegister::kExportData4))
      {
        const uint32_t k = reg - uint32_t(ucode::ExportRegister::kExportData0);

        for (uint32_t j = 0; j < 4; ++j)
        {
          if (!(mask & (1u << j)))
            continue;

          if (src[j])
            std::memcpy(data_[k][j], src[j], L * sizeof(float));
          else
            std::fill(data_[k][j], data_[k][j] + L, fill[j]);
        }

        written_ |= 1u << k;
      }

      if (reg == uint32_t(ucode::ExportRegister::kVSPosition) && positions_)
      {
        for (uint32_t j = 0; j < 4; ++j)
          if (mask & (1u << j))
            for (uint32_t l = 0; l < L; ++l)
              positions_[4 * l + j] = value(j, l);
      }

      // Interpolators / point size: not needed.
      return;
    }

    if (vmask)
    {
      const uint32_t t = TempIndex(instr.vector_dest(), instr.is_vector_dest_relative());

      for (uint32_t j = 0; j < 4; ++j)
        if (vmask & (1u << j))
          StoreTemp(T(t, j), vr[j], L);
    }

    if (smask)
    {
      const uint32_t t = TempIndex(instr.scalar_dest(), instr.is_scalar_dest_relative());

      for (uint32_t j = 0; j < 4; ++j)
        if (smask & (1u << j))
          StoreTemp(T(t, j), sr, L);
    }
  }

  void Interpreter::StoreFetch(uint32_t dest, bool relative, uint32_t swizzle, const float (*value)[kLanes])
  {
    const uint32_t t = TempIndex(dest, relative);

    for (uint32_t j = 0; j < 4; ++j)
    {
      float *d = T(t, j);
      const uint32_t s = (swizzle >> (3 * j)) & 7;

      if (s <= 3)
        StoreTemp(d, value[s], lanes_);
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
    const bool override = c_.override_data && slot == c_.override_slot;
    const uint64_t buffer_begin = override ? 0 : uint64_t(fc0 & 0x1FFFFFFCu); // bytes
    const uint64_t buffer_end = override ? uint64_t(c_.override_size) : buffer_begin + (fc1 & 0x03FFFFFCu);
    const uint32_t endian = override ? 2u : (fc1 & 3);
    const uint8_t *const source = override ? c_.override_data : membase_;
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

    // 32-bit float formats: plain swapped dwords (the common case here). Half-float formats: halves through DecodeElement's table.
    uint32_t float_words = 0, half_words = 0;

    if (!e.exp_adjust)
    {
      switch (e.format)
      {
      case 36: // k_32_FLOAT
        float_words = 1;
        break;

      case 37: // k_32_32_FLOAT
        float_words = 2;
        break;

      case 57: // k_32_32_32_FLOAT
        float_words = 3;
        break;

      case 38: // k_32_32_32_32_FLOAT
        float_words = 4;
        break;

      case 31: // k_16_16_FLOAT
        half_words = 1;
        break;

      case 32: // k_16_16_16_16_FLOAT
        half_words = 2;
        break;

      default:
        break;
      }
    }

    const bool valid = bytes && (override || (fc0 & 3) == 3);
    float result[4][kLanes];

    // Float / half-float formats: one tight loop per endian mode
    if (valid && (float_words || half_words))
    {
      const uint32_t rows = float_words ? float_words : 2 * half_words;
      const int32_t offset = instr.offset();

      for (uint32_t j = rows; j < 4; ++j)
        std::memset(result[j], 0, L * sizeof(float));

      // Per lane: the element's address, or (out of bounds) a zero element - no branches in the loop
      static const uint8_t kZeroElement[16] = {};
      const uint64_t limit = override ? buffer_end : std::min<uint64_t>(buffer_end, kGuestPhysicalSize);
      uint64_t oob = 0;

      // Two passes: the (scattered) loads per lane, then the swap and conversion over whole rows, which vectorizes
      auto run = [&]<uint32_t kEndian, uint32_t kWords, bool kHalf>()
      {
        alignas(16) uint32_t raw[kWords][kLanes];

        for (uint32_t l = 0; l < L; ++l)
        {
          const uint64_t first = uint64_t(uint32_t(int32_t(vfetch_address_[l]) + offset)) * 4;
          const bool ok = first >= buffer_begin && first + bytes <= limit;
          oob += !ok;
          const uint8_t *p = ok ? source + first : kZeroElement;

          for (uint32_t k = 0; k < kWords; ++k)
            std::memcpy(&raw[k][l], p + 4 * k, 4);
        }

        for (uint32_t k = 0; k < kWords; ++k)
        {
          const uint32_t *in = raw[k];

          if constexpr (kHalf)
          {
            float *lo = result[2 * k], *hi = result[2 * k + 1];

            for (uint32_t l = 0; l < L; ++l)
            {
              const uint32_t v = Swap32T<kEndian>(in[l]);
              lo[l] = AsFloat(HalfToFloatBits(v & 0xFFFF));
              hi[l] = AsFloat(HalfToFloatBits(v >> 16));
            }
          }
          else
          {
            float *out = result[k];

            for (uint32_t l = 0; l < L; ++l)
              out[l] = AsFloat(Swap32T<kEndian>(in[l]));
          }
        }
      };

      auto by_format = [&]<uint32_t kEndian>()
      {
        if (half_words == 1)
          run.template operator()<kEndian, 1, true>();
        else if (half_words == 2)
          run.template operator()<kEndian, 2, true>();
        else if (float_words == 1)
          run.template operator()<kEndian, 1, false>();
        else if (float_words == 2)
          run.template operator()<kEndian, 2, false>();
        else if (float_words == 3)
          run.template operator()<kEndian, 3, false>();
        else
          run.template operator()<kEndian, 4, false>();
      };

      switch (endian)
      {
      case 0:
        by_format.template operator()<0>();
        break;

      case 1:
        by_format.template operator()<1>();
        break;

      case 2:
        by_format.template operator()<2>();
        break;

      default:
        by_format.template operator()<3>();
        break;
      }

      stats_->fetch_oob += oob;

      StoreFetch(instr.dest(), instr.is_dest_relative(), instr.dest_swizzle(), result);
      return;
    }

    for (uint32_t l = 0; l < L; ++l)
    {
      const uint64_t first = uint64_t(uint32_t(int32_t(vfetch_address_[l]) + instr.offset())) * 4;
      uint32_t out[4] = {};

      if (valid && first >= buffer_begin && first + bytes <= buffer_end && (override || first + bytes <= kGuestPhysicalSize))
      {
        const uint8_t *p = source + first;

        if (float_words)
        {
          for (uint32_t w = 0; w < float_words; ++w)
          {
            uint32_t v;
            std::memcpy(&v, p + 4 * w, 4);
            out[w] = Swap32(v, endian);
          }
        }
        else if (half_words)
        {
          for (uint32_t w = 0; w < half_words; ++w)
          {
            uint32_t v;
            std::memcpy(&v, p + 4 * w, 4);
            v = Swap32(v, endian);
            out[2 * w] = HalfToFloatBits(v & 0xFFFF);
            out[2 * w + 1] = HalfToFloatBits(v >> 16);
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

    uint64_t elements = 0, bad_address = 0, out_of_range = 0, bad_format = 0;
    uint32_t last_format = stats_->last_format, last_base = stats_->last_base, last_count = stats_->last_count;

    // Fast path, the usual case: eA = stream constant + index * (0, 1, 0, 0), the same stream in every lane, a 32-bit float format. Then only the index differs per lane; the results are the same as the general loop below
    {
      const uint32_t L = lanes_;
      const uint32_t a0 = address_[0][0], a2 = address_[2][0], a3 = address_[3][0];
      uint32_t differ = 0;

      for (uint32_t l = 1; l < L; ++l)
        differ |= (address_[0][l] ^ a0) | (address_[2][l] ^ a2) | (address_[3][l] ^ a3);

      const uint32_t format = (a2 >> 8) & 0x3F, endian = a2 & 7;

      if (!differ && (a0 >> 30) == 1 && (a2 >> 23) == 0x96 && (a3 >> 23) == 0x96 && (format == 36 || format == 37 || format == 38) && endian <= 3 && !((a2 >> 19) & 1))
      {
        const uint32_t n = format == 36 ? 1 : format == 37 ? 2
                                                           : 4;
        const uint32_t count = a3 & 0x7FFFFF, base = a0 << 2;

        auto write = [&]<uint32_t kEndian, uint32_t kN>()
        {
          for (uint32_t l = 0; l < L; ++l)
          {
            const uint32_t a1 = address_[1][l];

            if ((a1 >> 23) != 0x96)
            {
              ++bad_address;
              continue;
            }

            const uint32_t index = a1 & 0x7FFFFF;
            last_format = format;
            last_base = base;
            last_count = count;

            for (uint32_t k = 0; k < 5; ++k)
            {
              if (!((written >> k) & 1) || index + k >= count)
                continue;

              const uint64_t address = uint64_t(base) + uint64_t(index + k) * 4 * kN;

              if (address + 4 * kN > kGuestPhysicalSize)
              {
                ++out_of_range;
                continue;
              }

              uint32_t v[kN];

              for (uint32_t j = 0; j < kN; ++j)
                v[j] = Swap32T<kEndian>(AsUint(data_[k][j][l]));

              std::memcpy(membase_ + address, v, 4 * kN);
              ++elements;
            }
          }
        };

        auto by_n = [&]<uint32_t kEndian>()
        {
          if (n == 1)
            write.template operator()<kEndian, 1>();
          else if (n == 2)
            write.template operator()<kEndian, 2>();
          else
            write.template operator()<kEndian, 4>();
        };

        switch (endian)
        {
        case 0:
          by_n.template operator()<0>();
          break;

        case 1:
          by_n.template operator()<1>();
          break;

        case 2:
          by_n.template operator()<2>();
          break;

        default:
          by_n.template operator()<3>();
          break;
        }

        stats_->elements += elements;
        stats_->bad_address += bad_address;
        stats_->out_of_range += out_of_range;
        stats_->last_format = last_format;
        stats_->last_base = last_base;
        stats_->last_count = last_count;
        return;
      }
    }

    for (uint32_t l = 0; l < lanes_; ++l)
    {
      const uint32_t eA[4] = {address_[0][l], address_[1][l], address_[2][l], address_[3][l]};

      // eA = stream constant + index * (0, 1, 0, 0): marker bits as Xenia checks.
      if ((eA[0] >> 30) != 1 || (eA[1] >> 23) != 0x96 || (eA[2] >> 23) != 0x96 || (eA[3] >> 23) != 0x96)
      {
        ++bad_address;
        continue;
      }

      const uint32_t format_info = eA[2];
      const uint32_t index = eA[1] & 0x7FFFFF;
      const uint32_t count = eA[3] & 0x7FFFFF;
      const uint32_t base = eA[0] << 2; // dwords -> bytes, drops the marker bits
      const uint32_t endian = format_info & 7;
      last_format = (format_info >> 8) & 0x3F;
      last_base = base;
      last_count = count;
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
            ++out_of_range;
            continue;
          }

          uint8_t *dst = membase_ + address;

          for (uint32_t j = 0; j < n; ++j)
          {
            const uint32_t v = Swap32(AsUint(data_[k][j][l]), endian);
            std::memcpy(dst + 4 * j, &v, 4);
          }

          ++elements;
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
          ++bad_format;
          continue;
        }

        const uint64_t address = uint64_t(base) + uint64_t(index + k) * size;

        if (address + size > kGuestPhysicalSize)
        {
          ++out_of_range;
          continue;
        }

        Swap128(packed, endian);

        // Little-endian dwords = guest byte order; 8 / 16-bit elements take the low bytes of the swapped dword (as Xenia replaces bytes in place).
        std::memcpy(membase_ + address, packed, size);
        ++elements;
      }
    }

    stats_->elements += elements;
    stats_->bad_address += bad_address;
    stats_->out_of_range += out_of_range;
    stats_->bad_format += bad_format;
    stats_->last_format = last_format;
    stats_->last_base = last_base;
    stats_->last_count = last_count;
  }
}