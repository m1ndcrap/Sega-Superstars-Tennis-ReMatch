// sst_rematch - crash report

#include "debug/crash_log.h"

#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/filesystem.h>
#include <rex/memory.h>
#include <rex/system/thread_state.h>

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sst::debug
{
#if defined(_WIN32)
  namespace
  {
    struct HostFunc
    {
      uintptr_t host;
      uint32_t guest;
    };

    std::vector<HostFunc> g_funcs; // sorted by host address (built at install: the handler must not allocate much)
    LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

    std::string Describe(uintptr_t addr)
    {
      HMODULE module = nullptr;
      char name[MAX_PATH] = "?";

      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(addr), &module) && module)
      {
        GetModuleFileNameA(module, name, MAX_PATH);
        const char *slash = std::max(std::strrchr(name, '\\'), std::strrchr(name, '/'));
        std::string out = fmt::format("{}+0x{:X}", slash ? slash + 1 : name, addr - reinterpret_cast<uintptr_t>(module));

        if (module == GetModuleHandleA(nullptr) && !g_funcs.empty())
        {
          auto it = std::upper_bound(g_funcs.begin(), g_funcs.end(), addr, [](uintptr_t a, const HostFunc &f)
                                     { return a < f.host; });

          if (it != g_funcs.begin())
          {
            --it;
            out += fmt::format(" (in or after sub_{:08X}, +0x{:X})", it->guest, addr - it->host);
          }
        }

        return out;
      }

      return fmt::format("0x{:X}", addr);
    }

    // Reads a big-endian guest word; false when the page is not readable
    bool ReadGuest(uint8_t *base, uint32_t addr, uint32_t &out)
    {
      if (!base || addr < 0x1000)
        return false;

      const uint8_t *p = base + addr + rex::memory::detail::PhysicalHostOffset(addr); // as the recompiled code addresses it
      MEMORY_BASIC_INFORMATION mbi{};

      if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;

      out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
      return true;
    }

    // The guest registers of the crashing thread (the recompiled code keeps them in its PPCContext) and the top of its guest stack
    std::string DescribeGuest()
    {
      auto *ts = rex::runtime::ThreadState::Get();

      if (!ts || !ts->context())
        return "[sst-crash] no guest thread state\n";

      const PPCContext &c = *ts->context();
      const PPCRegister *r[32] = {&c.r0, &c.r1, &c.r2, &c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10, &c.r11, &c.r12, &c.r13, &c.r14, &c.r15,
                                  &c.r16, &c.r17, &c.r18, &c.r19, &c.r20, &c.r21, &c.r22, &c.r23, &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31};
      std::string out = fmt::format("[sst-crash] guest thread {:X}: lr {:08X} ctr {:08X}\n", ts->thread_id(), uint32_t(c.lr), c.ctr.u32);

      for (int i = 0; i < 32; i += 4)
        out += fmt::format("[sst-crash]   r{:<2} {:08X}  r{:<2} {:08X}  r{:<2} {:08X}  r{:<2} {:08X}\n", i, r[i]->u32, i + 1, r[i + 1]->u32, i + 2, r[i + 2]->u32, i + 3, r[i + 3]->u32);

      uint8_t *base = ts->memory() ? ts->memory()->virtual_membase() : nullptr;
      const uint32_t sp = c.r1.u32;

      for (int row = 0; row < 24; ++row)
      {
        std::string line = fmt::format("[sst-crash]   sp+{:03X}:", row * 16);

        for (int col = 0; col < 4; ++col)
        {
          uint32_t v = 0;
          line += ReadGuest(base, sp + row * 16 + col * 4, v) ? fmt::format(" {:08X}", v) : " ????????";
        }

        out += line + "\n";
      }

      // Words at the register values that look like guest heap pointers, virtual or physical (the object being updated, its vtable)
      for (int i = 3; i < 32; ++i)
      {
        const uint32_t a = r[i]->u32;
        uint32_t v0, v1, v2, v3;

        if (a >= 0x40000000 && !(a >= 0x82000000 && a < 0x83000000) && ReadGuest(base, a, v0) && ReadGuest(base, a + 4, v1) && ReadGuest(base, a + 8, v2) && ReadGuest(base, a + 12, v3))
          out += fmt::format("[sst-crash]   [r{}={:08X}] {:08X} {:08X} {:08X} {:08X}\n", i, a, v0, v1, v2, v3);
      }

      return out;
    }

    LONG WINAPI Filter(EXCEPTION_POINTERS *info)
    {
      static volatile LONG once = 0;

      if (InterlockedExchange(&once, 1) == 0 && info && info->ExceptionRecord && info->ContextRecord)
      {
        const EXCEPTION_RECORD &rec = *info->ExceptionRecord;
        std::string report = fmt::format("[sst-crash] exception 0x{:08X} at {}", uint32_t(rec.ExceptionCode), Describe(reinterpret_cast<uintptr_t>(rec.ExceptionAddress)));

        if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2)
          report += fmt::format(" ({} 0x{:X})", rec.ExceptionInformation[0] == 1 ? "write" : rec.ExceptionInformation[0] == 8 ? "execute"
                                                                                                                              : "read",
                                rec.ExceptionInformation[1]);

        report += "\n";
#if defined(_M_X64) || defined(__x86_64__)
        CONTEXT ctx = *info->ContextRecord;

        for (int frame = 0; frame < 32 && ctx.Rip; ++frame)
        {
          report += fmt::format("[sst-crash]   #{:02} {}\n", frame, Describe(uintptr_t(ctx.Rip)));
          DWORD64 image_base = 0;
          PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);

          if (!fn)
          {
            // Leaf function: the return address is at [rsp]
            ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
          }

          void *handler_data = nullptr;
          DWORD64 establisher = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
        }
#endif
        report += DescribeGuest();
        REXLOG_ERROR("{}", report);

        if (FILE *f = std::fopen((rex::filesystem::GetExecutableFolder() / "sst_crash.txt").string().c_str(), "w"))
        {
          std::fputs(report.c_str(), f);
          std::fclose(f);
        }
      }

      return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
    }
  }

  void InstallCrashLog()
  {
    if (g_funcs.empty())
    {
      for (const PPCFuncMapping *m = PPCFuncMappings; m->guest && m->host; ++m)
        g_funcs.push_back({reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest)});

      std::sort(g_funcs.begin(), g_funcs.end(), [](const HostFunc &a, const HostFunc &b)
                { return a.host < b.host; });
    }

    g_previous = SetUnhandledExceptionFilter(Filter);
    REXLOG_INFO("[sst-crash] crash report on ({} recompiled functions)", g_funcs.size());
  }
#else
  void InstallCrashLog() {}
#endif
}