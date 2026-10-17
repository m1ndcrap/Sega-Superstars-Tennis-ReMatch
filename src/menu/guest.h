// The menu code's access to the game: guest memory, the script front end's directory, and the script VM's natives. Guest memory is big-endian; `base` is the start of the guest address space (the hooks' `base` argument).

#pragma once

#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

namespace sst::menu
{
  // Guest memory
  inline uint8_t *GuestPtr(uint8_t *base, uint32_t addr) { return base + addr + rex::memory::detail::PhysicalHostOffset(addr); }

  inline uint32_t Load32(uint8_t *base, uint32_t addr)
  {
    uint32_t v;
    std::memcpy(&v, GuestPtr(base, addr), 4);
    return std::byteswap(v);
  }

  inline void Store32(uint8_t *base, uint32_t addr, uint32_t v)
  {
    v = std::byteswap(v);
    std::memcpy(GuestPtr(base, addr), &v, 4);
  }

  // A block of the guest's system heap (kept for the session); 0 if none is left
  inline uint32_t GuestAlloc(uint32_t size)
  {
    auto *memory = rex::system::kernel_memory();
    return memory ? memory->SystemHeapAlloc(size) : 0;
  }

  // A copy of host data in a new guest block (32-byte rounded); 0 on failure
  inline uint32_t CopyToGuest(uint8_t *base, const void *data, uint32_t size)
  {
    const uint32_t block = GuestAlloc((size + 31) & ~31u);

    if (block)
      std::memcpy(GuestPtr(base, block), data, size);

    return block;
  }

  // The file name part of a guest path ("game:\\RESOURCE\\MENU\\PauseMenu.DAT" -> "pausemenu.dat"), lower case
  inline std::string GuestFileName(uint8_t *base, uint32_t path)
  {
    std::string s;

    for (uint32_t i = 0; i < 260; ++i)
    {
      const char c = char(*GuestPtr(base, path + i));

      if (!c)
        break;

      s += char(std::tolower(uint8_t(c)));
    }

    const size_t slash = s.find_last_of("\\/:");
    return slash == std::string::npos ? s : s.substr(slash + 1);
  }

  // The front end's script directory (front end + 0x488): a binary tree of nodes {+4 left, +8 right, +0x10 name hash, +0x18 module}, as walked by sub_821CCA28. The node for script `hash`, or 0.
  constexpr uint32_t kScriptDirectory = 0x488, kNodeLeft = 4, kNodeRight = 8, kNodeHash = 0x10, kNodeModule = 0x18;

  inline uint32_t FindScriptNode(uint8_t *base, uint32_t front_end, uint32_t hash)
  {
    uint32_t node = Load32(base, front_end + kScriptDirectory);

    for (int guard = 0; node && guard < 64; ++guard)
    {
      const uint32_t key = Load32(base, node + kNodeHash);

      if (key == hash)
        return node;

      node = Load32(base, node + (hash > key ? kNodeRight : kNodeLeft));
    }

    return 0;
  }

  // The VM calls a native (sub_821DFBC0) with r3 = its result {u32 value, u32 type}, r4 = the VM, r5 = native id (group << 8 | index), r8 = argument values (u32 each), r9 = argument types (u8 each), r10 = argument count.
  enum ValueType : uint8_t
  {
    kNil = 0,
    kFloat = 1,
    kInt = 2,
    kHash = 3
  };

  struct NativeCall
  {
    uint8_t *base;
    uint32_t out, values, types, count;

    NativeCall(PPCContext &ctx, uint8_t *b) : base(b), out(ctx.r3.u32), values(ctx.r8.u32), types(ctx.r9.u32), count(ctx.r10.u32) {}

    // Argument i as an integer (floats truncated); 0 when missing
    int Int(uint32_t i) const
    {
      if (i >= count)
        return 0;

      const uint32_t raw = Load32(base, values + 4 * i);

      if (*GuestPtr(base, types + i) == kFloat)
        return int(std::bit_cast<float>(raw));

      return int32_t(raw);
    }

    // Argument i as a raw dword (hashes)
    uint32_t Raw(uint32_t i) const { return i < count ? Load32(base, values + 4 * i) : 0; }

    void Return(uint32_t value, ValueType type) const
    {
      Store32(base, out, value);
      Store32(base, out + 4, type);
    }

    void ReturnInt(int value) const { Return(uint32_t(value), kInt); }
    void ReturnHash(uint32_t value) const { Return(value, kHash); }
  };
}