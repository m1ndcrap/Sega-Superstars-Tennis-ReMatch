// sst_rematch - native renderer: button prompt icons for the device in use

#pragma once

#include <cstdint>

namespace sst::native::prompts
{
  // A page the prompts know: page index, or -1. width / height: the texture's; data: the guest's tiled base level (at least 4 KB)
  int MatchPage(const uint8_t *data, uint32_t width, uint32_t height);
  const char *PageName(int page);

  // Changes whenever the prompts drawn into the pages change (input device, newly captured art, key bindings)
  uint32_t Generation();

  // Upload of a known page's base level, block by block (host BC2 blocks, 16 bytes, after endian conversion). BeginPage prepares the replacement blocks for the current style; Block() captures source art and replaces target blocks in place
  void BeginPage(int page);
  void Block(int page, uint32_t bx, uint32_t by, uint8_t *block);
  void EndPage(int page);
}