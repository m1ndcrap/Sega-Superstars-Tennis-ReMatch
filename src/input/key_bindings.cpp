// Keyboard bindings: SST's default layout, CONTROLS > KEY BINDINGS (rebinding a key, the keyboard's player), and sst_keys.ini

#include "input/controls.h"
#include "input/cursor.h"
#include "input/input_internal.h"

#include <rex/cvar.h>
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
#else
#include <SDL3/SDL_keyboard.h>
#include <rex/ui/sdl_virtual_key.h>
#endif

#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

REXCVAR_DECLARE(std::string, sst_keyboard_player);

namespace sst::input
{
  namespace
  {
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
      {
        if (name == n)
          return v;
      }

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
      {
        if (!k.empty())
          out.push_back(k);
      }

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
      {
        if (key == n)
          return d;
      }

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

    bool AnyKeyDown()
    {
      if (!WindowFocused())
        return false;

      for (uint32_t vk = 0x08; vk <= 0xFE; ++vk)
      {
        if (KeyDown(vk))
          return true;
      }

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

  bool WindowFocused()
  {
#if defined(_WIN32)
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
#else
    // SDL's keyboard focus: one of the game's windows has it
    return SDL_GetKeyboardFocus() != nullptr;
#endif
  }

  bool KeyDown(uint32_t vk)
  {
#if defined(_WIN32)
    return (GetAsyncKeyState(int(vk)) & 0x8000) != 0;
#else
    // SDL's keyboard state (kept by the window's event loop), through the SDK's scancode -> virtual key table
    int count = 0;
    const bool *state = SDL_GetKeyboardState(&count);

    if (!state)
      return false;

    for (int sc = 0; sc < count; ++sc)
    {
      if (state[sc] && uint32_t(rex::ui::TranslateSDLScancode(SDL_Scancode(sc))) == vk)
        return true;
    }

    return false;
#endif
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

    const bool to_separate = Lower(REXCVAR_GET(sst_keyboard_player)) != "separate";
    rex::cvar::SetFlagByName("sst_keyboard_player", to_separate ? "separate" : "shared");
    REXLOG_INFO("[sst-input] keyboard player: {}", to_separate ? "separate (keyboard 1P, pads 2P-4P)" : "shared with 1P");

    ReassignPlayers();

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
    {
      return " ";
    }

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
      if (!WindowFocused())
        return 0;

      if (KeyDown(kVkEscape)) // Esc: keep the binding
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

  void ApplyDefaults()
  {
    const auto saved = LoadKeys();

    if (auto it = saved.find("keyboard_player"); it != saved.end() && !rex::cvar::HasNonDefaultValue("sst_keyboard_player"))
      rex::cvar::SetFlagByName("sst_keyboard_player", it->second);

    for (const auto &[name, value] : kDefaults)
    {
      if (!rex::cvar::HasNonDefaultValue(name))
      {
        const auto it = saved.find(name);
        rex::cvar::SetFlagByName(name, it != saved.end() ? it->second.c_str() : value);
      }
    }

    if (!saved.empty())
      REXLOG_INFO("[sst-input] key bindings from {} ({} keys)", KeysPath().filename().string(), saved.size());

    StartCursorHider();

    REXLOG_INFO("[sst-input] keyboard {}: move W A S D, shots J K L O (X A B Y), superstar I / Space, partner Q / E, back U, start Enter, Esc pause / back / quit, D-pad arrows", rex::cvar::Query<bool>("mnk_mode") ? "on" : "off (--mnk_mode=false)");
  }
}