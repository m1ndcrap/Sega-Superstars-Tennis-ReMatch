// The game's script modules. A module starts with a header of offsets: +0 code (0x20), +4 function table, +8 globals, +12 a third table, +28 debug strings (a table of string offsets; the strings end the module). Code addresses are module offsets.

#pragma once

#include "menu/be_bytes.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace sst::menu::script
{
  // The module's size, from its header (the end of the debug strings); 0 if the header does not look like a module
  size_t ModuleSize(const uint8_t *module, size_t max_size);

  // Builds code to be placed at module offset `base`. Jumps and calls to labels are resolved by finish()
  class Asm
  {
  public:
    explicit Asm(uint32_t base) : base_(base) {}

    uint32_t pc() const { return base_ + uint32_t(b_.size()); }

    void u8(uint8_t v) { b_.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v >> 8)), u8(uint8_t(v)); }
    void u32(uint32_t v) { Put32(b_, v); }

    void pushi(int32_t v) { u8(0x01), u32(uint32_t(v)); } // push an integer
    void pushh_raw(uint32_t h) { u8(0x02), u32(h); }      // push a hash (as a number)
    void pushh(uint32_t h) { pushh_raw(h), op(6); }       // push a hash (typed)
    void pushg(uint16_t g) { u8(0x09), u16(g); }          // push global g
    void op(uint8_t o) { u8(0x0D), u8(o); }               // operator o on the stack top
    void native(uint8_t group, uint8_t index, uint8_t argc) { u8(0x12), u8(argc), u16(uint16_t(group << 8 | index)); }
    void push0() { u8(0x17); }
    void pop() { u8(0x18); }
    void ret() { u8(0x1B); }
    void yield() { u8(0x1D); }
    void index() { u8(0x23); }                              // array[index]
    void stabs(int16_t s) { u8(0x27), u16(uint16_t(s)); }   // store to stack slot s
    void pushabs(int16_t s) { u8(0x29), u16(uint16_t(s)); } // push stack slot s
    void setgh(uint32_t h) { u8(0x2D), u32(h); }            // store to the global named h
    void pushgh(uint32_t h) { u8(0x2F), u32(h); }           // push the global named h

    // Call (short form): argc, i32 relative to the next instruction
    void call(uint8_t argc, uint32_t target)
    {
      u8(0x1A), u8(argc);
      u32(uint32_t(int32_t(target) - int32_t(pc() + 4)));
    }

    void call_label(uint8_t argc, int label)
    {
      u8(0x1A), u8(argc);
      calls_.push_back({b_.size(), label});
      u32(0);
    }

    // Jumps: 0x13 jt, 0x14 jf, 0x15 jmp; i16 relative to the next instruction
    void jump(uint8_t opc, int label)
    {
      u8(opc);
      jumps_.push_back({b_.size(), label});
      u16(0);
    }

    void jump_abs(uint8_t opc, uint32_t target)
    {
      u8(opc);
      u16(uint16_t(int16_t(int32_t(target) - int32_t(pc() + 2))));
    }

    int label()
    {
      labels_.push_back(UINT32_MAX);
      return int(labels_.size()) - 1;
    }

    void here(int l) { labels_[l] = pc(); }
    uint32_t address(int label) const { return labels_[label]; }

    // The code, with label references filled in; false if a label was never placed or a jump is out of range
    bool finish(Bytes &out)
    {
      for (auto [at, l] : calls_)
      {
        if (labels_[l] == UINT32_MAX)
          return false;

        Wr32(b_.data() + at, uint32_t(int32_t(labels_[l]) - int32_t(base_ + at + 4)));
      }

      for (auto [at, l] : jumps_)
      {
        if (labels_[l] == UINT32_MAX)
          return false;

        const int32_t rel = int32_t(labels_[l]) - int32_t(base_ + at + 2);

        if (rel < -32768 || rel > 32767)
          return false;

        b_[at] = uint8_t(uint16_t(rel) >> 8);
        b_[at + 1] = uint8_t(rel);
      }

      out = b_;
      return true;
    }

  private:
    uint32_t base_;
    Bytes b_;
    std::vector<uint32_t> labels_;
    std::vector<std::pair<size_t, int>> jumps_, calls_;
  };
}