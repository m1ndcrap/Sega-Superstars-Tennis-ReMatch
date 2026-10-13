// sst_rematch - QUIT: the main menu script patch

#include "menu/main_menu_quit.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <fmt/format.h>

namespace sst::menu::quit
{
  namespace
  {
    uint32_t Rd32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    int32_t S32(uint32_t v) { return int32_t(v); }

    void Wr32(uint8_t *p, uint32_t v)
    {
      p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
    }

    void Put32(Bytes &b, uint32_t v)
    {
      b.push_back(uint8_t(v >> 24)), b.push_back(uint8_t(v >> 16)), b.push_back(uint8_t(v >> 8)), b.push_back(uint8_t(v));
    }

    // Script bytecode (opcodes: claude/ingame-menu-re.md, tools/sbn_asm.py); addresses are module offsets
    class Asm
    {
    public:
      explicit Asm(uint32_t base) : base_(base) {}
      uint32_t pc() const { return base_ + uint32_t(b_.size()); }
      void u8(uint8_t v) { b_.push_back(v); }
      void u16(uint16_t v) { u8(uint8_t(v >> 8)), u8(uint8_t(v)); }
      void u32(uint32_t v) { Put32(b_, v); }
      void pushi(int32_t v) { u8(0x01), u32(uint32_t(v)); }
      void pushh_raw(uint32_t h) { u8(0x02), u32(h); }
      void pushg(uint16_t g) { u8(0x09), u16(g); }
      void op(uint8_t o) { u8(0x0D), u8(o); }
      void native(uint8_t group, uint8_t index, uint8_t argc) { u8(0x12), u8(argc), u16(uint16_t(group << 8 | index)); }
      void push0() { u8(0x17); }
      void pop() { u8(0x18); }
      void ret() { u8(0x1B); }
      void yield() { u8(0x1D); }
      void index() { u8(0x23); }
      void stabs(int16_t s) { u8(0x27), u16(uint16_t(s)); }
      void pushabs(int16_t s) { u8(0x29), u16(uint16_t(s)); }
      void setgh(uint32_t h) { u8(0x2D), u32(h); }
      void pushgh(uint32_t h) { u8(0x2F), u32(h); }
      // call (short form): argc, i32 relative to the next instruction
      void call(uint8_t argc, uint32_t target)
      {
        u8(0x1A), u8(argc);
        u32(uint32_t(int32_t(target) - int32_t(pc() + 4)));
      }

      // call to a label in this code
      void call_label(uint8_t argc, int label)
      {
        u8(0x1A), u8(argc);
        calls_.push_back({b_.size(), label});
        u32(0);
      }

      void jump_abs(uint8_t opc, uint32_t target)
      {
        u8(opc);
        u16(uint16_t(int16_t(int32_t(target) - int32_t(pc() + 2))));
      }

      uint32_t address(int label) const { return labels_[label]; }

      // 0x13 jt, 0x14 jf, 0x15 jmp: i16 relative to the next instruction
      void jump(uint8_t opc, int label)
      {
        u8(opc);
        fix_.push_back({b_.size(), label});
        u16(0);
      }

      int label()
      {
        labels_.push_back(UINT32_MAX);
        return int(labels_.size()) - 1;
      }

      void here(int l) { labels_[l] = pc(); }

      bool finish(Bytes &out)
      {
        for (auto [at, l] : calls_)
        {
          if (labels_[l] == UINT32_MAX)
            return false;

          Wr32(b_.data() + at, uint32_t(int32_t(labels_[l]) - int32_t(base_ + at + 4)));
        }

        for (auto [at, l] : fix_)
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
      std::vector<std::pair<size_t, int>> fix_;
      std::vector<std::pair<size_t, int>> calls_;
    };

    // mainmenu facts (Xbox)
    constexpr int16_t kSlotLevel = 12, kSlotCursor = 13, kSlotTimer = 15, kSlotLeave = 25; // level: 1 category list, 0 sub-items
    constexpr uint16_t kGlobalActions = 7;
    constexpr int kRowIndex = 6; // the new category (after OPTIONS, index 5)
    // Message box (script "msgbox"): message to show (cleared when dismissed), its flags, the chosen button
    constexpr uint32_t kMsgId = 0x1473B103, kMsgChoice = 0x65AEA351;
    constexpr std::pair<uint32_t, int32_t> kMsgFlags[] = {{0x94557658, 1}, {0xDD766DAB, 1}, {0xD0CA61AA, 1}, {0x100F3079, -1}};

    // The category list's "select" code for a row without sub-items:
    //   push0; pushg 7; pushabs 13; index; pushi 0; index; call 1 -> action(); pop; pushi 0; stabs 15; pushi 1; stabs 25
    // (bytes 16-23 of the call: argc, i32 target, debug line)
    constexpr uint8_t kSelectSig[] = {0x17, 0x09, 0x00, 0x07, 0x29, 0x00, 0x0D, 0x23, 0x01, 0x00, 0x00, 0x00, 0x00, 0x23, 0x34, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0x18, 0x01, 0x00, 0x00, 0x00, 0x00, 0x27, 0x00, 0x0F, 0x01, 0x00, 0x00, 0x00, 0x01, 0x27, 0x00, 0x19};
    constexpr size_t kSelectWild0 = 16, kSelectWild1 = 24; // call target + debug line

    size_t ModuleSize(const uint8_t *m, size_t max_size)
    {
      if (max_size < 0x20)
        return 0;

      const uint32_t code = Rd32(m), functab = Rd32(m + 4), t2 = Rd32(m + 8), t3 = Rd32(m + 12), debug = Rd32(m + 28);

      if (code != 0x20 || !(code <= functab && functab <= t2 && t2 <= t3 && t3 <= debug) || debug + 4 > max_size)
        return 0;

      const uint32_t first = Rd32(m + debug);

      if (first < debug + 4 || ((first - debug) & 3) || first - debug > 64)
        return 0;

      size_t end = first;

      for (uint32_t p = debug; p < first; p += 4)
      {
        size_t s = Rd32(m + p);

        while (s < max_size && m[s])
          ++s;

        if (s >= max_size)
          return 0;

        end = std::max(end, s + 1);
      }

      return (end + 3) & ~size_t(3);
    }
  }

  std::optional<Bytes> PatchScript(const uint8_t *m, size_t size, std::string &error)
  {
    const size_t msize = ModuleSize(m, size);

    if (!msize)
      return error = "not a script module", std::nullopt;

    const uint32_t functab = Rd32(m + 4), t2 = Rd32(m + 8), t3 = Rd32(m + 12), debug = Rd32(m + 28);

    if ((functab | t2 | t3 | debug) & 3)
      return error = "module layout", std::nullopt;

    const uint32_t nglob = Rd32(m + t2);

    if (nglob < 11 || t2 + 4 + 5 * size_t(nglob) > t3)
      return error = "globals", std::nullopt;
    // Arrays of the category tables: the expected sizes (6 categories; table 10: two labels per category with sub-items)
    struct Arr
    {
      uint32_t slot, obj, count, elems;
    };

    auto arr = [&](uint32_t g, Arr &a) -> bool
    {
      a.slot = t2 + 4 + 4 * g;

      if (m[t2 + 4 + 4 * nglob + g] != 0x86)
        return false;

      a.obj = uint32_t(int32_t(a.slot) + S32(Rd32(m + a.slot)));

      if (a.obj < t2 || a.obj + 0x14 > t3 || Rd32(m + a.obj + 4) != 0xFFFF8800)
        return false;

      a.count = Rd32(m + a.obj + 8);
      a.elems = uint32_t(int32_t(a.obj) + S32(Rd32(m + a.obj + 12)));
      return a.elems >= t2 && a.elems + 8 * size_t(a.count) <= t3;
    };

    struct Change
    {
      uint16_t global;
      uint32_t expect_count;
      std::vector<std::pair<uint32_t, uint32_t>> add; // value, type (type 0x86: built below)
    };

    // New elements. Types: 0 nil, 3 hash, 0x86 array (relative)
    const std::vector<Change> changes = {
        {2, 6, {{kRow, 3}}},
        {3, 6, {{kHighlight, 3}}},
        {4, 6, {{kTextDescription, 3}}},
        {5, 6, {{kBarLabel, 3}}},
        {6, 6, {{0, 0}}},
        {7, 6, {{0, 0x86}}}, // [kAction]
        {8, 6, {{0, 0}}},
        {10, 10, {{kOptionsFrom, 3}, {kOptionsTo, 3}}}, // OPTIONS (category 5) pushes the new row down
    };
    std::vector<Arr> arrays;

    for (const Change &c : changes)
    {
      Arr a;

      if (!arr(c.global, a) || a.count != c.expect_count)
        return error = fmt::format("table {} (count {})", c.global, a.count), std::nullopt;

      arrays.push_back(a);
    }

    // The select code
    uint32_t site = 0, action_fn = 0;
    int found = 0;

    for (uint32_t p = 0x20; p + sizeof(kSelectSig) <= functab; ++p)
    {
      bool ok = true;

      for (size_t i = 0; i < sizeof(kSelectSig) && ok; ++i)
        ok = (i >= kSelectWild0 && i < kSelectWild1) || m[p + i] == kSelectSig[i];

      if (ok)
      {
        site = p;
        ++found;
      }
    }

    if (found != 1)
      return error = fmt::format("select code found {} times", found), std::nullopt;

    {
      const uint32_t call_at = site + 14; // 0x34 argc i32 debug(2x i16): next instruction at +10
      action_fn = uint32_t(int32_t(call_at + 10) + S32(Rd32(m + call_at + 2)));

      if (action_fn < 0x20 || action_fn >= functab)
        return error = "action function", std::nullopt;
    }

    // The main loop's end: pushi 0; yield (debug form, 5 bytes); pop (the frame's input event); jmp -> loop start (pushabs 25; op not; jf)
    uint32_t loop_end = 0, loop_start = 0;
    {
      constexpr uint8_t kLoopStart[] = {0x29, 0x00, 0x19, 0x0D, 0x07, 0x14};
      int starts = 0;

      for (uint32_t p = 0x20; p + sizeof(kLoopStart) <= functab; ++p)
        if (std::memcmp(m + p, kLoopStart, sizeof(kLoopStart)) == 0)
          loop_start = p, ++starts;

      if (starts != 1)
        return error = fmt::format("main loop found {} times", starts), std::nullopt;

      int ends = 0;

      for (uint32_t p = 0x20; p + 14 <= functab; ++p)
      {
        const uint8_t *q = m + p;

        if (q[0] == 0x01 && Rd32(q + 1) == 0 && q[5] == 0x33 && q[10] == 0x18 && q[11] == 0x15 && int32_t(p + 14) + int16_t(uint16_t(q[12] << 8 | q[13])) == int32_t(loop_start))
          loop_end = p, ++ends;
      }

      if (ends != 1)
        return error = fmt::format("main loop end found {} times", ends), std::nullopt;
    }

    // Code added after the game's code:
    //   ask:    the YES / NO message; YES quits
    //   select: replaces the select code: the new row asks, any other row runs the original code
    //   frame:  called at the end of every main loop frame: Esc on the category list (abs 12 = 1) asks too; then the frame's yield
    Asm a(functab);
    const int ask = a.label(), wait = a.label(), closed = a.label(), no = a.label(), other = a.label(), frame = a.label(), no_esc = a.label();
    // select
    a.pushabs(kSlotCursor);
    a.pushi(kRowIndex);
    a.op(20); // eq
    a.jump(0x14, other);
    a.push0();
    a.call_label(0, ask);
    a.pop();
    a.push0();
    a.ret();
    a.here(other);
    a.push0();
    a.pushg(kGlobalActions);
    a.pushabs(kSlotCursor);
    a.index();
    a.pushi(0);
    a.index();
    a.call(1, action_fn);
    a.pop();
    a.pushi(0);
    a.stabs(kSlotTimer);
    a.pushi(1);
    a.stabs(kSlotLeave);
    a.push0();
    a.ret();
    // ask
    a.here(ask);
    a.native(0x53, uint8_t(kNativeDialogBegin), 0);
    a.pop();
    a.pushh_raw(kMessage);
    a.setgh(kMsgId);

    for (auto [g, v] : kMsgFlags)
      a.pushi(v), a.setgh(g);

    a.here(wait);
    a.pushgh(kMsgId);
    a.jump(0x14, closed);
    a.native(3, 11, 0);
    a.pop();
    a.pushi(0);
    a.yield();
    a.jump(0x15, wait);
    a.here(closed);
    a.native(0x53, uint8_t(kNativeDialogEnd), 0);
    a.pop();
    a.pushgh(kMsgChoice);
    a.pushi(1); // YES (buttons: NO, YES)
    a.op(20);
    a.jump(0x14, no);
    a.native(0x53, uint8_t(kNativeQuit), 0);
    a.pop();
    a.here(no);
    a.push0();
    a.ret();
    // frame
    a.here(frame);
    a.pushabs(kSlotLevel);
    a.native(0x53, uint8_t(kNativeEscape), 1);
    a.jump(0x14, no_esc);
    a.push0();
    a.call_label(0, ask);
    a.pop();
    a.here(no_esc);
    a.pushi(0);
    a.yield();
    a.push0();
    a.ret();
    Bytes fn;

    if (!a.finish(fn))
      return error = "function assembly", std::nullopt;

    const uint32_t frame_fn = a.address(frame);

    while (fn.size() & 3)
      fn.push_back(0x11); // nop

    const uint32_t d1 = uint32_t(fn.size());
    // New module: [0, functab) code | function | [functab, t3) function table and globals | new arrays | [t3, end) symbols and debug info
    Bytes out(m, m + functab);
    out.insert(out.end(), fn.begin(), fn.end());
    out.insert(out.end(), m + functab, m + t3);
    const uint32_t block = uint32_t(out.size());
    // [kAction]: elements, then the object
    const uint32_t action_elems = uint32_t(out.size());
    Put32(out, kAction);
    Put32(out, 3);
    const uint32_t action_obj = uint32_t(out.size());
    Put32(out, 0);
    Put32(out, 0xFFFF8800);
    Put32(out, 1);
    Put32(out, uint32_t(int32_t(action_elems) - int32_t(action_obj)));
    Put32(out, 0x80000001);

    for (size_t i = 0; i < changes.size(); ++i)
    {
      const Arr &old = arrays[i];
      const Change &c = changes[i];
      const uint32_t n = old.count + uint32_t(c.add.size());
      const uint32_t elems = uint32_t(out.size());

      for (uint32_t e = 0; e < old.count; ++e)
      {
        const uint32_t ea = old.elems + 8 * e;
        uint32_t v = Rd32(m + ea);
        const uint32_t type = Rd32(m + ea + 4);

        if ((type & 0x7F) == 5 || (type & 0x7F) == 6)
        {
          // Relative: to the same target (moved by d1 when inside the globals block)
          uint32_t target = uint32_t(int32_t(ea) + S32(v));

          if (target >= functab && target < t3)
            target += d1;

          v = uint32_t(int32_t(target) - int32_t(uint32_t(out.size())));
        }

        Put32(out, v);
        Put32(out, type);
      }

      for (auto [v, type] : c.add)
      {
        if (type == 0x86)
          v = uint32_t(int32_t(action_obj) - int32_t(uint32_t(out.size())));

        Put32(out, v);
        Put32(out, type);
      }

      const uint32_t obj = uint32_t(out.size());
      Put32(out, 0);
      Put32(out, 0xFFFF8800);
      Put32(out, n);
      Put32(out, uint32_t(int32_t(elems) - int32_t(obj)));
      Put32(out, 0x80000000u | n);
      // The global now points at the new array
      const uint32_t slot = old.slot + d1;
      Wr32(out.data() + slot, uint32_t(int32_t(obj) - int32_t(slot)));
    }

    const uint32_t d2 = uint32_t(out.size()) - block;
    out.insert(out.end(), m + t3, m + msize);
    // The main loop's frame end calls "frame" instead of yielding itself
    {
      Asm s(loop_end);
      s.push0();
      s.call(0, frame_fn);
      s.pop(); // frame's result
      s.pop(); // the frame's input event (as the original code)
      s.jump_abs(0x15, loop_start);
      Bytes code;

      if (!s.finish(code) || code.size() > 14)
        return error = "loop patch", std::nullopt;

      code.resize(14, 0x11); // nop
      std::copy(code.begin(), code.end(), out.begin() + loop_end);
    }
    // The select code calls "select" (the start of the added code) instead
    {
      Asm s(site);
      s.push0();
      s.call(0, functab);
      s.pop();
      Bytes code;

      if (!s.finish(code) || code.size() > sizeof(kSelectSig))
        return error = "select patch", std::nullopt;

      code.resize(sizeof(kSelectSig), 0x11); // nop
      std::copy(code.begin(), code.end(), out.begin() + site);
    }
    // Header and debug info pointers
    Wr32(out.data() + 4, functab + d1);
    Wr32(out.data() + 8, t2 + d1);
    Wr32(out.data() + 12, t3 + d1 + d2);
    Wr32(out.data() + 28, debug + d1 + d2);
    const uint32_t new_debug = debug + d1 + d2;
    const uint32_t first = Rd32(m + debug);

    for (uint32_t p = 0; p < first - debug; p += 4)
      Wr32(out.data() + new_debug + p, Rd32(m + debug + p) + d1 + d2);

    return out;
  }
}