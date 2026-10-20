// Restarting the game from the VIDEO page

#include "menu/restart.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwchar>
#include <string>
#else
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;
#endif

namespace sst::menu
{
#ifdef _WIN32
  namespace
  {
    constexpr const wchar_t *kParentVar = L"SST_RESTART_PARENT";
  }

  bool RestartGame()
  {
    wchar_t exe[MAX_PATH] = {};

    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
      return false;

    std::wstring command_line = GetCommandLineW(); // same arguments

    // Inherited by the new copy: it waits for this process to exit
    SetEnvironmentVariableW(kParentVar, std::to_wstring(GetCurrentProcessId()).c_str());
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessW(exe, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi); // same working folder
    SetEnvironmentVariableW(kParentVar, nullptr);

    if (!ok)
      return false;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
  }

  void WaitForRestartParent()
  {
    wchar_t value[32] = {};

    if (!GetEnvironmentVariableW(kParentVar, value, 32))
      return;

    SetEnvironmentVariableW(kParentVar, nullptr);
    const DWORD pid = DWORD(std::wcstoul(value, nullptr, 10));

    if (!pid)
      return;

    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid))
    {
      WaitForSingleObject(h, 15000);
      CloseHandle(h);
    }
  }

  void ExitSoon(unsigned ms)
  {
    CreateThread(
        nullptr, 0,
        [](LPVOID param) -> DWORD
        {
          Sleep(DWORD(reinterpret_cast<uintptr_t>(param)));
          TerminateProcess(GetCurrentProcess(), 0);
          return 0;
        },
        reinterpret_cast<LPVOID>(uintptr_t(ms)), 0, nullptr);
  }
#else
  namespace
  {
    constexpr const char *kParentVar = "SST_RESTART_PARENT";

    // This process's arguments (/proc/self/cmdline: NUL-separated)
    std::vector<std::string> Arguments()
    {
      std::vector<std::string> args;
      std::ifstream file("/proc/self/cmdline", std::ios::binary);
      std::string arg;

      while (std::getline(file, arg, '\0'))
        args.push_back(arg);

      return args;
    }
  }

  bool RestartGame()
  {
    std::vector<std::string> args = Arguments();

    if (args.empty())
      return false;

    std::vector<char *> argv;

    for (std::string &a : args)
      argv.push_back(a.data());

    argv.push_back(nullptr);

    // Inherited by the new copy: it waits for this process to exit. Same arguments and working folder
    setenv(kParentVar, std::to_string(getpid()).c_str(), 1);
    pid_t child = 0;
    const int rc = posix_spawn(&child, "/proc/self/exe", nullptr, nullptr, argv.data(), environ);
    unsetenv(kParentVar);
    return rc == 0;
  }

  void WaitForRestartParent()
  {
    const char *value = std::getenv(kParentVar);

    if (!value)
      return;

    const pid_t pid = pid_t(std::strtol(value, nullptr, 10));
    unsetenv(kParentVar);

    if (pid <= 0)
      return;

    for (int i = 0; i < 300 && (kill(pid, 0) == 0 || errno == EPERM); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  void ExitSoon(unsigned ms)
  {
    std::thread([ms]
                {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      _exit(0); })
        .detach();
  }
#endif
}