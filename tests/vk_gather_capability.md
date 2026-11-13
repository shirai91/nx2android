# Existing extended-gather capability cleanup

Depends on numeric parity PR #355. Production code copies only the
`ImageGatherExtended` requirement check from nxnative `b9e3c2b66`, pinned at
`1f01e28c87da5233dfa14fb31fabf1535df33013:dm/android/tools/spv_android.py`.
The opcode/operand map and `Offset | ConstOffsets` check are unchanged.
Other capability handling remains in the existing native loader. No shader
operation, descriptor ABI, driver feature enablement, or new translation is added.

The Python regression is copied from `dm/tools/tests/test_android_texture_capability.py`.
It checks ordinary and depth gathers with no operands, None, ConstOffset,
Offset and ConstOffsets; only unused capability 25 disappears. It also verifies
idempotence and preservation of unrelated instructions/capabilities. Export tests
inspect the actual published DMSV and cache tests change the pass source.

`shader_gather_fixture.py` copies the donor `texture_ops_fixture.py` Maxwell
programs and helpers from `test_txq_type.py`. `texture_ops_cases.h` and
`texture_ops_matrix.h` are unchanged donor CPU oracles. `vk_gather_capability.c`
copies `texture_ops.c`, adapting its input to target uncompressed DMSV and its
extra driver-buffer binding. It deliberately disables `shaderImageGatherExtended`
when creating the standalone Vulkan device. No user device is selected or operated
by the fixture generator. Its Vulkan runner receives a bundle and variant number.

## Reproduce

Put the NDK shader tools and dotnet on PATH. Use a translator built from this
repository. All fixture assets are synthetic.

```sh
python -m unittest discover -s tests -p 'test_shader_gather_capability.py'
python tests/shader_gather_fixture.py /tmp/gather /path/to/dmsh_xlate.dll
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android28-clang"   -O2 -Wall -Wextra tests/vk_gather_capability.c -o /tmp/gather-runner -lvulkan -lm
```

On an authorized test device, copy the runner and bundles; invoke
`gather-runner clean.dmsv VARIANT`. Positive variants are
`0 1 2 3 4 6 8 9 10 13 16 19 22`, with 144 independently checked cases each.
The runner writes binary result/guard records to stdout; redirect it to a file.
Control variants 0 and 10 must exit 77 with `control.dmsv`.
Required-capability variants `11 12 14 15 17 18 20 21` must exit 77 with `clean.dmsv`.

Validation on the owned Android API 35 Lavapipe emulator: 13 × 144 = 1,872 cases,
7,488 result words plus surrounding guards; two uncleaned negative controls and
eight retained-requirement controls. Every executed module passes SPIR-V validation.
This demonstrates feature-disabled execution on Lavapipe, not qualification on a
physical GPU without the feature and not game playability.

## Preserved baseline limits

Variants 5 and 7 have a pre-existing invalid dynamic `Offset` on `OpImageFetch`;
variant 23 has a pre-existing out-of-bounds full 2D query extract. The generator
checks that both pre-cleanup and cleaned modules fail for these same reasons.
They are not executed. These translation fixes are outside this capability port.
Modes 1–7 of the copied donor oracle are retained but were not used for this
capability check. Production native code is unchanged; no new engine build is
claimed.
