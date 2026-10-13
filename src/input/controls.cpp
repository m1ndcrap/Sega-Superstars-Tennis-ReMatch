// sst_rematch - keyboard controls and input device tracking

#include "input/controls.h"

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
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

REXCVAR_DEFINE_STRING(sst_prompts, "auto", "SST",
                      "Button prompts: auto (the device used last), xbox, playstation or keyboard");

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

    // SST's keyboard layout. Left hand: WASD moves, Q / E the bumpers (instruct partner in doubles), Space the left trigger (superstar mode). Right hand: the shot buttons on J K L O, I = right trigger (superstar mode), U = back (change view). Arrows = D-pad, Enter / Esc = start (pause), Backspace = B as well
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
        {"keybind_start", "Return,Escape"},
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
    std::atomic<uint32_t> g_style{uint32_t(PromptStyle::kKeyboard)};

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
          // A device takes over the prompts when it starts being used (neutral -> pressed), not while it is held: two devices held at once (or a pad that rests off-centre) would otherwise trade the prompts every poll
          const bool active = !rex::input::IsNeutral(out_state->gamepad);
          std::lock_guard lock(g_mutex);
          bool &was = active_[uint64_t(id)];

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

        return r;
      }

      X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES *out_caps) override { return inner_->GetDeviceCapabilities(id, flags, out_caps); }
      X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION *vibration) override { return inner_->SetDeviceVibration(id, vibration); }
      X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE *out_keystroke) override { return inner_->GetDeviceKeystroke(id, flags, out_keystroke); }
      void OnWindowAvailable(rex::ui::Window *window) override { inner_->OnWindowAvailable(window); }

    private:
      std::unique_ptr<rex::input::InputDriver> inner_;
      std::unordered_map<uint64_t, Kind> kinds_;  // this driver's devices (guarded by g_mutex)
      std::unordered_map<uint64_t, bool> active_; // last state per device: pressed (guarded by g_mutex)
    };

    void Add(rex::input::InputSystem &system, std::unique_ptr<rex::input::InputDriver> driver)
    {
      if (driver->Setup() == X_STATUS_SUCCESS)
        system.AddDriver(std::make_unique<TrackingDriver>(std::move(driver)));
    }
  }

  void ApplyDefaults()
  {
    for (const auto &[name, value] : kDefaults)
      if (!rex::cvar::HasNonDefaultValue(name))
        rex::cvar::SetFlagByName(name, value);

    REXLOG_INFO("[sst-input] keyboard {}: move W A S D, shots J K L O (X A B Y), superstar I / Space, "
                "partner Q / E, back U, pause Enter / Esc, D-pad arrows",
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
    system->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());

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