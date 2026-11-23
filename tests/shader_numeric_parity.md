# Existing Android-parity shader export passes

`spv_half.py`, `spv_precise.py`, and `spv_texture_offsets.py` come from nxnative's Android parity branch
`codex/cross-platform-runtime`, pinned at `1f01e28c87da5233dfa14fb31fabf1535df33013`.
Their implementations are unchanged except for local import paths. Original
commits are `74de0520b` (binary16 packing), `ae1d800e1` (numeric min/max/clamp),
and `4255c6ad3` (exact signed-four-bit texture offsets).
`spv_module.py` copies the decoder helpers; `spv_builder.py` copies the required
`Module` methods from `spv_capture.py`, excluding unrelated geometry capture.

The exporter applies the donor order, precise(half(offsets(SPIR-V))), after validating
translator products and before collecting reflection. Packaged shaders, captured
shaders, geometry variants, and vertex-input retries reach this common path.
Errors keep the existing per-shader failure/report behavior. All five helper
files participate in the porter's shader-bundle cache key.

The offset pass replaces proven bounded dynamic sample offsets with the donor's
native constant-offset selection, preserving LOD/gradient operands and filtering.
Implicit samples retain their derivative control flow. Unproven, escaped-local,
and wider ranges are rejected with the existing per-shader diagnostic. The donor
acceptance tests use equivalent SPIR-V fixtures instead of importing its larger
Maxwell instruction generator; no new lowering logic is added.

The half pass uses the donor's 32-bit integer conversion to nearest-even
binary16, including subnormals, overflow, signed zero and infinity. NaNs become
0x7e00 as in the source Apple comparison. It requires no shaderFloat16 feature.
The precise pass selects the numeric operand before floating min/max/clamp;
integer operations are unchanged. Neither pass changes no-op modules, and both
are idempotent. These fixes do not establish complete game compatibility.

## Reproduce

Put the NDK's `shader-tools/<host>` folder on PATH, then run:

```sh
python -m unittest discover -s tests -p 'test_shader_*.py'
python tests/shader_half_pack_fixture.py /tmp/numeric-fixture
python tests/shader_precise_minmax_fixture.py /tmp/numeric-fixture
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang" --target=aarch64-linux-android31 -O2 tests/shader_half_pack.c -lvulkan -o /tmp/numeric-fixture/half-pack
```

Choose the appropriate NDK host compiler directory on Linux. `shader_half_pack.c`
is the unchanged donor standalone Vulkan compute runner. Push the executable,
`lowered.spv` and `inputs.bin` to a private directory on a selected test device;
run `half-pack lowered.spv inputs.bin` using `adb -s DEVICE exec-out`, redirect its
binary stdout to a local file, and compare it with `expected.bin`. The same runner
accepts `minmax.spv` / `minmax-inputs.bin` and `saturate.spv` /
`saturate-inputs.bin`; compare their outputs with the corresponding expected
files. These are synthetic shader checks, independent of game assets.

The half fixture/oracle and host interpreter tests are copied from the source
work with import/export adaptation. The numeric fixture reuses its input pairs
and acceptance rules through native GLSL, without the source's architecture-
specific guest generator. Pairs with two NaNs or ambiguous subnormal/zero ties
retain the donor exclusions. Scalar/vector float32 and float64 modules pass
SPIR-V validation; float64 is not exercised on Android devices without support.

Device validation used Android 15 ARM64 SwiftShader: all 258,028 half components
(129,014 guarded records), 228 min/max pairs and 13 saturation inputs matched the
CPU oracle. Physical Android GPU and commercial gameplay qualification remain
outside this check.

Real original-Switch export validation used Link's Awakening's existing shader
package and the current translator built from source. Before adding the offset
pass, all 9,698 shaders exported, and 9,696 passed spirv-val; the other two failed
with dynamic sample Offset operands in both the raw translator and exported
modules. The copied offset pass makes both valid. Final full-bundle results are
recorded in the PR validation; game-derived outputs remain outside the repository.
Offset pixel/filtering results were not requalified on a physical GPU.
