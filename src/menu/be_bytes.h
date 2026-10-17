// Big-Endian bytes in host buffers (the game's scene files and script modules, copied out of guest memory)

#pragma once

#include <bit>
#include <cstdint>
#include <vector>

namespace sst::menu
{
  using Bytes = std::vector<uint8_t>;

  inline uint16_t Rd16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }
  inline uint32_t Rd32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
  inline float RdF(const uint8_t *p) { return std::bit_cast<float>(Rd32(p)); }

  inline void Wr32(uint8_t *p, uint32_t v)
  {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
  }

  inline void WrF(uint8_t *p, float f) { Wr32(p, std::bit_cast<uint32_t>(f)); }

  // Appends
  inline void Put32(Bytes &b, uint32_t v)
  {
    const size_t at = b.size();
    b.resize(at + 4);
    Wr32(b.data() + at, v);
  }

  inline void PutF(Bytes &b, float f) { Put32(b, std::bit_cast<uint32_t>(f)); }

  // A four-character chunk / object type ("GROP") as stored
  constexpr uint32_t Tag(const char (&s)[5])
  {
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
  }
}