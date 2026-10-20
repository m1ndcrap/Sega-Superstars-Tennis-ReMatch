// sst_rematch - crash report

#include "debug/crash_log.h"
#include "gpu/d3d12_setup.h"

#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/filesystem.h>
#include <rex/memory.h>
#include <rex/system/thread_state.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <exception>
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
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>

#include <rex/exception_handler.h>
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

    // Host call stack from a context (unwind tables; leaf functions via [rsp])
    std::string Stack(CONTEXT ctx)
    {
      std::string out;
#if defined(_M_X64) || defined(__x86_64__)
      for (int frame = 0; frame < 32 && ctx.Rip; ++frame)
      {
        out += fmt::format("[sst-crash]   #{:02} {}\n", frame, Describe(uintptr_t(ctx.Rip)));
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);

        if (!fn)
        {
          ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
          ctx.Rsp += 8;
          continue;
        }

        void *handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
      }
#endif
      return out;
    }

    volatile LONG g_reported = 0;

    void WriteReport(const std::string &report)
    {
      REXLOG_ERROR("{}", report);

      if (FILE *f = std::fopen((rex::filesystem::GetExecutableFolder() / "sst_crash.txt").string().c_str(), "w"))
      {
        std::fputs(report.c_str(), f);
        std::fclose(f);
      }
    }

    // abort() / std::terminate / CRT invalid parameter end the process with a fast fail (0xC0000409), which skips the unhandled
    // exception filter: report the stack of the thread that is about to end it
    void ReportHere(const char *why)
    {
      if (InterlockedExchange(&g_reported, 1) != 0)
        return;

      sst::gpu::DumpDeviceRemoved(); // a lost graphics device ends the process through abort(): log why first

      CONTEXT ctx{};
      RtlCaptureContext(&ctx);
      std::string report = fmt::format("[sst-crash] {} on host thread {}\n", why, GetCurrentThreadId());
      report += Stack(ctx);
      report += DescribeGuest();
      WriteReport(report);
    }

    void OnTerminate()
    {
      std::string why = "std::terminate";

      if (std::exception_ptr e = std::current_exception())
      {
        try
        {
          std::rethrow_exception(e);
        }
        catch (const std::exception &x)
        {
          why = fmt::format("std::terminate: uncaught exception: {}", x.what());
        }
        catch (...)
        {
          why = "std::terminate: uncaught non-standard exception";
        }
      }

      ReportHere(why.c_str());
      std::abort();
    }

    void OnAbortSignal(int)
    {
      ReportHere("abort()");
    }

    void OnInvalidParameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t)
    {
      ReportHere("CRT invalid parameter");
      std::abort();
    }

    LONG WINAPI Filter(EXCEPTION_POINTERS *info)
    {
      if (info && info->ExceptionRecord && info->ContextRecord && InterlockedExchange(&g_reported, 1) == 0)
      {
        const EXCEPTION_RECORD &rec = *info->ExceptionRecord;
        std::string report = fmt::format("[sst-crash] exception 0x{:08X} at {}", uint32_t(rec.ExceptionCode), Describe(reinterpret_cast<uintptr_t>(rec.ExceptionAddress)));

        if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2)
          report += fmt::format(" ({} 0x{:X})", rec.ExceptionInformation[0] == 1 ? "write" : rec.ExceptionInformation[0] == 8 ? "execute"
                                                                                                                              : "read",
                                rec.ExceptionInformation[1]);

        report += fmt::format(" on host thread {}\n", GetCurrentThreadId());
        report += Stack(*info->ContextRecord);
        report += DescribeGuest();
        WriteReport(report);
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

    // A windowed app's stderr goes nowhere: send it to a file, so failed assert() messages (SDK code, plugins) are kept
    {
      const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);

      if (!err || err == INVALID_HANDLE_VALUE)
      {
        FILE *f = nullptr;
        _wfreopen_s(&f, (rex::filesystem::GetExecutableFolder() / "sst_stderr.txt").wstring().c_str(), L"w", stderr);

        if (f)
          setvbuf(stderr, nullptr, _IONBF, 0);
      }
    }

    g_previous = SetUnhandledExceptionFilter(Filter);
    std::set_terminate(OnTerminate);
    std::signal(SIGABRT, OnAbortSignal);
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_abort_behavior(0, _WRITE_ABORT_MSG); // no message box
    REXLOG_INFO("[sst-crash] crash report installed ({} recompiled functions)", g_funcs.size());
  }
#else
  // POSIX: the SDK owns SIGSEGV / SIGILL (guest memory write tracking and MMIO are page faults it resolves), so the report is one more rex::arch::ExceptionHandler, installed after the runtime's own: it only sees faults nothing else handled.
  // It writes the report, puts the default action back and declines, so the faulting instruction runs again and the process ends as it would have
  namespace
  {
    struct HostFunc
    {
      uintptr_t host;
      uint32_t guest;
    };

    std::vector<HostFunc> g_funcs; // sorted by host address (built at install)
    uintptr_t g_exe_base = 0;
    std::atomic<bool> g_reported{false};

    std::string Describe(uintptr_t addr)
    {
      Dl_info info{};

      if (!dladdr(reinterpret_cast<void *>(addr), &info) || !info.dli_fbase)
        return fmt::format("0x{:X}", addr);

      const char *name = info.dli_fname ? info.dli_fname : "?";
      const char *slash = std::strrchr(name, '/');
      std::string out = fmt::format("{}+0x{:X}", slash ? slash + 1 : name, addr - reinterpret_cast<uintptr_t>(info.dli_fbase));

      if (info.dli_sname)
        out += fmt::format(" ({})", info.dli_sname);

      if (reinterpret_cast<uintptr_t>(info.dli_fbase) == g_exe_base && !g_funcs.empty())
      {
        auto it = std::upper_bound(g_funcs.begin(), g_funcs.end(), addr, [](uintptr_t a, const HostFunc &f)
                                   { return a < f.host; });

        // Only inside the recompiled range: the next function bounds it, and past the last one it is ordinary host code
        if (it != g_funcs.begin() && addr - std::prev(it)->host < (it != g_funcs.end() ? 0x40000u : 0x4000u))
        {
          --it;
          out += fmt::format(" (in or after sub_{:08X}, +0x{:X})", it->guest, addr - it->host);
        }
      }

      return out;
    }

    // Reads a big-endian guest word; false when the page is not readable (process_vm_readv reports that instead of faulting)
    bool ReadGuest(uint8_t *base, uint32_t addr, uint32_t &out)
    {
      if (!base || addr < 0x1000)
        return false;

      uint8_t b[4];
      const iovec local = {b, 4};
      const iovec remote = {base + addr + rex::memory::detail::PhysicalHostOffset(addr), 4};

      if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != 4)
        return false;

      out = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
      return true;
    }

    // The guest registers of the crashing thread and the top of its guest stack (as on Windows)
    std::string DescribeGuest()
    {
      auto *ts = rex::runtime::ThreadState::Get();

      if (!ts || !ts->context())
        return "[sst-crash] no guest thread state\n";

      const PPCContext &c = *ts->context();
      const PPCRegister *r[32] = {&c.r0, &c.r1, &c.r2, &c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10, &c.r11, &c.r12, &c.r13, &c.r14, &c.r15, &c.r16, &c.r17, &c.r18, &c.r19, &c.r20, &c.r21, &c.r22, &c.r23, &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31};
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

      for (int i = 3; i < 32; ++i)
      {
        const uint32_t a = r[i]->u32;
        uint32_t v0, v1, v2, v3;

        if (a >= 0x40000000 && !(a >= 0x82000000 && a < 0x83000000) && ReadGuest(base, a, v0) && ReadGuest(base, a + 4, v1) && ReadGuest(base, a + 8, v2) && ReadGuest(base, a + 12, v3))
          out += fmt::format("[sst-crash]   [r{}={:08X}] {:08X} {:08X} {:08X} {:08X}\n", i, a, v0, v1, v2, v3);
      }

      return out;
    }

    // Host call stack of this thread (from the signal handler: the unwinder steps through the signal frame into the faulting code)
    std::string Stack(uintptr_t pc)
    {
      std::string out = pc ? fmt::format("[sst-crash]   at {}\n", Describe(pc)) : std::string();
      void *frames[64];
      const int n = backtrace(frames, 64);
      int first = 0;

      // From a signal handler the first frames are the report itself and the signal trampoline: start at the faulting frame
      for (int i = 0; pc && i < n; ++i)
      {
        if (reinterpret_cast<uintptr_t>(frames[i]) == pc)
        {
          first = i + 1;
          break;
        }
      }

      for (int i = first; i < n; ++i)
        out += fmt::format("[sst-crash]   #{:02} {}\n", i, Describe(reinterpret_cast<uintptr_t>(frames[i])));

      return out;
    }

    void WriteReport(const std::string &report)
    {
      REXLOG_ERROR("{}", report);

      if (FILE *f = std::fopen((rex::filesystem::GetExecutableFolder() / "sst_crash.txt").string().c_str(), "w"))
      {
        std::fputs(report.c_str(), f);
        std::fclose(f);
      }

      if (auto *logger = spdlog::default_logger_raw())
        logger->flush();
    }

    void ReportHere(const char *why)
    {
      if (g_reported.exchange(true))
        return;

      std::string report = fmt::format("[sst-crash] {} on host thread {}\n", why, uint64_t(gettid()));
      report += Stack(0);
      report += DescribeGuest();
      WriteReport(report);
    }

    void OnTerminate()
    {
      std::string why = "std::terminate";

      if (std::exception_ptr e = std::current_exception())
      {
        try
        {
          std::rethrow_exception(e);
        }
        catch (const std::exception &x)
        {
          why = fmt::format("std::terminate: uncaught exception: {}", x.what());
        }
        catch (...)
        {
          why = "std::terminate: uncaught non-standard exception";
        }
      }

      ReportHere(why.c_str());
      std::abort();
    }

    void OnAbortSignal(int)
    {
      ReportHere("abort()");
    }

    bool OnException(rex::arch::Exception *ex, void *)
    {
      if (!g_reported.exchange(true))
      {
        std::string report;

        if (ex->code() == rex::arch::Exception::Code::kAccessViolation)
          report = fmt::format("[sst-crash] access violation ({} 0x{:X})", ex->access_violation_operation() == rex::arch::Exception::AccessViolationOperation::kWrite ? "write" : "read", ex->fault_address());
        else
          report = "[sst-crash] illegal instruction";

        report += fmt::format(" at {} on host thread {}\n", Describe(uintptr_t(ex->pc())), uint64_t(gettid()));
        report += Stack(uintptr_t(ex->pc()));
        report += DescribeGuest();
        WriteReport(report);
      }

      // Nothing handles it: the default action ends the process when the instruction faults again
      struct sigaction dfl = {};
      dfl.sa_handler = SIG_DFL;
      sigaction(SIGSEGV, &dfl, nullptr);
      sigaction(SIGILL, &dfl, nullptr);
      sigaction(SIGBUS, &dfl, nullptr);

      // Queued until the handler returns (the signal is blocked inside it), so the process ends even when the instruction would not fault again (a signal sent with kill)
      raise(ex->code() == rex::arch::Exception::Code::kAccessViolation ? SIGSEGV : SIGILL);
      return false;
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

    Dl_info self{};

    if (dladdr(reinterpret_cast<void *>(&InstallCrashLog), &self))
      g_exe_base = reinterpret_cast<uintptr_t>(self.dli_fbase);

    void *warm[1];
    backtrace(warm, 1); // loads the unwinder now, not inside the signal handler

    rex::arch::ExceptionHandler::Install(OnException, nullptr);
    std::set_terminate(OnTerminate);
    std::signal(SIGABRT, OnAbortSignal);
    REXLOG_INFO("[sst-crash] crash report installed ({} recompiled functions)", g_funcs.size());
  }
#endif
}