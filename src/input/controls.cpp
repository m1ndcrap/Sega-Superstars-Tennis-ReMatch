// sst_rematch - keyboard controls and input device tracking

#include "input/controls.h"
#include "input/cursor.h"
#include "input/lightbar.h"

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/device_assignment.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/state_merge.h>
#if defined(_WIN32)
#include <rex/input/xinput/xinput_input_driver.h>
#endif
#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

REXCVAR_DEFINE_STRING(sst_prompts, "auto", "SST",
                      "Button prompts: auto (the device used last), xbox, playstation or keyboard");
REXCVAR_DEFINE_STRING(sst_keyboard_player, "shared", "SST",
                      "Keyboard's player: shared (1P, together with the first pad), separate (1P on its own, pads 2P-4P; also set in CONTROLS > KEY BINDINGS) or 1-4");

namespace sst::input
{
  namespace
  {
    using rex::X_RESULT;
    using rex::X_STATUS;
    using rex::input::DeviceId;
    using rex::input::DeviceInfo;
    using rex::input::X_INPUT_CAPABILITIES;
    using rex::input::X_INPUT_KEYSTROKE;
    using rex::input::X_INPUT_STATE;
    using rex::input::X_INPUT_VIBRATION;

    // SST's keyboard layout. Left hand: WASD moves, Q / E the bumpers (instruct partner in doubles), Space the left trigger (superstar mode). Right hand: the shot buttons on J K L O, I = right trigger (superstar mode), U = back (change view). Arrows = D-pad, Enter = start, Backspace = B as well. Esc is handled by the port (EscapeMode): pause in a match, back in menus, QUIT on the main menu
    constexpr std::pair<const char *, const char *> kDefaults[] = {
        {"mnk_mode", "true"},
        {"keybind_lstick_up", "W"},
        {"keybind_lstick_down", "S"},
        {"keybind_lstick_left", "A"},
        {"keybind_lstick_right", "D"},
        {"keybind_a", "K"},
        {"keybind_b", "L,Backspace"},
        {"keybind_x", "J"},
        {"keybind_y", "O"},
        {"keybind_right_trigger", "I"},
        {"keybind_left_trigger", "Space"},
        {"keybind_left_shoulder", "Q"},
        {"keybind_right_shoulder", "E"},
        {"keybind_back", "U"},
        {"keybind_start", "Return"},
        {"keybind_dpad_up", "Up"},
        {"keybind_dpad_down", "Down"},
        {"keybind_dpad_left", "Left"},
        {"keybind_dpad_right", "Right"},
        // Unused by SST (and the SDK's defaults would clash with the layout above)
        {"keybind_lstick_press", ""},
        {"keybind_rstick_press", ""},
        {"keybind_rstick_up", ""},
        {"keybind_rstick_down", ""},
        {"keybind_rstick_left", ""},
        {"keybind_rstick_right", ""},
        {"keybind_guide", ""},
    };

    enum class Kind : uint8_t
    {
      kNone,
      kKeyboard,
      kPlayStation,
      kXbox,
    };

    std::mutex g_mutex;
    std::unordered_map<uint64_t, Kind> g_kinds; // device -> kind (pads, while connected)
    std::vector<uint64_t> g_pad_order;          // pads in the order they were first seen
    std::atomic<uint32_t> g_last_used{uint32_t(Kind::kNone)};
    std::atomic<uint32_t> g_generation{0};
    std::atomic<uint32_t> g_confirm_presses{0};
    std::atomic<uint32_t> g_y_presses{0};
    std::atomic<uint64_t> g_last_confirm_device{0}; // the device of the last Start / A press
    std::atomic<int> g_primary_user{-1};
    std::atomic<bool> g_title{false};             // the title screen is up (the game picks its 1P from the Start press)
    std::atomic<uint64_t> g_title_device{0};      // the device that pressed Start there: it plays on slot 0 (the slot with the signed-in profile)

    int SlotOfDevice(uint64_t id);
    void PromoteDevice(uint64_t id);            // the game's 1P: the player slot that pressed Start on the title screen (-1: not known yet, slot 0)
    std::atomic<uint32_t> g_style{uint32_t(PromptStyle::kKeyboard)};

    // Esc
    std::atomic<bool> g_in_match{false};
    std::atomic<uint32_t> g_keyboard_player{0};
    std::atomic<int64_t> g_main_menu_beat{0}; // steady clock ms of the main menu's last frame on its category list (0: none)
    std::atomic<bool> g_dialog_up{false};
    std::atomic<int64_t> g_pause_beat{0}; // steady clock ms of the pause menu's last frame
    std::atomic<uint32_t> g_escape_presses{0};

    int64_t NowMs()
    {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    enum class EscapeMode : uint8_t
    {
      kBack,     // B: menus
      kPause,    // START: in a match
      kMainMenu, // no button; counted for the main menu's QUIT (native 0x53:17)
    };

    EscapeMode CurrentEscapeMode()
    {
      if (g_dialog_up.load())
        return EscapeMode::kBack; // (the YES / NO box: B = NO)

      if (const int64_t beat = g_main_menu_beat.load(); beat && NowMs() - beat < 250)
        return EscapeMode::kMainMenu;

      if (!g_in_match.load())
        return EscapeMode::kBack;

      // In a match: Start pauses; in the pause menu, Esc is B (back to the match)
      if (const int64_t beat = g_pause_beat.load(); beat && NowMs() - beat < 250)
        return EscapeMode::kBack;

      return EscapeMode::kPause;
    }

    // Esc state, shared by every keyboard poll (guarded by g_mutex)
    bool g_esc_down = false;
    EscapeMode g_esc_mode = EscapeMode::kBack; // decided when Esc goes down, kept while held

    // True while Esc is held and this window has the focus
    bool EscapeHeld(bool active)
    {
#if defined(_WIN32)
      if (!active)
        return false;

      HWND fg = GetForegroundWindow();
      DWORD pid = 0;

      if (!fg || !GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId())
        return false;

      return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
#else
      (void)active;
      return false;
#endif
    }

    std::string Lower(std::string s)
    {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                     { return char(std::tolower(c)); });
      return s;
    }

    Kind KindOf(const DeviceInfo &info)
    {
      if (info.synthetic)
        return info.name == "Keyboard and Mouse" ? Kind::kKeyboard : Kind::kNone;

      const std::string name = Lower(info.name), guid = Lower(info.guid);

      for (const char *k : {"dualsense", "dualshock", "playstation", "sony", "ps3", "ps4", "ps5"})
        if (name.find(k) != std::string::npos)
          return Kind::kPlayStation;

      // SDL GUIDs carry the USB vendor (little-endian) at hex digits 8-11: 054C = Sony
      if (guid.size() >= 12 && guid.compare(8, 4, "4c05") == 0)
        return Kind::kPlayStation;

      return Kind::kXbox;
    }

    PromptStyle StyleFor(Kind k)
    {
      return k == Kind::kPlayStation ? PromptStyle::kPlayStation : k == Kind::kXbox ? PromptStyle::kXbox
                                                                                    : PromptStyle::kKeyboard;
    }

    // Recomputes the style; called with g_mutex held
    void Update()
    {
      PromptStyle style;
      const std::string forced = Lower(REXCVAR_GET(sst_prompts));

      if (forced == "xbox")
        style = PromptStyle::kXbox;
      else if (forced == "playstation" || forced == "ps")
        style = PromptStyle::kPlayStation;
      else if (forced == "keyboard")
        style = PromptStyle::kKeyboard;
      else if (const Kind used = Kind(g_last_used.load()); used != Kind::kNone)
        style = StyleFor(used);
      else
      {
        style = PromptStyle::kKeyboard;

        for (uint64_t id : g_pad_order)
          if (auto it = g_kinds.find(id); it != g_kinds.end())
          {
            style = StyleFor(it->second);
            break;
          }
      }

      if (g_style.exchange(uint32_t(style)) != uint32_t(style))
      {
        g_generation.fetch_add(1);
        REXLOG_INFO("[sst-input] button prompts: {}", PromptStyleName(style));
      }
    }

    // Wraps one of the SDK's drivers: learns its devices' kinds and notices which one is used
    class TrackingDriver final : public rex::input::InputDriver
    {
    public:
      explicit TrackingDriver(std::unique_ptr<rex::input::InputDriver> inner) : InputDriver(nullptr, 0), inner_(std::move(inner))
      {
        // (InputSystem sets the focus / overlay callback on this wrapper; the inner driver asks through it)
        inner_->set_is_active_callback([this]
                                       { return is_active(); });
      }

      X_STATUS Setup() override { return inner_->Setup(); }

      void EnumerateDevices(std::vector<DeviceInfo> &out) override
      {
        const size_t first = out.size();
        inner_->EnumerateDevices(out);
        std::lock_guard lock(g_mutex);
        std::vector<uint64_t> now;
        bool changed = false;

        for (size_t i = first; i < out.size(); ++i)
        {
          const uint64_t id = uint64_t(out[i].id);
          const Kind kind = KindOf(out[i]);
          now.push_back(id);

          if (kind == Kind::kNone || kind == Kind::kKeyboard)
          {
            kinds_[id] = kind;
            continue;
          }

          if (g_kinds.emplace(id, kind).second)
          {
            g_pad_order.push_back(id);
            changed = true;
            REXLOG_INFO("[sst-input] controller \"{}\": {} buttons", out[i].name, kind == Kind::kPlayStation ? "PlayStation" : "Xbox");
          }

          kinds_[id] = kind;
        }
        // Pads that went away
        for (auto it = kinds_.begin(); it != kinds_.end();)
        {
          if (std::find(now.begin(), now.end(), it->first) == now.end())
          {
            if (g_kinds.erase(it->first))
            {
              std::erase(g_pad_order, it->first);
              changed = true;

              if (g_last_used.load() == uint32_t(it->second))
                g_last_used = uint32_t(Kind::kNone);
            }

            it = kinds_.erase(it);
          }
          else
          {
            ++it;
          }
        }

        if (changed)
          Update();
      }

      X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE *out_state) override
      {
        const X_RESULT r = inner_->GetDeviceState(id, out_state);

        if (r == X_ERROR_SUCCESS && out_state)
        {
          Kind own = Kind::kNone;
          bool promote = false;
          {
            std::lock_guard lock(g_mutex);

            if (auto it = kinds_.find(uint64_t(id)); it != kinds_.end())
              own = it->second;

            // Esc (keyboard only): START in a match, nothing on the main menu's category list (the script asks to QUIT), B elsewhere
            if (own == Kind::kKeyboard)
            {
              const bool down = EscapeHeld(is_active());

              if (down && !g_esc_down)
              {
                g_esc_mode = CurrentEscapeMode();

                if (g_esc_mode == EscapeMode::kMainMenu)
                  g_escape_presses.fetch_add(1);

                g_confirm_presses.fetch_add(1); // (skips the intro too)
              }

              g_esc_down = down;

              if (down && g_esc_mode != EscapeMode::kMainMenu)
                out_state->gamepad.buttons = uint16_t(uint16_t(out_state->gamepad.buttons) | (g_esc_mode == EscapeMode::kPause ? rex::input::X_INPUT_GAMEPAD_START : rex::input::X_INPUT_GAMEPAD_B));
            }
          }

          // A device takes over the prompts when it starts being used (neutral -> pressed), not while it is held: two devices held at once (or a pad that rests off-centre) would otherwise trade the prompts every poll
          const bool active = !rex::input::IsNeutral(out_state->gamepad);
          const uint16_t buttons = uint16_t(out_state->gamepad.buttons);
          {
          std::lock_guard lock(g_mutex);
          bool &was = active_[uint64_t(id)];
          // Start / A presses (new presses only), for skipping the intro
          uint16_t &held = buttons_[uint64_t(id)];

          if (buttons & ~held & (rex::input::X_INPUT_GAMEPAD_START | rex::input::X_INPUT_GAMEPAD_A))
          {
            g_confirm_presses.fetch_add(1);
            g_last_confirm_device = uint64_t(id);

            // Title screen: Start / A from a device on another slot than the first. The game would make that slot its 1P, and only slot 0 has the (local) profile - any other is a guest ("Gamer Profile Warning", no saving). The device moves to slot 0 first; this press is held back, and the game sees it on slot 0 at the next poll
            if (g_title.load() && SlotOfDevice(uint64_t(id)) > 0)
            {
              promote = true;
              out_state->gamepad.buttons = uint16_t(uint16_t(out_state->gamepad.buttons) & ~(rex::input::X_INPUT_GAMEPAD_START | rex::input::X_INPUT_GAMEPAD_A));
            }
          }

          if (buttons & ~held & rex::input::X_INPUT_GAMEPAD_Y)
            g_y_presses.fetch_add(1);

          held = buttons;

          if (active && !was)
          {
            Kind kind = Kind::kNone;

            if (auto it = kinds_.find(uint64_t(id)); it != kinds_.end())
              kind = it->second;

            if (kind != Kind::kNone && g_last_used.exchange(uint32_t(kind)) != uint32_t(kind))
              Update();
          }

          was = active;
          }

          if (promote)
            PromoteDevice(uint64_t(id));
        }

        return r;
      }

      X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES *out_caps) override { return inner_->GetDeviceCapabilities(id, flags, out_caps); }
      X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION *vibration) override { return inner_->SetDeviceVibration(id, vibration); }
      X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE *out_keystroke) override { return inner_->GetDeviceKeystroke(id, flags, out_keystroke); }
      void OnWindowAvailable(rex::ui::Window *window) override { inner_->OnWindowAvailable(window); }

    private:
      std::unique_ptr<rex::input::InputDriver> inner_;
      std::unordered_map<uint64_t, Kind> kinds_;       // this driver's devices (guarded by g_mutex)
      std::unordered_map<uint64_t, bool> active_;      // last state per device: pressed (guarded by g_mutex)
      std::unordered_map<uint64_t, uint16_t> buttons_; // last buttons per device (guarded by g_mutex)
    };

    // Players. SHARED (default): the keyboard plays as 1P together with the first pad (one player, either device), pad N is player N. SEPARATE (CONTROLS > KEY BINDINGS, for keyboard + pad multiplayer): the keyboard is 1P on its own, the pads are 2P-4P. --sst_keyboard_player=1-4 puts the keyboard on a fixed player (pads keep theirs)
    class PlayerAssignment final : public rex::input::DeviceAssignment
    {
    public:
      void OnDevicesChanged(const std::vector<DeviceInfo> &devices) override
      {
        std::lock_guard lock(mutex_);
        devices_ = devices;
        Assign();
      }

      void DevicesForUser(uint32_t user_index, std::vector<DeviceId> &out) const override
      {
        std::lock_guard lock(mutex_);
        out.clear();

        if (user_index < rex::input::kMaxGuestUsers)
          out = users_[user_index];
      }

      // The keyboard setting changed
      void Reassign()
      {
        std::lock_guard lock(mutex_);
        Assign();
      }

    private:
      void Assign()
      {
        for (auto &u : users_)
          u.clear();

        const std::string mode = Lower(REXCVAR_GET(sst_keyboard_player));
        const bool separate = mode == "separate";
        const int fixed = mode.size() == 1 && mode[0] >= '1' && mode[0] <= '4' ? mode[0] - '1' : -1;
        std::vector<LightbarPad> pads;

        for (const DeviceInfo &d : devices_)
        {
          if (!d.synthetic)
          {
            const uint32_t user = std::min<uint32_t>(d.ordinal + (separate ? 1 : 0), rex::input::kMaxGuestUsers - 1);
            users_[user].push_back(d.id);
            pads.push_back({d.guid, user, user});
            continue;
          }

          if (d.name != "Keyboard and Mouse")
          {
            users_[0].push_back(d.id); // the NOP stand-in
            continue;
          }

          const uint32_t user = fixed >= 0 ? uint32_t(fixed) : 0;
          users_[user].push_back(d.id);
          g_keyboard_player = user;
        }

        // The device that started the game from the title screen plays on slot 0 (the slot with the profile); whatever was there takes its slot
        if (const uint64_t title = g_title_device.load())
          for (uint32_t u = 1; u < rex::input::kMaxGuestUsers; ++u)
            if (std::find(users_[u].begin(), users_[u].end(), DeviceId(title)) != users_[u].end())
            {
              std::swap(users_[0], users_[u]);

              for (LightbarPad &p : pads)
                p.slot = p.slot == u ? 0 : p.slot == 0 ? u : p.slot;

              if (g_keyboard_player == u)
                g_keyboard_player = 0;
              else if (g_keyboard_player == 0)
                g_keyboard_player = u;

              break;
            }

        std::string desc;

        for (uint32_t u = 0; u < rex::input::kMaxGuestUsers; ++u)
          for (const DeviceInfo &d : devices_)
            if (std::find(users_[u].begin(), users_[u].end(), d.id) != users_[u].end() && d.name != "None")
              desc += fmt::format("{}{}P {}", desc.empty() ? "" : ", ", u + 1, d.name);

        if (desc != last_)
        {
          last_ = desc;
          REXLOG_INFO("[sst-input] players: {}", desc.empty() ? "none" : desc);
        }

        // Lightbars show the game's player numbers: the slot that started the game (title screen) is 1P, the other slots with a device follow in order
        std::vector<uint32_t> order;
        const int primary = g_primary_user.load();

        // (Before the title screen is left nobody has started the game: pads count from 1P among themselves)
        if (primary >= 0 && primary < int(rex::input::kMaxGuestUsers) && UsedLocked(uint32_t(primary)))
          order.push_back(uint32_t(primary));

        for (uint32_t u = 0; u < rex::input::kMaxGuestUsers; ++u)
          if (UsedLocked(u, primary < 0) && std::find(order.begin(), order.end(), u) == order.end())
            order.push_back(u);

        for (LightbarPad &p : pads)
          p.number = uint32_t(std::find(order.begin(), order.end(), p.slot) - order.begin());

        SetLightbars(pads);
      }

      bool UsedLocked(uint32_t u, bool pads_only = false) const
      {
        for (DeviceId id : users_[u])
          for (const DeviceInfo &d : devices_)
            if (d.id == id && d.name != "None" && !(pads_only && d.synthetic))
              return true;

        return false;
      }

    public:
      // The player slot a device plays on (-1: none)
      int UserOf(uint64_t id) const
      {
        std::lock_guard lock(mutex_);

        for (uint32_t u = 0; u < rex::input::kMaxGuestUsers; ++u)
          for (DeviceId d : users_[u])
            if (uint64_t(d) == id)
              return int(u);

        return -1;
      }

    private:
      mutable std::mutex mutex_;
      std::vector<DeviceInfo> devices_;
      std::vector<DeviceId> users_[rex::input::kMaxGuestUsers];
      std::string last_;
    };

    PlayerAssignment *g_assignment = nullptr;

    int SlotOfDevice(uint64_t id)
    {
      return g_assignment ? g_assignment->UserOf(id) : -1;
    }

    void PromoteDevice(uint64_t id)
    {
      if (g_title_device.exchange(id) == id || !g_assignment)
        return;

      REXLOG_INFO("[sst-input] title screen: the device that pressed Start moves to player slot 1 (the profile's)");
      g_assignment->Reassign();
    }

    void Add(rex::input::InputSystem &system, std::unique_ptr<rex::input::InputDriver> driver)
    {
      if (driver->Setup() == X_STATUS_SUCCESS)
        system.AddDriver(std::make_unique<TrackingDriver>(std::move(driver)));
    }
  }

  // ---- key bindings (CONTROLS > KEY BINDINGS) ----
  namespace
  {
    struct KeyAction
    {
      const char *cvar;
      const char *label; // the page's label font: A-Z, 0-4, '-', ':', '/'
    };

    constexpr KeyAction kKeyActions[] = {
        {"keybind_lstick_up", "MOVE UP"},
        {"keybind_lstick_down", "MOVE DOWN"},
        {"keybind_lstick_left", "MOVE LEFT"},
        {"keybind_lstick_right", "MOVE RIGHT"},
        {"keybind_a", "A BUTTON"},
        {"keybind_b", "B BUTTON"},
        {"keybind_x", "X BUTTON"},
        {"keybind_y", "Y BUTTON"},
        {"keybind_left_shoulder", "LEFT BUMPER"},
        {"keybind_right_shoulder", "RIGHT BUMPER"},
        {"keybind_left_trigger", "LEFT TRIGGER"},
        {"keybind_right_trigger", "RIGHT TRIGGER"},
        {"keybind_back", "BACK BUTTON"},
        {"keybind_start", "START BUTTON"},
        {"keybind_dpad_up", "D-PAD UP"},
        {"keybind_dpad_down", "D-PAD DOWN"},
        {"keybind_dpad_left", "D-PAD LEFT"},
        {"keybind_dpad_right", "D-PAD RIGHT"},
    };
    constexpr int kKeyActionCount = int(std::size(kKeyActions));

    // Keys that can be bound (names as the SDK's key parser knows them; Esc cancels, bare modifiers never fire in the SDK's keyboard driver)
    constexpr const char *kBindableKeys[] = {
        "A",
        "B",
        "C",
        "D",
        "E",
        "F",
        "G",
        "H",
        "I",
        "J",
        "K",
        "L",
        "M",
        "N",
        "O",
        "P",
        "Q",
        "R",
        "S",
        "T",
        "U",
        "V",
        "W",
        "X",
        "Y",
        "Z",
        "0",
        "1",
        "2",
        "3",
        "4",
        "5",
        "6",
        "7",
        "8",
        "9",
        "Return",
        "Space",
        "Tab",
        "Backspace",
        "Delete",
        "Insert",
        "Home",
        "End",
        "PageUp",
        "PageDown",
        "Left",
        "Right",
        "Up",
        "Down",
        "Backtick",
        "Minus",
        "Plus",
        "Comma",
        "Period",
        "Semicolon",
        "Slash",
        "Backslash",
        "LBracket",
        "RBracket",
        "Quote",
        "Numpad0",
        "Numpad1",
        "Numpad2",
        "Numpad3",
        "Numpad4",
        "Numpad5",
        "Numpad6",
        "Numpad7",
        "Numpad8",
        "Numpad9",
        "NumpadPlus",
        "NumpadMinus",
        "NumpadStar",
        "NumpadSlash",
        "CapsLock",
        "F1",
        "F2",
        "F3",
        "F4",
        "F5",
        "F6",
        "F7",
        "F8",
        "F9",
        "F10",
        "F11",
        "F12",
    };

    std::filesystem::path KeysPath()
    {
      return rex::filesystem::GetExecutableFolder() / "sst_keys.ini";
    }

    const char *DefaultFor(const std::string &name)
    {
      for (const auto &[n, v] : kDefaults)
        if (name == n)
          return v;

      return "";
    }

    std::map<std::string, std::string> LoadKeys()
    {
      std::map<std::string, std::string> out;
      std::ifstream f(KeysPath());
      std::string line;

      while (std::getline(f, line))
      {
        const size_t eq = line.find('=');

        if (eq == std::string::npos || (line.compare(0, 8, "keybind_") != 0 && line.compare(0, 15, "keyboard_player") != 0))
          continue;

        auto trim = [](std::string v)
        {
          while (!v.empty() && std::isspace((unsigned char)v.back()))
            v.pop_back();

          size_t i = 0;

          while (i < v.size() && std::isspace((unsigned char)v[i]))
            ++i;

          return v.substr(i);
        };
        out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
      }

      return out;
    }

    void SaveKeys()
    {
      std::ofstream f(KeysPath(), std::ios::trunc);
      f << "# SST ReMatch keyboard bindings (CONTROLS > KEY BINDINGS). Delete this file to go back to the defaults.\n";

      f << "keyboard_player=" << REXCVAR_GET(sst_keyboard_player) << "\n";

      for (const KeyAction &a : kKeyActions)
        f << a.cvar << "=" << rex::cvar::Query<std::string>(a.cvar) << "\n";
    }

    std::vector<std::string> SplitKeys(const std::string &v)
    {
      std::vector<std::string> out;
      std::stringstream ss(v);
      std::string k;

      while (std::getline(ss, k, ','))
        if (!k.empty())
          out.push_back(k);

      return out;
    }

    std::string JoinKeys(const std::vector<std::string> &keys)
    {
      std::string out;

      for (const std::string &k : keys)
        out += (out.empty() ? "" : ",") + k;

      return out;
    }

    // "PageUp" -> "PAGE UP", "Numpad0" -> "NUMPAD 0", "Return" -> "ENTER"
    std::string KeyDisplay(const std::string &key)
    {
      static const std::pair<const char *, const char *> kNames[] = {{"Return", "ENTER"}, {"LBracket", "LEFT BRACKET"}, {"RBracket", "RIGHT BRACKET"}, {"NumpadStar", "NUMPAD STAR"}, {"Backspace", "BACKSPACE"}, {"CapsLock", "CAPS LOCK"}};

      for (auto [n, d] : kNames)
        if (key == n)
          return d;

      std::string out;

      for (size_t i = 0; i < key.size(); ++i)
      {
        const char c = key[i];

        if (i && ((std::isupper((unsigned char)c) && std::islower((unsigned char)key[i - 1])) || (std::isdigit((unsigned char)c) && std::isalpha((unsigned char)key[i - 1]))) && key != "+")
          out += ' ';

        out += char(std::toupper((unsigned char)c));
      }

      return out;
    }

    enum class Capture : uint8_t
    {
      kIdle,
      kWaitRelease, // the key that chose the row is still down
      kWaitPress,
      kHoldRelease, // the new key is still down (the menu ignores it)
    };

    std::mutex g_keys_mutex;
    Capture g_capture = Capture::kIdle;
    int g_capture_action = -1;

    bool Focused()
    {
#if defined(_WIN32)
      HWND fg = GetForegroundWindow();
      DWORD pid = 0;
      return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
#else
      return false;
#endif
    }

    bool KeyDown(uint32_t vk)
    {
#if defined(_WIN32)
      return (GetAsyncKeyState(int(vk)) & 0x8000) != 0;
#else
      (void)vk;
      return false;
#endif
    }

    bool AnyKeyDown()
    {
      if (!Focused())
        return false;

      for (uint32_t vk = 0x08; vk <= 0xFE; ++vk)
        if (KeyDown(vk))
          return true;

      return false;
    }

    void Bind(int action, const std::string &key)
    {
      const KeyAction &a = kKeyActions[action];
      const std::vector<std::string> old = SplitKeys(rex::cvar::Query<std::string>(a.cvar));
      // The key leaves any other action; one left without keys takes this action's old key
      for (int j = 0; j < kKeyActionCount; ++j)
      {
        if (j == action)
          continue;

        std::vector<std::string> keys = SplitKeys(rex::cvar::Query<std::string>(kKeyActions[j].cvar));
        const size_t before = keys.size();
        std::erase(keys, key);

        if (keys.size() == before)
          continue;

        if (keys.empty() && !old.empty() && old[0] != key)
          keys.push_back(old[0]);

        rex::cvar::SetFlagByName(kKeyActions[j].cvar, JoinKeys(keys));
        REXLOG_INFO("[sst-input] {} -> {}", kKeyActions[j].cvar, JoinKeys(keys));
      }

      rex::cvar::SetFlagByName(a.cvar, key);
      REXLOG_INFO("[sst-input] {} -> {}", a.cvar, key);
      SaveKeys();
    }
  }

  // Rows: KEYBOARD PLAYER, the actions (Y resets them)
  int KeyBindingCount()
  {
    return kKeyActionCount + 1;
  }

  const char *KeyBindingLabel(int row)
  {
    const int i = row - 1;
    return row == 0 ? "KEYBOARD PLAYER" : i >= 0 && i < kKeyActionCount ? kKeyActions[i].label
                                                                        : " ";
  }

  bool KeyBindingToggle(int row)
  {
    if (row != 0)
      return false;

    const bool separate = Lower(REXCVAR_GET(sst_keyboard_player)) != "separate";
    rex::cvar::SetFlagByName("sst_keyboard_player", separate ? "separate" : "shared");
    REXLOG_INFO("[sst-input] keyboard player: {}", separate ? "separate (keyboard 1P, pads 2P-4P)" : "shared with 1P");

    if (g_assignment)
      g_assignment->Reassign();

    SaveKeys();
    return true;
  }

  std::string KeyBindingValue(int row)
  {
    if (row == 0)
    {
      const std::string mode = Lower(REXCVAR_GET(sst_keyboard_player));
      return mode == "separate" ? "SEPARATE" : mode.size() == 1 && mode[0] >= '1' && mode[0] <= '4' ? mode + "P"
                                                                                                    : "SHARED WITH 1P";
    }

    const int i = row - 1;

    if (i < 0 || i >= kKeyActionCount)
      return " ";

    {
      std::lock_guard lock(g_keys_mutex);

      if (g_capture_action == i && (g_capture == Capture::kWaitRelease || g_capture == Capture::kWaitPress))
        return "PRESS A KEY";
    }

    const std::vector<std::string> keys = SplitKeys(rex::cvar::Query<std::string>(kKeyActions[i].cvar));

    if (keys.empty())
      return "-";

    std::string out;

    for (size_t k = 0; k < keys.size() && k < 2; ++k)
      out += (k ? " / " : "") + KeyDisplay(keys[k]);

    return out;
  }

  void BeginKeyCapture(int row)
  {
    std::lock_guard lock(g_keys_mutex);
    const int i = row - 1;

    if (i < 0 || i >= kKeyActionCount)
      return;

    g_capture = Capture::kWaitRelease;
    g_capture_action = i;
  }

  bool KeyCaptureActive()
  {
    std::lock_guard lock(g_keys_mutex);
    return g_capture != Capture::kIdle;
  }

  int PollKeyCapture()
  {
    std::lock_guard lock(g_keys_mutex);

    switch (g_capture)
    {
    case Capture::kIdle:
      return 0;

    case Capture::kWaitRelease:
      if (!AnyKeyDown())
        g_capture = Capture::kWaitPress;

      return 0;

    case Capture::kWaitPress:
    {
      if (!Focused())
        return 0;

      if (KeyDown(0x1B)) // Esc: keep the binding
      {
        g_capture = Capture::kHoldRelease;
        REXLOG_INFO("[sst-input] key binding cancelled");
        return 2;
      }

      for (const char *name : kBindableKeys)
      {
        const uint32_t vk = uint32_t(rex::ui::ParseVirtualKey(name));

        if (vk && KeyDown(vk))
        {
          Bind(g_capture_action, name);
          g_capture = Capture::kHoldRelease;
          return 1;
        }
      }

      return 0;
    }

    case Capture::kHoldRelease:
      if (!AnyKeyDown())
        g_capture = Capture::kIdle, g_capture_action = -1;

      return 0;
    }

    return 0;
  }

  void ResetKeyBindings()
  {
    for (const KeyAction &a : kKeyActions)
      rex::cvar::SetFlagByName(a.cvar, DefaultFor(a.cvar));

    SaveKeys();
    REXLOG_INFO("[sst-input] key bindings reset to the defaults");
  }

  bool InMatch()
  {
    return g_in_match.load();
  }

  void MarkGameStarted()
  {
    const int user = g_assignment ? g_assignment->UserOf(g_last_confirm_device.load()) : -1;

    if (user >= 0 && g_primary_user.exchange(user) != user)
    {
      REXLOG_INFO("[sst-input] the game was started from player slot {}: it is 1P", user + 1);

      if (g_assignment)
        g_assignment->Reassign();
    }
  }

  uint32_t YPresses()
  {
    return g_y_presses.load(std::memory_order_relaxed);
  }

  void ApplyDefaults()
  {
    const auto saved = LoadKeys();

    if (auto it = saved.find("keyboard_player"); it != saved.end() && !rex::cvar::HasNonDefaultValue("sst_keyboard_player"))
      rex::cvar::SetFlagByName("sst_keyboard_player", it->second);

    for (const auto &[name, value] : kDefaults)
      if (!rex::cvar::HasNonDefaultValue(name))
      {
        const auto it = saved.find(name);
        rex::cvar::SetFlagByName(name, it != saved.end() ? it->second.c_str() : value);
      }

    if (!saved.empty())
      REXLOG_INFO("[sst-input] key bindings from {} ({} keys)", KeysPath().filename().string(), saved.size());

    StartCursorHider();

    REXLOG_INFO("[sst-input] keyboard {}: move W A S D, shots J K L O (X A B Y), superstar I / Space, "
                "partner Q / E, back U, start Enter, Esc pause / back / quit, D-pad arrows",
                rex::cvar::Query<bool>("mnk_mode") ? "on" : "off (--mnk_mode=false)");
  }

  // Same drivers and order as rex::input::CreateDefaultInputSystem
  std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode)
  {
    auto system = std::make_unique<rex::input::InputSystem>(nullptr);

    if (!tool_mode)
    {
#if defined(_WIN32)
      const std::string backend = rex::cvar::Query<std::string>("input_backend");

      if (backend == "xinput")
        Add(*system, std::make_unique<rex::input::xinput::XinputInputDriver>(nullptr, 0));
#endif
      if (backend == "sdl")
        Add(*system, std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 0));

      Add(*system, std::make_unique<rex::input::mnk::MnkInputDriver>(nullptr, 0));
    }

    system->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, tool_mode ? 0 : 1));
    auto assignment = std::make_unique<PlayerAssignment>();
    g_assignment = assignment.get();
    system->SetDeviceAssignment(std::move(assignment));
    {
      std::lock_guard lock(g_mutex);
      Update();
    }
    return system;
  }

  PromptStyle CurrentPromptStyle()
  {
    return PromptStyle(g_style.load(std::memory_order_relaxed));
  }

  uint32_t ConfirmPresses()
  {
    return g_confirm_presses.load(std::memory_order_relaxed);
  }

  void SetInMatch(bool in_match)
  {
    if (g_in_match.exchange(in_match) != in_match)
      REXLOG_INFO("[sst-input] Esc: {}", in_match ? "pause (match)" : "back (menus)");
  }

  void MainMenuFrame(bool category_list)
  {
    g_main_menu_beat = category_list ? NowMs() : 0;
  }

  void SetTitleScreen(bool up)
  {
    g_title = up;
  }

  void PauseMenuFrame()
  {
    g_pause_beat = NowMs();
  }

  void SetDialogUp(bool up)
  {
    g_dialog_up = up;
  }

  uint32_t EscapePresses()
  {
    return g_escape_presses.load(std::memory_order_relaxed);
  }

  uint32_t PromptStyleGeneration()
  {
    return g_generation.load(std::memory_order_relaxed);
  }

  const char *PromptStyleName(PromptStyle style)
  {
    switch (style)
    {
    case PromptStyle::kPlayStation:
      return "PlayStation";
    case PromptStyle::kKeyboard:
      return "keyboard";
    default:
      return "Xbox";
    }
  }
}