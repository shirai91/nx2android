# Existing native logic-operation port

This reuses nxnative Android parity `e6c73832b` and `color_operations` / `logic_format` at `1f01e28c87da5233dfa14fb31fabf1535df33013`. It depends on #354 (effective alpha state and format classification) and #361 (fragment output masks); those implementations are reused. The local dependency merge is `2099cbe18f00c7f612a540d67f3651a4bb350f80`; the new logic delta starts there.

All 16 NVN operations use Vulkan's matching stored-bit truth tables on the donor's eligible unsigned integer and normalized formats. Non-COPY logic overrides blending. Active unsupported/mixed formats or effective alpha testing retain the donor's COPY behavior. Devices without `logicOp` also use that existing COPY fallback; `DM_VK_NO_LOGIC_OP` allows exercising it. No software fallback algorithm is added.

`test_logic_operations.py` runs the production helper and reused classification under ASan/UBSan: 33 operation, format, feature, alpha, output-hole and blend-precedence cases. A second host test executes the real prewarm worker with persisted enabled/disabled keys and verifies the current device gate before shader/pipeline creation. The inherited output-mask/alpha tests retain their own scope. `vk_logic_operations.c` adapts the donor `tests/logic.inc` oracle to aligned target memory, viewport/channel-mask initialization and ordinary target recording. The four formats are RGBA8, RGBA16, RG32UI and RGBA8UI. Actual UINT declarations are used; float-to-integer output reinterpretation remains a separate open PR (#163).

```sh
python nxnative/dm/android/build.py --game 0100000000010000 --ndk "$ANDROID_NDK_HOME" --out /tmp/logic-host --exe --jobs 8
python tests/run_vk_logic_operations.py --ndk "$ANDROID_NDK_HOME" --host-build /tmp/logic-host --out /tmp/logic-replay
```

The build script does not contact a device. On an authorized device, copy `logic-operations` and `shaders.dmsv` into a dedicated directory and set `DM_ROOT` to it. Run `./logic-operations` with `DM_VK_RP=0/1`, and repeat with `DM_VK_NO_LOGIC_OP=1`. Use `DM_VK_NO_ZEROCOPY=1` on the emulator. Set `NX2A_LOGIC_PREWARM=1` to exercise learned records: run natively first, then restart the same directory with `DM_VK_NO_LOGIC_OP=1`. A Vulkan-call observer rejects enabled pipeline requests on the disabled device. The fixture prints whether native logic is available and selects the corresponding donor oracle. Each invocation checks 268 pixels, including scissor/masks and an unwritten floating-point MRT hole between eligible outputs.

Qualification: 327 host tests passed (9 skipped), 192 Android host sources built. Android 15 ARM64 Lavapipe (`llvmpipe`, LLVM21.1.4) passed native and forced-COPY modes with both render paths; SwiftShader, which reports no logic support, passed the fallback. Native and disabled-feature restarts with learned pipeline records passed in both render modes; the pre-fix control failed on an enabled recorded key. The existing alpha regression also passed all 768 cases in each render mode on the combined branch. Physical GPUs, commercial-game playability, and unsupported format/alpha combinations beyond the donor behavior are not claimed.
