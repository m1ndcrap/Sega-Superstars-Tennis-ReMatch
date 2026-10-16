// sst_rematch - game-thread sampling profiler. See hitch_profiler.h

#include "gpu/native/hitch_profiler.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include <psapi.h> // K32EnumProcessModules (kernel32, no psapi.lib needed)

REXCVAR_DEFINE_BOOL(sst_hitch_profiler, false, "SST",
                    "Debug: sample the game thread ~1000x/s and log where slow game frames (and "
                    "the game thread in general) spend their time");

REXCVAR_DEFINE_INT32(sst_hitch_threshold_ms, 20, "SST",
                     "Debug (--sst_hitch_profiler): game frames longer than this are reported");

REXCVAR_DECLARE(int32_t, sst_native_log_every);

namespace sst::native::hitch
{
  namespace
  {
    constexpr size_t kStackCopy = 8192;
    constexpr uint32_t kRing = 16384; // ~16 s of samples
    constexpr uint32_t kFrames = 16;  // addresses kept per sample

    struct Sample
    {
      int64_t qpc = 0;
      uint32_t count = 0;
      uint64_t pc[kFrames] = {}; // [0] = instruction pointer, then stack hits
    };

    struct Window
    {
      int64_t begin, end;
      double ms;
      uint64_t frame;
    };

    // A loaded DLL: code sections and (lazily) its exports, for naming frames outside the exe
    struct Module
    {
      uintptr_t base = 0, end = 0;
      std::string name;
      bool exports_loaded = false;
      std::vector<std::pair<uint32_t, std::string>> exports; // rva -> name, sorted
    };

    struct CodeRange
    {
      uintptr_t begin, end;
      uint32_t module;
    };

    struct Profiler
    {
      HANDLE thread = nullptr; // the game thread
      std::thread sampler;
      std::atomic<bool> quit{false};

      // Executable code range and guest function starts (host address -> guest)
      uintptr_t exe_begin = 0, exe_end = 0;
      std::vector<std::pair<uintptr_t, uint32_t>> funcs;

      // Other modules (sampler thread only)
      std::vector<Module> modules;
      std::vector<CodeRange> code; // executable sections of `modules`, sorted
      int64_t modules_refreshed = 0;

      // Sample ring (written by the sampler only)
      std::vector<Sample> ring = std::vector<Sample>(kRing);
      std::atomic<uint64_t> written{0};

      // Requests from the game thread
      std::mutex mutex;
      std::vector<Window> slow;

      struct Period
      {
        int64_t begin, end;
        uint64_t frames;
      };

      std::vector<Period> periods;
      int64_t period_begin = 0;
      uint64_t period_frames = 0;

      // Game-thread side
      int64_t last_swap = 0;
      uint64_t frame = 0, logged_slow = 0;
      double qpc_ms = 0;
    };

    Profiler *g_prof = nullptr;

    int64_t Qpc()
    {
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return t.QuadPart;
    }

    // Guest function containing host address `pc`, or 0. Uses the exe's unwind table for the exact function start, so host code
    // linked into the exe (hooks, SDK) is not taken for the guest function laid out before it
    uint32_t GuestFunction(const Profiler &p, uintptr_t pc, uintptr_t *host_start = nullptr)
    {
      if (pc < p.exe_begin || pc >= p.exe_end || p.funcs.empty())
        return 0;

      DWORD64 image = 0;

      if (const RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(DWORD64(pc), &image, nullptr))
      {
        const uintptr_t start = uintptr_t(image) + rf->BeginAddress;
        auto it = std::lower_bound(p.funcs.begin(), p.funcs.end(), start, [](const auto &f, uintptr_t v)
                                   { return f.first < v; });

        if (it != p.funcs.end() && it->first == start)
          return it->second;

        if (host_start)
          *host_start = start;

        return 0;
      }

      auto it = std::upper_bound(p.funcs.begin(), p.funcs.end(), pc, [](uintptr_t v, const auto &f)
                                 { return v < f.first; });

      if (it == p.funcs.begin())
        return 0;

      --it;

      if (pc - it->first > 0x40000)
        return 0; // too far: not recompiled code

      return it->second;
    }

    std::string ModuleName(uintptr_t pc)
    {
      HMODULE module = nullptr;

      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(pc), &module) || !module)
        return "?";

      wchar_t path[MAX_PATH];
      const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
      std::wstring w(path, n);
      const size_t slash = w.find_last_of(L"\\/");

      if (slash != std::wstring::npos)
        w = w.substr(slash + 1);

      return std::string(w.begin(), w.end());
    }

    // Re-reads the loaded module list (new DLLs only), at most every 2 s
    void RefreshModules(Profiler &p, int64_t now, int64_t qpc_2s)
    {
      if (p.modules_refreshed && now - p.modules_refreshed < qpc_2s)
        return;

      p.modules_refreshed = now;
      HMODULE handles[1024];
      DWORD needed = 0;

      if (!K32EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed))
        return;

      const size_t count = std::min<size_t>(needed / sizeof(HMODULE), std::size(handles));
      bool added = false;

      for (size_t i = 0; i < count; ++i)
      {
        const uintptr_t base = uintptr_t(handles[i]);

        if (base == p.exe_begin || std::any_of(p.modules.begin(), p.modules.end(), [&](const Module &m)
                                               { return m.base == base; }))
          continue;

        const auto *b = reinterpret_cast<const uint8_t *>(base);
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(b);

        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
          continue;

        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(b + dos->e_lfanew);

        if (nt->Signature != IMAGE_NT_SIGNATURE)
          continue;

        Module m;
        m.base = base;
        m.end = base + nt->OptionalHeader.SizeOfImage;
        m.name = ModuleName(base);
        const uint32_t index = uint32_t(p.modules.size());
        const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);

        for (WORD k = 0; k < nt->FileHeader.NumberOfSections; ++k)
        {
          if (sec[k].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            p.code.push_back({base + sec[k].VirtualAddress, base + sec[k].VirtualAddress + std::max(sec[k].Misc.VirtualSize, sec[k].SizeOfRawData), index});
        }

        p.modules.push_back(std::move(m));
        added = true;
      }

      if (added)
        std::sort(p.code.begin(), p.code.end(), [](const CodeRange &a, const CodeRange &c)
                  { return a.begin < c.begin; });
    }

    // Module index if `v` points into the code of a known DLL, else -1
    int CodeModule(const Profiler &p, uintptr_t v)
    {
      auto it = std::upper_bound(p.code.begin(), p.code.end(), v, [](uintptr_t x, const CodeRange &r)
                                 { return x < r.begin; });

      if (it == p.code.begin())
        return -1;

      --it;
      return v < it->end ? int(it->module) : -1;
    }

    void LoadExports(Module &m)
    {
      m.exports_loaded = true;
      const auto *b = reinterpret_cast<const uint8_t *>(m.base);
      const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(b + reinterpret_cast<const IMAGE_DOS_HEADER *>(b)->e_lfanew);
      const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

      if (!dir.VirtualAddress || !dir.Size)
        return;

      const auto *ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(b + dir.VirtualAddress);
      const auto *functions = reinterpret_cast<const DWORD *>(b + ex->AddressOfFunctions);
      const auto *names = reinterpret_cast<const DWORD *>(b + ex->AddressOfNames);
      const auto *ordinals = reinterpret_cast<const WORD *>(b + ex->AddressOfNameOrdinals);

      for (DWORD i = 0; i < ex->NumberOfNames; ++i)
      {
        if (ordinals[i] >= ex->NumberOfFunctions)
          continue;

        const DWORD rva = functions[ordinals[i]];

        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size)
          continue; // forwarder

        m.exports.emplace_back(rva, reinterpret_cast<const char *>(b + names[i]));
      }

      std::sort(m.exports.begin(), m.exports.end());
    }

    // "sub_XXXXXXXX" (guest), "exe+0xRVA" (host function in the exe; look it up in sst_rematch.map) or "module!NearestExport"
    std::string Label(Profiler &p, uintptr_t pc, std::unordered_map<uintptr_t, std::string> &cache)
    {
      if (auto it = cache.find(pc); it != cache.end())
        return it->second;

      std::string out;
      uintptr_t host = 0;

      if (const uint32_t g = GuestFunction(p, pc, &host))
      {
        out = fmt::format("sub_{:08X}", g);
      }
      else if (pc >= p.exe_begin && pc < p.exe_end)
      {
        out = host ? fmt::format("exe+0x{:X}", host - p.exe_begin) : fmt::format("exe@0x{:X}", pc - p.exe_begin);
      }
      else if (const int mi = CodeModule(p, pc); mi >= 0)
      {
        Module &m = p.modules[mi];

        if (!m.exports_loaded)
          LoadExports(m);

        const uint32_t rva = uint32_t(pc - m.base);
        auto it = std::upper_bound(m.exports.begin(), m.exports.end(), rva, [](uint32_t v, const auto &e)
                                   { return v < e.first; });

        if (it == m.exports.begin())
          out = m.name;
        else
          out = m.name + "!" + std::prev(it)->second;
      }
      else
      {
        out = ModuleName(pc);
      }

      cache.emplace(pc, out);
      return out;
    }

    // Aggregates the samples in [begin, end) into "where (from)" shares
    // detail: only samples whose leaf is outside guest code, labelled with the host call chain (leaf <- callers, up to the first
    // guest function), to see what the system / runtime calls are doing and who makes them
    std::string Describe(Profiler &p, int64_t begin, int64_t end, uint32_t top, uint32_t *total, bool detail = false)
    {
      std::unordered_map<std::string, uint32_t> counts;
      std::unordered_map<uintptr_t, std::string> module_cache;
      std::unordered_map<uintptr_t, std::string> labels;
      uint32_t n = 0;
      const uint64_t w = p.written.load(std::memory_order_acquire);
      const uint64_t first = w > kRing ? w - kRing : 0;

      for (uint64_t i = first; i < w; ++i)
      {
        const Sample &s = p.ring[i % kRing];

        if (s.qpc < begin || s.qpc >= end || !s.count)
          continue;

        const uint32_t leaf = GuestFunction(p, s.pc[0]);

        if (detail)
        {
          if (leaf)
            continue;

          ++n;
          std::string chain = Label(p, uintptr_t(s.pc[0]), labels);
          std::string last = chain;
          uint32_t links = 0;

          for (uint32_t k = 1; k < s.count && links < 4; ++k)
          {
            const std::string l = Label(p, uintptr_t(s.pc[k]), labels);

            if (l == last)
              continue;

            chain += " <- " + l;
            last = l;
            ++links;

            if (l.starts_with("sub_"))
              break;
          }

          ++counts[chain];
          continue;
        }

        ++n;
        std::string where;

        if (leaf)
        {
          where = fmt::format("sub_{:08X}", leaf);
        }
        else
        {
          const uintptr_t key = s.pc[0] >> 16; // module lookup per 64 KB page
          auto it = module_cache.find(key);

          if (it == module_cache.end())
            it = module_cache.emplace(key, ModuleName(uintptr_t(s.pc[0]))).first;

          where = it->second;
        }

        // First guest function on the stack below the leaf
        uint32_t from = 0;

        for (uint32_t k = 1; k < s.count && !from; ++k)
        {
          const uint32_t g = GuestFunction(p, uintptr_t(s.pc[k]));

          if (g && g != leaf)
            from = g;
        }

        if (from)
          where += fmt::format(" <- sub_{:08X}", from);

        ++counts[where];
      }

      *total = n;
      std::vector<std::pair<std::string, uint32_t>> sorted(counts.begin(), counts.end());
      std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b)
                { return a.second > b.second; });
      std::string out;

      for (uint32_t i = 0; i < sorted.size() && i < top; ++i)
        out += fmt::format("{}{}% {}", i ? ", " : "", (sorted[i].second * 100 + n / 2) / std::max(1u, n), sorted[i].first);

      return out;
    }

    void SamplerMain(Profiler *p)
    {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
      HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);

      if (!timer)
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

      std::vector<uint8_t> stack(kStackCopy);
      LARGE_INTEGER freq;
      QueryPerformanceFrequency(&freq);
      const int64_t qpc_2s = freq.QuadPart * 2;

      while (!p->quit.load(std::memory_order_acquire))
      {
        if (timer)
        {
          LARGE_INTEGER due;
          due.QuadPart = -10000; // 1 ms
          SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
          WaitForSingleObject(timer, 50);
        }
        else
        {
          Sleep(1);
        }

        // the game thread is suspended only for this block
        if (SuspendThread(p->thread) == DWORD(-1))
          continue;

        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        const bool ok = GetThreadContext(p->thread, &ctx) != 0;
        size_t copied = 0;

        if (ok && ctx.Rsp)
        {
          // The frames above the stack pointer, up to the end of the stack's committed region
          MEMORY_BASIC_INFORMATION mbi = {};

          if (VirtualQuery(reinterpret_cast<const void *>(ctx.Rsp), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
          {
            const uintptr_t region_end = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
            copied = std::min<size_t>(kStackCopy, region_end - uintptr_t(ctx.Rsp));
            std::memcpy(stack.data(), reinterpret_cast<const void *>(ctx.Rsp), copied);
          }
        }

        ResumeThread(p->thread);

        // After the resume: this allocates and takes the loader lock, which the suspended thread may have held
        RefreshModules(*p, Qpc(), qpc_2s);

        if (!ok)
          continue;

        const uint64_t index = p->written.load(std::memory_order_relaxed);
        Sample &s = p->ring[index % kRing];
        s.qpc = Qpc();
        s.count = 1;
        s.pc[0] = ctx.Rip;

        for (size_t off = 0; off + 8 <= copied && s.count < kFrames; off += 8)
        {
          uint64_t v;
          std::memcpy(&v, stack.data() + off, 8);

          if ((v >= p->exe_begin && v < p->exe_end) || CodeModule(*p, uintptr_t(v)) >= 0)
            s.pc[s.count++] = v;
        }

        p->written.store(index + 1, std::memory_order_release);

        // Reports requested by the game thread
        std::vector<Window> slow;
        std::vector<Profiler::Period> periods;

        {
          std::lock_guard lock(p->mutex);
          slow.swap(p->slow);
          periods.swap(p->periods);
        }

        for (const Window &w : slow)
        {
          uint32_t n = 0;
          const std::string what = Describe(*p, w.begin, w.end, 6, &n);
          REXLOG_INFO("[sst-hitch] slow game frame {} ({:.1f} ms, {} samples): {}", w.frame, w.ms, n,
                      what);

          if (w.ms > 100.0)
          {
            uint32_t hn = 0;
            const std::string host = Describe(*p, w.begin, w.end, 6, &hn, true);

            if (hn)
              REXLOG_INFO("[sst-hitch]   outside guest code: {}", host);
          }
        }

        for (const Profiler::Period &period : periods)
        {
          uint32_t n = 0;
          const std::string what = Describe(*p, period.begin, period.end, 10, &n);
          REXLOG_INFO("[sst-hitch] game thread over the last {} frames ({} samples): {}",
                      period.frames, n, what);
          uint32_t hn = 0;
          const std::string host = Describe(*p, period.begin, period.end, 12, &hn, true);

          if (hn)
            REXLOG_INFO("[sst-hitch] outside guest code ({} samples, {}% of the thread): {}", hn, (hn * 100 + n / 2) / std::max(1u, n), host);
        }
      }

      if (timer)
        CloseHandle(timer);
    }

    bool Start(Profiler &p)
    {
      if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &p.thread, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return false;

      // Executable image range
      const auto *base = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
      const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
      const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
      p.exe_begin = uintptr_t(base);
      p.exe_end = p.exe_begin + nt->OptionalHeader.SizeOfImage;

      for (const PPCFuncMapping *m = PPCFuncMappings; m->host; ++m)
        p.funcs.emplace_back(reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest));

      std::sort(p.funcs.begin(), p.funcs.end());
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      p.qpc_ms = 1000.0 / double(f.QuadPart);
      p.sampler = std::thread(SamplerMain, &p);
      REXLOG_INFO("[sst-hitch] sampling the game thread (~1000/s); {} guest functions mapped; "
                  "reporting frames over {} ms",
                  p.funcs.size(), REXCVAR_GET(sst_hitch_threshold_ms));
      return true;
    }

  }

  void OnGameFrame()
  {
    if (!REXCVAR_GET(sst_hitch_profiler))
      return;

    static bool started = false;

    if (!started)
    {
      started = true;
      auto *p = new Profiler();

      if (!Start(*p))
      {
        REXLOG_WARN("[sst-hitch] could not open the game thread for sampling");
        delete p;
        return;
      }

      g_prof = p;
      p->last_swap = Qpc();
      p->period_begin = p->last_swap;
      return;
    }

    Profiler *p = g_prof;

    if (!p)
      return;

    const int64_t now = Qpc();
    const double ms = double(now - p->last_swap) * p->qpc_ms;
    ++p->frame;
    const int every = REXCVAR_GET(sst_native_log_every);
    std::lock_guard lock(p->mutex);

    // Report slow frames: all of the first 40, then one in 10
    if (ms > double(REXCVAR_GET(sst_hitch_threshold_ms)) && (p->logged_slow < 40 || p->logged_slow % 10 == 0))
    {
      p->slow.push_back({p->last_swap, now, ms, p->frame});
    }

    if (ms > double(REXCVAR_GET(sst_hitch_threshold_ms)))
      ++p->logged_slow;

    ++p->period_frames;

    if (every > 0 && p->period_frames >= uint64_t(every))
    {
      p->periods.push_back({p->period_begin, now, p->period_frames});
      p->period_begin = now;
      p->period_frames = 0;
    }

    p->last_swap = now;
  }

  void Shutdown()
  {
    Profiler *p = g_prof;

    if (!p)
      return;

    p->quit.store(true);

    if (p->sampler.joinable())
      p->sampler.join();
  }
}