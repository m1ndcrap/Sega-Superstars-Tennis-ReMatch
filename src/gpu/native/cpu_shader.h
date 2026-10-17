// A CPU interpreter for Xenos vertex shaders. It runs the game's memory-export shaders (which write vertex data (morphs & particles) to memory instead of drawing) and, for debugging, ordinary vertex shaders. memexport.h schedules the runs...
// Derived from Xenia's ShaderInterpreter and memexport translators (copyright 2022 Ben Vanik, all rights reserved, released under the BSD license (ReXGlue SDK adaptation by Tom Clay, 2026)).

#pragma once

#include "gpu/native/memexport.h"

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/xenos.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sst::native::cpu_shader
{
  namespace ucode = rex::graphics::ucode;
  namespace xenos = rex::graphics::xenos;

  constexpr uint32_t kTempCount = xenos::kMaxShaderTempRegisters; // 64
  constexpr uint32_t kMaxCfSteps = 4096;                          // control flow instructions per invocation
  constexpr uint32_t kGuestPhysicalSize = 0x20000000u;

  // The interpreter runs a vertex shader for up to kLanes vertices at once ("lanes"): every instruction is decoded once and applied to all lanes. Control flow is shared by the lanes, which is exact when it doesn't depend on per-vertex state (the predicate, or a0-relative constants). Shaders that do are run with one lane per batch (see NeedsPerVertexFlow)
  constexpr uint32_t kLanes = 128;

  // CF instruction `cf` of the microcode (false if outside it)
  bool ReadCf(const uint32_t *ucode, uint32_t dwords, uint32_t cf, ucode::ControlFlowInstruction *ci);

  // Whether the shader's control flow depends on per-vertex state (the predicate), which the lanes can't share; also the number of temp registers it can touch (all with relative addressing)
  bool NeedsPerVertexFlow(const uint32_t *ucode, uint32_t dwords, uint32_t *temp_count);

  class Interpreter
  {
  public:
    Interpreter(const uint32_t *ucode, uint32_t dwords, const VsConstants &c, uint8_t *membase, MemexportStats *stats, uint32_t temp_count) : ucode_(ucode), dwords_(dwords), c_(c), membase_(membase), stats_(stats), temps_(size_t(kTempCount) * 4 * kLanes), clear_floats_(size_t(temp_count) * 4 * kLanes)
    {
      //.........
    }

    // Runs `lanes` (<= kLanes) vertices sharing control flow. positions: if set, receives the position export (xyzw) of each lane.
    void Run(const uint32_t *vertex_indices, uint32_t lanes, float *positions = nullptr)
    {
      lanes_ = lanes;
      positions_ = positions;

      if (positions)
        for (uint32_t i = 0; i < 4 * lanes; ++i)
          positions[i] = std::nanf("");

      std::fill(temps_.begin(), temps_.begin() + clear_floats_, 0.0f);

      for (uint32_t l = 0; l < lanes; ++l)
        T(0, 0)
      [l] = float(vertex_indices[l]);

      std::memset(ps_, 0, sizeof(ps_));
      std::memset(a0_, 0, sizeof(a0_));
      std::memset(pred_, 0, sizeof(pred_));
      state_ = {};
      written_ = 0;
      address_valid_ = false;
      Execute();
      Flush();
    }

  private:
    struct State
    {
      uint32_t vfetch_full[3] = {};
      uint32_t call_stack_depth = 0;
      uint32_t call_return[4] = {};
      uint32_t loop_depth = 0;
      xenos::LoopConstant loops[4] = {};
      uint32_t loop_iterators[4] = {};

      int32_t LoopAddress() const
      {
        if (!loop_depth)
          return 0;

        const xenos::LoopConstant &l = loops[loop_depth - 1];
        return std::min(256, std::max(-256, int32_t(loop_iterators[loop_depth - 1]) * l.step + int32_t(l.start)));
      }
    };

    float *T(uint32_t reg, uint32_t comp) { return &temps_[(size_t(reg) * 4 + comp) * kLanes]; }

    uint32_t TempIndex(uint32_t address, bool relative) const
    {
      return uint32_t(int32_t(address) + (relative ? state_.LoopAddress() : 0)) & (kTempCount - 1);
    }

    bool BoolConstant(uint32_t address) const
    {
      return (c_.bool_constants[(address >> 5) & 7] >> (address & 31)) & 1;
    }

    const float *FloatConstant(int32_t index) const
    {
      static const float kZero[4] = {};
      return (index < 0 || index >= 256) ? kZero : c_.float_constants + 4 * index;
    }

    void Execute();
    void ExecuteAlu(const ucode::AluInstruction &instr);
    void ExecuteVfetch(const uint32_t *dwords, uint32_t address);
    void StoreFetch(uint32_t dest, bool relative, uint32_t swizzle, const float (*value)[kLanes]);
    void Flush();

    const uint32_t *ucode_;
    uint32_t dwords_;
    const VsConstants &c_;
    uint8_t *membase_;
    MemexportStats *stats_;

    uint32_t lanes_ = 0;
    float *positions_ = nullptr;

  public:
    // Fetch-index capture: per full vfetch instruction address, the element index each vertex fetched (lane l of the current batch -> base + l).
    std::unordered_map<uint32_t, std::vector<int32_t>> *fetch_records_ = nullptr;
    uint32_t fetch_lane_base_ = 0;

  private:
    std::vector<float> temps_; // [reg][component][lane]
    size_t clear_floats_;      // zeroed per batch: the registers the shader uses
    float ps_[kLanes];         // previous scalar
    int32_t a0_[kLanes];
    bool pred_[kLanes]; // lane 0 steers control flow (one lane if it matters)
    uint32_t vfetch_address_[kLanes];
    State state_;

    // Memory export: eA (raw bits) and eM0-4 per lane; which eM# were written.
    uint32_t address_[4][kLanes];
    bool address_valid_ = false;
    float data_[5][4][kLanes];
    uint32_t written_ = 0;
  };
}