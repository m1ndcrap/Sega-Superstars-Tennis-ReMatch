// sst_rematch - keyboard controls and input device tracking

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace rex::system
{
  class IInputSystem;
}

namespace sst::input
{
  enum class PromptStyle : uint32_t
  {
    kXbox = 0,
    kPlayStation = 1,
    kKeyboard = 2,
  };

  // Keyboard on, SST's key layout: only for settings not given on the command line / in the config
  void ApplyDefaults();

  // The SDK's default input system (SDL or XInput pads, keyboard, NOP fallback) with device tracking
  std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);

  // Kind of device last used (any button, trigger or stick past the dead zone); before any input, the first pad connected (keyboard when there is none). --sst_prompts forces a style
  PromptStyle CurrentPromptStyle();

  // Count of Start / A presses on any device (keyboard: Enter, K, Esc), for skipping the intro
  uint32_t ConfirmPresses();

  // Esc on the keyboard: pause (START) in a match, back (B) in menus, QUIT on the main menu's category list
  void SetInMatch(bool in_match);       // from the front-end's script starts (HUD scripts: match; menu scripts: not)
  bool InMatch();
  void MainMenuFrame(bool category_list); // every main menu frame: on its category list or not
  void PauseMenuFrame();                 // every frame the in-match pause menu reads input (Esc: back instead of pause)
  void SetDialogUp(bool up);            // the port's YES / NO box (Esc = B = NO)
  uint32_t EscapePresses();              // Esc presses counted on the main menu's category list

  // The title screen was left: the device that pressed Start there is the game's 1P (lightbar colours follow the game's player numbers)
  void MarkGameStarted();

  // Y presses on any device (the CONTROLS page opens KEY BINDINGS with Y)
  uint32_t YPresses();

  // CONTROLS > KEY BINDINGS: rows 0..KeyBindingCount()-1: KEYBOARD PLAYER (shared with 1P / separate), then the bindings (Y on the page: ResetKeyBindings). Saved to sst_keys.ini next to the exe
  int KeyBindingCount();
  const char *KeyBindingLabel(int row);
  std::string KeyBindingValue(int row); // "PRESS A KEY" while the row waits for a key
  bool KeyBindingToggle(int row);       // KEYBOARD PLAYER: switches it (false: not that row)
  void BeginKeyCapture(int row);        // the next key pressed (Esc: cancel) becomes the row's binding
  int PollKeyCapture();                 // every frame: 1 bound, 2 cancelled (redraw), 0 nothing new
  bool KeyCaptureActive();              // waiting for a key or for its release: menu input is ignored
  void ResetKeyBindings();

  // Bumped whenever CurrentPromptStyle() changes
  uint32_t PromptStyleGeneration();

  const char *PromptStyleName(PromptStyle style);
}