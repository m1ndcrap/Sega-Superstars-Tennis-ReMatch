// sst_rematch - native renderer: Xbox 360 vertex declarations -> D3D12

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <dxgiformat.h>

namespace sst::native
{

  struct DeclElement
  {
    uint16_t stream;
    uint16_t offset;
    uint32_t type;
    uint8_t usage;
    uint8_t usage_index;
  };

  uint32_t DeclFormat(uint32_t type);       // xenos::VertexFormat value
  uint32_t DeclEndian(uint32_t type);       // 0 none, 1 8in16, 2 8in32, 3 16in32
  uint32_t DeclFormatSize(uint32_t format); // bytes, 0 if unknown

  // The translated shaders declare BLENDINDICES / NORMAL / TANGENT / BINORMAL as uint4 (raw bits) and everything else as float4
  bool UsageIsUint(uint8_t usage);

  struct ElementMapping
  {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool r11g11b10 = false;     // packed normal: the VS needs SPEC_CONSTANT_R11G11B10_NORMAL
    const char *note = nullptr; // why the mapping is approximate (logged once per type)
  };
  ElementMapping MapElement(uint32_t type, uint8_t usage);

  const char *UsageSemantic(uint8_t usage); // "POSITION", "TEXCOORD", ...
  std::string DescribeType(uint32_t type);

  void SwapVertexData(uint32_t endian, const uint8_t *src, uint8_t *dst, size_t bytes);

  // CPU vertex fetch
  struct FetchElement
  {
    uint32_t format = 0; // xenos::VertexFormat
    bool is_signed = false;
    bool normalized = true; // num_format_all == 0
    bool no_zero = false;   // signed repeating-fraction mode "no zero"
    int exp_adjust = 0;
    uint32_t offset = 0; // bytes inside the vertex

    // Source for each component of the shader input: 0-3 fetched component,
    // 4 = 0.0, 5 = 1.0.
    uint8_t sel[4] = {0, 1, 2, 3};
    enum class Out : uint8_t
    {
      kFloat,     // float4 input
      kFloatBits, // uint4 input the shader asfloat()s (NORMAL/TANGENT/BINORMAL)
      kUint,      // uint4 input used as integers (BLENDINDICES)
    } out = Out::kFloat;
  };

  // Decodes one element of one vertex. `vertex` points at the vertex in guest memory (big-endian), `endian` is the fetch constant's mode.
  void DecodeElement(const FetchElement &e, uint32_t endian, const uint8_t *vertex, uint32_t out[4]);

}