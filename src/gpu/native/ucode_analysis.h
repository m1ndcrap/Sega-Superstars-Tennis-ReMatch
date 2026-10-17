// Native renderer: what a Xenos shader's microcode uses, without running it; the float constants its ALU reads, the vertex / texture fetch constants it fetches from, and a short disassembly for logs

#pragma once

#include <cstdint>
#include <string>

namespace sst::native
{
  // Float constant registers the shader's ALU instructions read (bit per register in used[4]); false if it uses relative constant addressing (then any register may be read)
  bool FloatConstantsRead(const uint32_t *ucode, uint32_t dwords, uint64_t used[4]);

  // Fetch constant slots (0-95) the shader's vertex fetches read
  uint32_t VertexFetchSlots(const uint32_t *ucode, uint32_t dwords, uint32_t *slots, uint32_t max);

  // Texture fetch constants (0-31) the shader's texture fetches read, with the first fetch's dimension (xenos::FetchOpDimension + 1; 0 = unused) and filter overrides (xenos::TextureFilter / AnisoFilter values; kUseFetchConst = 3 / 7 when the fetch constant decides).
  struct TextureFetchUse
  {
    uint8_t dimension = 0;
    uint8_t mag_filter = 3, min_filter = 3, mip_filter = 3, aniso_filter = 7;
    bool unnormalized = false, mixed_dimensions = false;
    uint16_t count = 0;
    float lod_bias = 0;
  };

  uint32_t TextureFetchUsage(const uint32_t *ucode, uint32_t dwords, TextureFetchUse uses[32]);

  // Disassembly-lite of the control flow / instructions (for logs)
  std::string DescribeMicrocode(const uint32_t *ucode, uint32_t dwords);
}