// sst_rematch - PlayStation pad lightbars in the player's colour

#include "input/lightbar.h"

#include "input/controls.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#endif

REXCVAR_DEFINE_BOOL(sst_lightbar, true, "SST", "PlayStation pads: lightbar in the player's colour (1P blue, 2P red, 3P yellow, 4P green)");

namespace sst::input
{
  namespace
  {
    struct Rgb
    {
      uint8_t r, g, b;
    };

    // The game's player colours (character select stars / cursors), at full brightness
    constexpr Rgb kPlayerColours[4] = {{8, 64, 255}, {255, 0, 32}, {255, 190, 0}, {120, 255, 0}};

    struct Pad
    {
      uint16_t product = 0;
      bool bluetooth = false;
      uint32_t slot = 0;
      uint32_t player = 0;
    };

    // The game's players in a match: a global object (0x82DB7F08) -> +0x20: four player objects -> +0x1C0: the pad (XInput user) the player uses. Native 9:193 (player -> pad) reads the same table. Array index = player number. -1: not readable (or not a pad)
    std::array<int, 4> GamePlayerPads()
    {
      std::array<int, 4> out{-1, -1, -1, -1};
      auto *memory = rex::system::kernel_memory();

      if (!memory || !InMatch())
        return out;

      auto rd = [&](uint32_t addr) -> uint32_t
      {
        const uint8_t *p = memory->TranslateVirtual<const uint8_t *>(addr);
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
      };
      auto plausible = [](uint32_t a)
      { return a >= 0x30000000u && a < 0xA0000000u && !(a & 3); };
      const uint32_t game = rd(0x82DB7F08);

      if (!plausible(game))
        return out;

      const uint32_t players = rd(game + 0x20);

      if (!plausible(players))
        return out;

      for (int i = 0; i < 4; ++i)
      {
        const uint32_t p = rd(players + 4 * uint32_t(i));

        if (!plausible(p))
          continue;

        const uint32_t pad = rd(p + 0x1C0);

        if (pad < 4)
          out[size_t(i)] = int(pad);
      }

      return out;
    }

    std::mutex g_mutex;
    std::condition_variable g_wake;
    std::vector<Pad> g_pads;
    uint32_t g_change = 0;
    bool g_thread_started = false;

    uint16_t Hex16Le(const std::string &guid, size_t at)
    {
      if (guid.size() < at + 4)
        return 0;

      const unsigned v = unsigned(std::stoul(guid.substr(at, 4), nullptr, 16));
      return uint16_t((v >> 8) | ((v & 0xFF) << 8));
    }

    bool IsDualSense(uint16_t pid) { return pid == 0x0CE6 || pid == 0x0DF2; }
    bool IsDualShock4(uint16_t pid) { return pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0; }

#if defined(_WIN32)
    uint32_t Crc32(uint32_t crc, const uint8_t *p, size_t n)
    {
      crc = ~crc;

      for (size_t i = 0; i < n; ++i)
      {
        crc ^= p[i];

        for (int k = 0; k < 8; ++k)
          crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
      }

      return ~crc;
    }

    struct HidPad
    {
      std::wstring path;
      uint16_t product = 0;
      bool bluetooth = false;
      uint64_t seen = 0; // first-seen order
    };

    std::map<std::wstring, uint64_t> g_first_seen;
    uint64_t g_seen_counter = 0;

    std::vector<HidPad> EnumerateSonyPads()
    {
      std::vector<HidPad> out;
      GUID hid_guid;
      HidD_GetHidGuid(&hid_guid);
      HDEVINFO set = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

      if (set == INVALID_HANDLE_VALUE)
        return out;

      SP_DEVICE_INTERFACE_DATA iface{};
      iface.cbSize = sizeof(iface);

      for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid_guid, i, &iface); ++i)
      {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);

        if (!need)
          continue;

        std::vector<uint8_t> buf(need);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr))
          continue;

        std::wstring path = detail->DevicePath;
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

        if (h == INVALID_HANDLE_VALUE)
          continue;

        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        const bool ok = HidD_GetAttributes(h, &attr);
        CloseHandle(h);

        if (!ok || attr.VendorID != 0x054C || !(IsDualSense(attr.ProductID) || IsDualShock4(attr.ProductID)))
          continue;

        std::wstring lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        HidPad p;
        p.path = path;
        p.product = attr.ProductID;
        p.bluetooth = lower.find(L"00001124-0000-1000-8000-00805f9b34fb") != std::wstring::npos || lower.find(L"bth") != std::wstring::npos;
        auto [it, added] = g_first_seen.emplace(path, g_seen_counter);

        if (added)
          ++g_seen_counter;

        p.seen = it->second;
        out.push_back(std::move(p));
      }

      SetupDiDestroyDeviceInfoList(set);
      std::sort(out.begin(), out.end(), [](const HidPad &a, const HidPad &b)
                { return a.seen < b.seen; });
      return out;
    }

    bool WriteLightbar(const HidPad &pad, Rgb c)
    {
      HANDLE h = CreateFileW(pad.path.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

      if (h == INVALID_HANDLE_VALUE)
        return false;

      size_t size = 0;
      PHIDP_PREPARSED_DATA pp = nullptr;
      HIDP_CAPS caps{};

      if (HidD_GetPreparsedData(h, &pp))
      {
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS)
          size = caps.OutputReportByteLength;

        HidD_FreePreparsedData(pp);
      }

      std::vector<uint8_t> buf;
      const bool ds5 = IsDualSense(pad.product);

      if (ds5 && !pad.bluetooth)
      {
        buf.assign(std::max<size_t>(48, size), 0);
        buf[0] = 0x02;     // USB effects report
        buf[1 + 1] = 0x04; // enable: lightbar only
        buf[1 + 44] = c.r, buf[1 + 45] = c.g, buf[1 + 46] = c.b;
      }
      else if (ds5)
      {
        buf.assign(std::max<size_t>(78, size), 0);
        // Bluetooth effects report. Byte 1: sequence number (high nibble, must change from report to report or the pad keeps its last state) | 2
        static uint8_t seq = 0;
        seq = uint8_t((seq + 1) & 0xF);
        buf[0] = 0x31;
        buf[1] = uint8_t(seq << 4 | 0x02);
        buf[2 + 1] = 0x04;
        buf[2 + 44] = c.r, buf[2 + 45] = c.g, buf[2 + 46] = c.b;
      }
      else if (!pad.bluetooth)
      {
        buf.assign(std::max<size_t>(32, size), 0);
        buf[0] = 0x05; // DualShock 4 USB
        buf[1] = 0x02; // lightbar only
        buf[6] = c.r, buf[7] = c.g, buf[8] = c.b;
      }
      else
      {
        buf.assign(std::max<size_t>(78, size), 0);
        buf[0] = 0x11; // DualShock 4 Bluetooth
        buf[1] = 0xC0 | 0x04;
        buf[3] = 0x02; // lightbar only
        buf[8] = c.r, buf[9] = c.g, buf[10] = c.b;
      }

      if (pad.bluetooth)
      {
        // CRC over the HID header (0xA2) and the first 74 bytes, little-endian at 74-77
        const uint8_t hdr = 0xA2;
        const uint32_t crc = Crc32(Crc32(0, &hdr, 1), buf.data(), 74);
        buf[74] = uint8_t(crc), buf[75] = uint8_t(crc >> 8), buf[76] = uint8_t(crc >> 16), buf[77] = uint8_t(crc >> 24);
      }

      DWORD written = 0;
      bool ok = WriteFile(h, buf.data(), DWORD(buf.size()), &written, nullptr) != 0;

      if (!ok)
        ok = HidD_SetOutputReport(h, buf.data(), ULONG(buf.size())) != 0;

      CloseHandle(h);
      return ok;
    }

    void Apply()
    {
      std::vector<Pad> pads;
      {
        std::lock_guard lock(g_mutex);
        pads = g_pads;
      }

      if (pads.empty() || !REXCVAR_GET(sst_lightbar))
        return;

      // In a match: the game's player numbers (whichever player reads the pad's slot)
      const std::array<int, 4> game = GamePlayerPads();
      {
        static std::array<int, 4> logged_game{-2, -2, -2, -2};

        if (game != logged_game)
        {
          logged_game = game;

          if (game != std::array<int, 4>{-1, -1, -1, -1})
            REXLOG_INFO("[sst-input] match players: pads {} {} {} {} (1P-4P, -1: none)", game[0], game[1], game[2], game[3]);
        }
      }

      for (Pad &p : pads)
        for (int i = 0; i < 4; ++i)
          if (game[size_t(i)] == int(p.slot))
          {
            p.player = uint32_t(i);
            break;
          }

      const std::vector<HidPad> hid = EnumerateSonyPads();
      // Pair each SDK pad with a HID device of the same model and bus, both in connection order
      std::vector<bool> used(hid.size(), false);
      std::sort(pads.begin(), pads.end(), [](const Pad &a, const Pad &b)
                { return a.player < b.player; });

      for (const Pad &p : pads)
        for (size_t i = 0; i < hid.size(); ++i)
          if (!used[i] && hid[i].product == p.product && hid[i].bluetooth == p.bluetooth)
          {
            used[i] = true;
            const Rgb c = kPlayerColours[p.player & 3];
            const bool ok = WriteLightbar(hid[i], c);
            static std::map<std::wstring, uint32_t> logged;

            if (auto [it, added] = logged.emplace(hid[i].path, p.player); added || it->second != p.player)
            {
              it->second = p.player;
              REXLOG_INFO("[sst-input] lightbar: {} pad ({}) is {}P{}", IsDualSense(p.product) ? "DualSense" : "DualShock 4", p.bluetooth ? "Bluetooth" : "USB", p.player + 1, ok ? "" : " - write failed");
            }

            break;
          }
    }

    void Worker()
    {
      uint32_t seen = 0;

      for (;;)
      {
        {
          std::unique_lock lock(g_mutex);
          g_wake.wait(lock, [&]
                      { return g_change != seen; });
          seen = g_change;
        }
        // SDL sets its own lightbar colour when it opens a pad and again later (Bluetooth pads switch to SDL's full reports on the first rumble, as pads come and go): apply after it, then keep re-applying every few seconds while PlayStation pads are connected
        for (int ms = 400;; ms = 2500)
        {
          std::unique_lock lock(g_mutex);

          if (g_wake.wait_for(lock, std::chrono::milliseconds(ms), [&]
                              { return g_change != seen; }))
            break;

          const bool any = !g_pads.empty();
          lock.unlock();

          if (!any)
            break;

          Apply();
        }
      }
    }
#endif
  }

  void SetLightbars(const std::vector<LightbarPad> &pads)
  {
#if defined(_WIN32)
    std::vector<Pad> list;

    for (const auto &[guid, slot, player] : pads)
    {
      // SDL GUID: bus (LE16) at hex 0, vendor at hex 8, product at hex 16
      if (guid.size() < 20 || Hex16Le(guid, 8) != 0x054C)
        continue;

      Pad p;
      p.product = Hex16Le(guid, 16);
      p.bluetooth = Hex16Le(guid, 0) == 0x0005;
      p.slot = slot;
      p.player = player;

      if (IsDualSense(p.product) || IsDualShock4(p.product))
        list.push_back(p);
    }

    std::lock_guard lock(g_mutex);

    if (list.empty() && g_pads.empty())
      return;

    g_pads = std::move(list);
    ++g_change;

    if (!g_thread_started)
    {
      g_thread_started = true;
      std::thread(Worker).detach();
    }

    g_wake.notify_all();
#else
    (void)pads;
#endif
  }
}