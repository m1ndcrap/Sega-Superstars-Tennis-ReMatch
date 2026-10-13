// sst_rematch - restarting the game from the VIDEO page

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
  bool RestartGame()
  {
    return false;
  }

  void WaitForRestartParent() {}
  void ExitSoon(unsigned) {}
#endif
}