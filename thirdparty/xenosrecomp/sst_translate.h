// sst_rematch - narrow interface to the vendored XenosRecomp translator

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace sst::xenos
{

  struct TranslateResult
  {
    bool ok = false;
    bool pixel_shader = false;
    uint32_t spec_constants_mask = 0; // SPEC_CONSTANT_* bits the shader reads
    std::string hlsl;                 // complete source (prelude included)
    std::string error;
  };

  // `container` is a complete Xbox 360 shader container (big-endian) in host memory. The returned HLSL declares but does not define g_SpecConstants(); append a definition (see SpecConstantsDefinition) before compiling.
  TranslateResult Translate(const uint8_t *container);

  std::string SpecConstantsDefinition(uint32_t spec);

  // Vertex inputs a vertex shader reads, as D3DDECLUSAGE / usage index pairs. The translated HLSL declares them as `i<Usage><Index> : <SEMANTIC><Index>`; BLENDINDICES, NORMAL, TANGENT and BINORMAL are uint4, the rest float4.
  struct VertexInput
  {
    uint8_t usage;
    uint8_t usage_index;
    uint16_t address; // index of the vfetch instruction that reads it
  };

  // Returns false for pixel shaders or a malformed container
  bool GetVertexInputs(const uint8_t *container, size_t size, VertexInput *out, uint32_t *count, uint32_t max_count, uint32_t *physical_offset);

  // Spec-constant bits
  inline constexpr uint32_t kSpecR11G11B10Normal = 1u << 0;
  inline constexpr uint32_t kSpecAlphaTest = 1u << 1;

}