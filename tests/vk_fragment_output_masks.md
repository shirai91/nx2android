# Existing fragment-output mask port

Source: `seedds/nxnative` commit `742318ae5d7328c35131a03c314ce45d66ca9776`,
`color_operations` and the `tests/logic.inc` sparse-MRT oracle. The source records
Super Mario 3D World + Bowser's Fury volume draws that write outputs 0 and 2
while blending and channel writes remain enabled for targets 1 and 3.

The target already reflects output locations in `vkshader_t.out_locs`. This port
uses that map to disable writes and blending for absent fragment outputs. It
also preserves all eight outputs written by the replacement `flatdbg.frag`
diagnostic shader, matching the replacement-fragment exception in the Metal
path's colour-attachment rule (`nvn_mtl.m`). No new SPIR-V transformation is
added. Existing attachment writeback bookkeeping is unchanged.

```sh
python -m unittest discover -s tests -p test_fragment_output_masks.py
python tests/run_vk_fragment_output_masks.py --ndk "$ANDROID_NDK_HOME" --host-build /tmp/output-mask-host --out /tmp/output-mask-probe
adb -s "$TEST_DEVICE" shell mkdir -p /data/local/tmp/output-mask-probe
adb -s "$TEST_DEVICE" push /tmp/output-mask-probe/fragment-output-masks /tmp/output-mask-probe/shaders.dmsv /data/local/tmp/output-mask-probe/
adb -s "$TEST_DEVICE" shell chmod 755 /data/local/tmp/output-mask-probe/fragment-output-masks
adb -s "$TEST_DEVICE" shell 'DM_VK_SHADERS=/data/local/tmp/output-mask-probe/shaders.dmsv DM_VK_RP=1 /data/local/tmp/output-mask-probe/fragment-output-masks'
```

Repeat with `DM_VK_RP=0`, and with `DM_VK_X_FLAT_VS=52:1` for the existing
magenta diagnostic shader. The runner builds fixtures only; it never contacts a
device. The native host build is the usual Vulkan `dm/android/build.py --exe`.

The replay adapts the donor's seeded-attachment oracle to four attachments and
COPY to isolate fragment-output eligibility. Native logic operations have a
[separate replay](vk_logic_operations.md). This fixture checks 64 pixels,
float attachments left untouched, scissor boundaries, output-map changes with
the same attachments, pipeline reuse and readback guards. An observer checks the
actual production pipeline masks and blend flags before driver creation. The old
host fails this observer on unwritten target 1 with write mask 15 and blending on.

Flat diagnostic mode expects magenta in all four bound targets (RGBA8 or R32F,
as appropriate). Commercial gameplay and physical GPUs are not qualified by
this synthetic probe; results for a particular revision belong in the PR.
