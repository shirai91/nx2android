# NVN alpha-test port validation

The runtime helpers `spv_alpha.h` and `spv_fragment_outputs.h` are copied unchanged
from nxnative's `codex/cross-platform-runtime` at
`1f01e28c87da5233dfa14fb31fabf1535df33013`, implemented by the Android parity work
in commit `9a0627d6a`. The fragment fixture and the color/depth/reference cases in
`vk_alpha_replay.c` come from that branch's `dm/gfx/nvn/vulkan/tests/alpha.frag`
and `alpha.inc`. The adapter uses nx2android's texture magic, explicit viewports,
DMSV shader keys, and registered memory pools. Its small vertex shader supplies
a full-screen triangle at depth 0.25.

The optional device fixture links the production Android host objects, replacing
only the executable entry point and test-program shader-key lookup. It submits
real NVN command records through the Vulkan replay, shader variant, pipeline,
and readback code. It checks 768 color/depth ramp pixels across all eight compare
functions, six references (including NaN), ordinary and early fragment tests,
storage writes before discard, and integer-output bypass. The donor's pipeline
creation-count assertion is omitted because nx2android's statistics API is a
stub; variant/layout cache reuse is covered by the host tests.

Build the native host first, then the fixture (choose absolute output paths):

```sh
python nxnative/dm/android/build.py --game 0100000000010000 --ndk "$ANDROID_NDK_HOME" --out /tmp/alpha-host --exe --jobs 8
python tests/run_vk_alpha_replay.py --ndk "$ANDROID_NDK_HOME" --host-build /tmp/alpha-host --out /tmp/alpha-fixture
adb -s DEVICE shell mkdir -p /data/local/tmp/nx2a-alpha-test
adb -s DEVICE push /tmp/alpha-fixture/alpha-replay /tmp/alpha-fixture/shaders.dmsv /data/local/tmp/nx2a-alpha-test/
adb -s DEVICE shell 'cd /data/local/tmp/nx2a-alpha-test && chmod 700 alpha-replay && DM_ROOT=/data/local/tmp/nx2a-alpha-test ./alpha-replay'
```

Host regressions use `python -m unittest discover -s tests -p test_vk_alpha.py`;
set `ANDROID_NDK_HOME` to enable the real glslc/spirv-val cases. These tests cover
the public reference recorder, comparison masks, integer/raster-discard bypass,
pipeline layouts and shader variants combined with integer texture, fragment
depth, and dual-source transforms. Separate variants share the same reference
push range and cache identity includes alpha enablement. Persistent pipeline
cache versions change with this contract.

The copied transformation supports SPIR-V through 1.3 with a float4 output at
location 0/index 0 and no existing push constants. Unsupported transformations
log a diagnostic and use the existing base-module fallback. These synthetic
checks do not qualify commercial games or physical Android GPUs.
