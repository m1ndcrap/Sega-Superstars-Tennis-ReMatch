// sst_rematch - artwork for the ultrawide side areas of loading screens

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sst::menu::art
{
  struct Image
  {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgb; // top-down rows, R G B
  };

  // A texture page: DXT3, Xenos 2D tiling, 8-in-16 byte order
  struct Page
  {
    uint32_t w = 0, h = 0; // multiples of 128
    std::vector<uint8_t> data;
  };

  struct Sides
  {
    Page left, right;
    uint32_t art_w = 0, art_h = 0; // the pictures' size (inside the pages)
  };

  // Loads and encodes the side pictures for a scene stem ("bg_ccloading"), once; nullptr if there are none (or they are unusable: the log says why)
  std::shared_ptr<const Sides> SidesFor(const std::string &stem);

  // Does a lower-case file name belong to a scene that can have side pictures?
  bool IsCourtBackground(const std::string &stem);

  // Exposed for tests
  bool LoadBmp(const std::string &path, Image &out, std::string &error);
  Page EncodeDxt3(const Image &img);
}