# Ryujinx shader translator (vendored subset)

Three projects from Ryujinx 1.3.2: `Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator`, with these local changes:

- `Ryujinx.Graphics.Shader/CodeGen/Spirv/Instructions.cs`, `GenerateTextureSample`:
  - Texel offsets that are not constants (Maxwell AOFFI / TLD4.PTP offsets read from a register) are built with `OpCompositeConstruct` and passed as `Offset`. A four-offset gather becomes four one-offset gathers, each giving component w. Upstream wrote them as an `OpConstantComposite` with `ConstOffset(s)`, which is invalid SPIR-V; SPIRV-Cross threw `nullptr` and the shader never built (2026-10-18: 8 shaders in Wonder, Link's Awakening, Tears of the Kingdom and Age of Calamity). Marked `nxnative:` in the code.
  - Texture coordinates that are NaN are replaced by 0 (the `nfZero` select). This came with the first import.
- `Ryujinx.Graphics.Shader/Translation/Optimizations/BindlessElimination.cs`, `TryGetOperation` / `IsSameOperand`: a bindless handle that reaches the texture instruction through a phi is followed through the phis that feed it (a handle kept in a loop variable comes back through the loop's own phi), and two `constant buffer | constant` handles count as the same value. Upstream compared only the phi's direct inputs, so such handles fell to the texture-pool array of 32 (`*_t2d_cb0_40000000`), which cannot be bound; 218 pixel shaders in Monster Hunter Rise (2026-10-20). Marked `nxnative:` in the code.

- `Ryujinx.Graphics.Shader/Instructions/InstEmitTexture.cs`, `EmitTex` (TLD 1D with integer coordinates only) and `EmitTexs` (TLDS `Texture1DLodZero`), with `IGpuAccessor.RecordTexelFetch1D`: Maxwell reads a 1D texture and a texture buffer with the same instruction, and the bound descriptor decides. When the texture type query does not answer "buffer", the read is translated as 2D at `(x & 0x3FFF, x >> 14)` instead of `(x, 0)`, and the handle is reported. A 1D texture (one row at most 16384 wide on the host) reads the same; the host binds a texture buffer in such a slot as a linear 2D texture of 16384-texel rows over its memory (`nvn_mtl.m` `tbuf_as_2d`). Upstream read `(x, 0)` and the host had only a dummy texture to give: the light and decal lists of Age of Calamity (at least 15,000 shaders) and Three Houses read zeros (2026-10-21). Marked `nxnative:` in the code.

- `Ryujinx.Graphics.Shader/StructuredIr/AstOptimizer.cs`, `Optimize` / `UsedByCall`: a local that a call takes as an argument is not propagated. For SPIR-V, `StructuredProgram` copies every call argument into a local (SPIR-V passes them by pointer); propagating that local's expression into the call and removing the local left the call with a local no declaration made, and `CodeGen/Spirv` `GetLocalPointer` threw `KeyNotFoundException`: 1,894 of Viractal's 25,464 shaders, nearly all pixel shaders, did not translate (2026-10-25, the Android port). Marked `nxnative:` in the code. Not yet in `dm/gfx/tools/dmsh_xlate_bin`, which is rebuilt on a Mac with `dmsh_chain.py --update-lock`; the Android port builds the translator from this source.

The shader translation tool in `dm/gfx/tools/dmsh_xlate` references them to translate the original Switch shaders to SPIR-V.

- `Ryujinx.Graphics.Shader/Translation/Optimizations/Optimizer.cs`,
  `EliminateMultiplyByFragmentCoordW`: accept the reciprocal as either operand of
  the outer FP32 multiply, retaining the existing input-load, FragCoord.w and
  constant-one checks. An explicit Maxwell FMUL in Odyssey's alpha depth shader
  used the opposite order to its material shader, so only the latter eliminated
  the native-perspective compensation round trip. Alpha coverage disagreed at
  individual pixels; retained HDR colour then multiplied to infinity and poisoned
  bloom. A same-process original/direct-UV/original reversal and single-pixel
  draw/depth histories isolated this (2026-11-12, Android). Marked `nxnative:`.
License: MIT, see `LICENSE.txt`. Upstream: <https://github.com/Ryubing>.
