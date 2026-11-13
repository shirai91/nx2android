# Existing texture-family alias port

The implementation copies `texture_backing_format` and its matching boundaries from nxnative Android parity `4fe574ebc` / `ccfd8e118` at `1f01e28c87da5233dfa14fb31fabf1535df33013`. It reuses nx2android's existing storage cache, lifetime and view code. RGBA/RGBX linear/sRGB descriptions share one linear RGBA allocation; BGRA/BGRX use a separate BGRA family. Individual image views retain their original color conversion and alpha mapping.

Matching applies only to memory-backed, nonvirtual, single-sample 2D/2D-array descriptions without explicit layouts. Existing target color IMAGE flags remain part of the key because they control storage-image usage. This port does not add sharing across those flags, different shapes, unrelated format families, explicit layouts or virtual pools.

`test_texture_format_aliases.py` executes the production key-building block with the real NVN types under ASan/UBSan, checking 64 family combinations and exclusions. The device fixture reuses the donor `recorded_texture_srgb.h` oracle. Its DrawTexture operation is adapted to an ordinary synthetic guest draw using nx2android's existing descriptor/pipeline path, so this PR does not depend on #357. Guest games/assets are unnecessary.

Build and run manually:

```sh
python nxnative/dm/android/build.py --game 0100000000010000 --ndk "$ANDROID_NDK_HOME" --out /tmp/alias-host --exe --jobs 8
python tests/run_vk_texture_aliases.py --ndk "$ANDROID_NDK_HOME" --host-build /tmp/alias-host --out /tmp/alias-replay
```

The script only builds. On an authorized device, copy `texture-aliases` and `shaders.dmsv` to a dedicated directory, set `DM_ROOT` to it and run `./texture-aliases`. Use `DM_VK_NO_ZEROCOPY=1` on the emulator. Run all combinations of `DM_VK_RP=0/1`, `DM_VK_UBWC=0/3`, and `NX2A_ALIAS_ARRAY` unset/set to 1.

Each invocation checks 4,096 pixels: eight source format pairs, both first-writer orders, four alternating clear/sample phases and 64 pixels per phase. Both directions must preserve the donor's sRGB transfer function and X-format alpha=1 behavior while reusing cached views. The baseline fails the first sample (zero instead of 13). This is synthetic Android 15 ARM64 SwiftShader qualification, not a physical-GPU or commercial-game playability claim.
