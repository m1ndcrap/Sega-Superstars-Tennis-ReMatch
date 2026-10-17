// Native renderer: the button prompts' pictures (prompts.cpp): the icon kinds, RGBA images, BC2 blocks (the pages' format), and the keyboard keys drawn for the keyboard's prompts

#pragma once

#include <cstdint>
#include <vector>

namespace sst::native::prompts
{
  enum Icon : uint8_t
  {
    // Xbox icons drawn by the game (targets)
    kA,
    kB,
    kX,
    kY,
    kStart,
    kDpad,
    kLB,
    kRB,
    kLT,
    kRT,
    kPadBody,   // the controller page's Xbox 360 pad picture
    kPadStickA, // its stick overlays
    kPadStickB,
    kTargetCount,

    // PlayStation art in the same sources
    kPsCross = kTargetCount,
    kPsCircle,
    kPsSquare,
    kPsTriangle,
    kPsL1,
    kPsR1,
    kPsL2,
    kPsR2,
    kIconCount
  };

  struct Image
  {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba; // straight alpha
    bool Empty() const { return rgba.empty(); }
  };

  // prompt_image.cpp; BC2 (DXT3): 16 bytes <-> 4 x 4 RGBA pixels
  void DecodeBC2(const uint8_t *block, uint8_t out[64]);
  void EncodeBC2(const uint8_t px[64], uint8_t out[16]);

  // src fitted (aspect kept, centred) into a w x h box
  Image Fit(const Image &src, uint32_t w, uint32_t h);

  // prompt_keys.cpp; The keyboard's picture for a target icon (the key bound to that pad button); empty if it has none
  Image KeyboardIcon(Icon icon, uint32_t w, uint32_t h);

  // Changes whenever a binding the keyboard icons show changes
  uint64_t KeysHash();
}