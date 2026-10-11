# XenosRecomp translator

Source: https://github.com/hedge-dev/XenosRecomp at 990d03b28a27b50277ee5d8d942e1c5f873869d1 (MIT, see LICENSE.md).

Only the Xenos microcode -> HLSL translator is used (`shader_recompiler.*`, `shader*.h`, `constant_table.h`, `shader_common.h`). The tool's `main.cpp`, DXC wrapper and cache writer are not used: sst_rematch translates and compiles at runtime (src/gpu/native/shader_cache.cpp), with DXC loaded from `dxcompiler.dll`.

Local changes (built with `SST_RECOMP`, same as `xenosrecomp-sst.patch` in the repo root):

- `shader_recompiler.cpp`: extra interpolators NORMAL0, TANGENT0, BINORMAL0, POSITION1 that Sega Superstars Tennis passes between stages.
- `shader_recompiler.cpp`: relative constant addressing (`c[a0+n]`, `c[aL+n]`, e.g. skinning palettes) reads a raw view of the constant buffer (`SST_CONST_REL`, bound at b3 / b4 space4); the const slot of each operand follows Xenia's `src_const_is_addressed`. Upstream asserted on 83 of the game's shaders and mis-indexed in release builds.
- `shader_recompiler.cpp`: vertex-shader exports with no matching interpolator are written to a scratch variable instead of asserting.
- `shader_common.h`: `getTexture3DDimensions` / `getWeights3D` for 3D-texture fetches.
- `shader_recompiler.cpp` / `shader_common.h`: 3D fetches keep their texel offsets (`tfetch3D(..., float3 offset)`, `getWeights3D(..., float3 offset)`). Upstream only passed offsets for 2D fetches; the game's shadow lookups take four taps at ±0.5 texel on a stacked 3D shadow map (soft edges), which all landed on the same texel without them.
- `sst_prefix.h`: replaces upstream `pch.h` (standard library + fmt + `byteSwap` / `be<T>` only).

`shader_common.h` is also the HLSL prelude for every translated shader; CMake embeds it into the exe.
