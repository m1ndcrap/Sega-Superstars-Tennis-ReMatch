// sst_rematch - narrow interface to the vendored XenosRecomp translator.
//
// Kept separate so the translator's global names (Shader, be<T>, byteSwap, ...)
// never meet the ReXGlue headers in one translation unit.

#pragma once

#include <cstdint>
#include <string>

namespace sst::xenos {

struct TranslateResult {
  bool ok = false;
  bool pixel_shader = false;
  uint32_t spec_constants_mask = 0;  // SPEC_CONSTANT_* bits the shader reads
  std::string hlsl;                  // complete source (prelude included)
  std::string error;
};

// `container` is a complete Xbox 360 shader container (big-endian) in host
// memory. The returned HLSL declares but does not define g_SpecConstants();
// append a definition (see SpecConstantsDefinition) before compiling.
TranslateResult Translate(const uint8_t* container);

std::string SpecConstantsDefinition(uint32_t spec);

// Spec-constant bits (same values as shader_common.h).
inline constexpr uint32_t kSpecR11G11B10Normal = 1u << 0;
inline constexpr uint32_t kSpecAlphaTest = 1u << 1;

}  // namespace sst::xenos
