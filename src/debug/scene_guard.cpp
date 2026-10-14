// sst_rematch - guard for the scene manager's update list

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <cstdint>
#include <fmt/format.h>
#include <string>

namespace
{
  constexpr uint32_t kNodeOffset = 0x24; // node inside the object
  constexpr uint32_t kMaxNodes = 8192;

  // Guest memory as the recompiled code addresses it (the 0xE0000000+ physical window sits 0x1000 bytes further into host memory)
  uint8_t *GuestPtr(uint8_t *base, uint32_t a)
  {
    return base + a + rex::memory::detail::PhysicalHostOffset(a);
  }

  uint32_t Load32(uint8_t *base, uint32_t a)
  {
    const uint8_t *p = GuestPtr(base, a);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  void Store32(uint8_t *base, uint32_t a, uint32_t v)
  {
    uint8_t *p = GuestPtr(base, a);
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
  }

  // Guest heap memory (virtual 0x40000000+, physical 0xA0000000+), word aligned, not the code/data image
  bool HeapPointer(uint32_t a)
  {
    return a >= 0x40000000 && (a & 3) == 0 && !(a >= 0x82000000 && a < 0x83000000) && a < 0xFFFF0000;
  }

  // A scene object: its vtable lies in the image and its +0x20 entry is code
  bool LooksLikeSceneObject(uint8_t *base, uint32_t obj)
  {
    if (!HeapPointer(obj))
      return false;

    const uint32_t vtable = Load32(base, obj);

    if (vtable < 0x82000000 || vtable >= 0x83000000 || (vtable & 3))
      return false;

    const uint32_t update = Load32(base, vtable + 0x20);
    return update >= 0x82000000 && update < 0x83000000 && (update & 3) == 0;
  }

  std::string Dump(uint8_t *base, uint32_t a, uint32_t words)
  {
    std::string s;

    for (uint32_t i = 0; i < words; ++i)
      s += fmt::format("{}{:08X}", i ? " " : "", Load32(base, a + 4 * i));

    return s;
  }

  std::atomic<uint32_t> g_reports{0};

  void CheckUpdateList(uint8_t *base, uint32_t manager)
  {
    if (!HeapPointer(manager) && !(manager >= 0x82000000 && manager < 0x83000000))
      return;

    const uint32_t list = manager + 8;
    uint32_t prev = 0;
    uint32_t node = Load32(base, list);

    for (uint32_t n = 0; node && n < kMaxNodes; ++n)
    {
      if (!HeapPointer(node))
      {
        // The links themselves are bad: cut the list here (the rest is lost, as the update would crash on it)
        if (g_reports.fetch_add(1) < 16)
          REXLOG_ERROR("[sst-scene] update list: node #{} is {:08X} (previous {:08X}) - list cut there", n, node, prev);

        if (prev)
          Store32(base, prev + 4, 0);
        else
          Store32(base, list, 0);

        Store32(base, list + 4, prev);
        return;
      }

      const uint32_t next = Load32(base, node + 4);
      const uint32_t owner = Load32(base, node);
      const uint32_t obj = node - kNodeOffset;

      if (owner != obj)
      {
        const bool alive = LooksLikeSceneObject(base, obj);

        if (g_reports.fetch_add(1) < 16)
          REXLOG_ERROR("[sst-scene] update list: node {:08X} (#{}) holds {:08X} instead of its object {:08X} ({}); object: {} | node links: next {:08X} prev {:08X} | before: {}",
                       node, n, owner, obj, alive ? "repaired" : "unlinked", Dump(base, obj, 16), next, Load32(base, node + 8), Dump(base, obj - 0x20, 8));

        if (alive)
          Store32(base, node, obj);
        else
        {
          // Out of the list without writing to the node (its memory may belong to something else now)
          if (prev)
            Store32(base, prev + 4, next);
          else
            Store32(base, list, next);

          if (next && HeapPointer(next))
            Store32(base, next + 8, prev);
          else
            Store32(base, list + 4, prev);

          node = next;
          continue;
        }
      }

      prev = node;
      node = next;
    }
  }
}

// Scene manager update: the listed objects' vfunc +0x20
extern "C" void __imp__sub_82581DE8(PPCContext &__restrict, uint8_t *);
extern "C" REX_FUNC(sub_82581DE8)
{
  CheckUpdateList(base, ctx.r3.u32);
  __imp__sub_82581DE8(ctx, base);
}