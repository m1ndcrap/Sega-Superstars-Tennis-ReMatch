// sst_rematch - keyboard controls and input device tracking

#pragma once

#include <cstdint>
#include <memory>

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
  void MainMenuFrame(bool category_list); // every main menu frame: on its category list or not
  void SetDialogUp(bool up);            // the port's YES / NO box (Esc = B = NO)
  uint32_t EscapePresses();              // Esc presses counted on the main menu's category list

  // Bumped whenever CurrentPromptStyle() changes
  uint32_t PromptStyleGeneration();

  const char *PromptStyleName(PromptStyle style);
}