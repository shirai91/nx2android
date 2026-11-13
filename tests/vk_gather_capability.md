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
`gather-runner clean.dmsv VARIANT` for variants 0 through 23 (144 independently
checked cases each). The runner writes binary result/guard records to stdout;
redirect it to a file. Control variants 0 and 10 must exit 77 with `control.dmsv`.
They are explicit test-loader rejection, not a Vulkan-driver rejection check.

## Follow-up texture translation port

The follow-up to #372 copies nxnative's existing texture translation fixes:

- `0dff2e865`: full size queries return zero for axes absent from the resolved
  descriptor and retain real dimensions, array layers and mip counts. Sampled
  1D declarations carry their required capability.
- `759594122`, `4255c6ad3`, `ea6dcf3f7`: Vulkan jobs select Ryujinx's existing
  fetch/gather offset lowering while preserving sample offsets for the existing
  exact downstream pass. The final donor accessor/interface and TexturePass
  condition are copied unchanged. The target exporter sets `host_target=vulkan`.

All 24 copied Maxwell variants now pass Vulkan 1.1 validation. The previously
invalid variants 5/7 (dynamic integer fetch offsets) and 23 (full 2D size query)
are included, with no exclusions. The copied donor CPU oracle is unchanged.
Dynamic gathers now use the existing donor lowering and need no extended-gather
feature for this corpus; synthetic direct Offset/ConstOffsets unit tests still
verify retention when those operands actually remain in a module.

Validation on the owned Android API 35 Lavapipe emulator: 24 × 144 = 3,456 cases,
13,824 result words plus surrounding guards, with the feature deliberately disabled.
The two uncleaned capability controls still exit 77. This demonstrates execution
on Lavapipe, not physical unsupported-GPU qualification or game playability.

Additional donor tests check seven resolved query shapes in both default/Metal
and Vulkan job modes, and fragment implicit/bias offsets: one original sample
becomes 256 native constant-offset samples with no LOD query or varying branch
that would change derivatives. Set `DMSH_TEST_XLATE` to this branch's freshly built
`dmsh_xlate.dll` and put dotnet plus the NDK SPIR-V tools on PATH:

```sh
PYTHONPATH=tests python -m unittest test_shader_query_dimensions test_shader_fragment_offsets test_shader_texture_target
```

The chain header is refreshed, and the Android runtime builds all 192 sources.
Modes 1–7 of the copied device oracle remain available but were not used for
this follow-up. Their filtering/driver behavior and physical-device gameplay
remain outside the qualification claimed here.
