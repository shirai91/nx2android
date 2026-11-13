# Existing full texture/sampler pool port

This ports nxnative's Android parity implementation at `1f01e28c87da5233dfa14fb31fabf1535df33013`: `spv_pool.py`, `spv_descriptor_table.h` and its builder subset, `resource_pools.inc`, and the table-layout/descriptor-writing portions of `uniform_table.inc` and `nvn_vk.c`. Initial resource-pool implementation: `0a84c8dea179aff5aab5205da7c1c7f84cc012e8`. It reuses the shader helpers from PR #355.

The exporter preserves the logical 20-bit texture and 12-bit sampler indices, converts the full arrays to bounded runtime arrays, and compacts binding numbers without changing guest buffer associations. The runtime deduplicates registered resources, uploads the donor's logical-to-compact maps, and selects descriptor layouts/pipelines by live counts. The target's existing texture registration history is also used when building the maps. Samplers loaded through memory retain the target's current-registration policy. Existing `android_dynamic_slots=on` profiles still use 32 logical slots.

DMSV records remain 20 bytes: kinds 17/18 describe texture/sampler pools with `count=1`, `reg=pool slot`, and `off=logical range`; kind 19 describes the bounds UBO. Kind 20 is an internal map SSBO appended at shader load; serialized kind-20 records are rejected. Runtime pool descriptors are written in the donor's bounded chunks; ordinary descriptor caching remains unchanged. Pool pipelines are built synchronously and excluded from prewarm records because their layouts depend on live registrations. Shader variant IDs distinguish both graphics and compute pipelines. Pipeline cache ABI/record version changes discard old layouts.

## Synthetic checks

Run the ordinary suite with the NDK shader tools on PATH. `test_resource_pool_export.py` compiles real SPIR-V, checks guest binding identity after compaction, and validates both the Python lowering and C remapper with `spirv-val`. The donor C driver also checks malformed/missing-array rejection under ASan/UBSan. `test_resource_pool_runtime.py` exercises sparse maps, duplicates, views, holes, rebasing, sampler bounds and the target's replay-job history selection under ASan/UBSan.

Build the production host and translator, then build the manual device fixture:

```sh
dotnet build nxnative/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj -c Release -o /tmp/pool-translator
python nxnative/dm/android/build.py --game 0100000000010000 --ndk "$ANDROID_NDK_HOME" --out /tmp/pool-host --exe --jobs 8
python tests/run_vk_resource_pools.py --ndk "$ANDROID_NDK_HOME" --host-build /tmp/pool-host --translator /tmp/pool-translator/dmsh_xlate.dll --out /tmp/pool-replay
```

The build script does not contact a device. With an authorized test device, copy `resource-pools` and `shaders.dmsv` into a dedicated directory and run there with `DM_ROOT` set to that directory. Set `DM_VK_NO_ZEROCOPY=1` for the emulator. Test both `DM_VK_RP=1` and `DM_VK_RP=0`.

The compute shader is the donor's actual Maxwell program: each lane loads a handle from storage, performs a dynamic texture/sampler lookup, and writes its result. The fragment fixture applies the same donor oracle through the real graphics pipeline. Target adapters supply 256-byte storage alignment, aligned synthetic NVN memory pools, texture object headers and DMSV packaging. The checks cover 2,048 samples across IDs 1/2/31/32/301/4096/65536, sampler 4095, holes/out-of-range indices, pool changes, empty registrations, borders, re-registration, live-count layout changes and pipeline reuse; surrounding output words must remain intact. Async pipeline compilation remains enabled to exercise pool pipelines' synchronous path.

Separate fixture invocations with `NX2A_POOL_LIMIT_ONLY=1`, `NX2A_POOL_FEATURE_ONLY=1`, and `NX2A_POOL_ALLOC_ONLY=1` verify rejection before Vulkan module creation when map descriptors exceed device limits or required indexing features are absent, and allocation/reuse of 2,049-sampler sets across submission slots. The latter uses layouts without a pipeline; actual shader layouts remain subject to per-stage and combined pipeline limits.

`NX2A_POOL_VIEW_ONLY=1` checks the actual descriptor image views and Maxwell readback for integer texture/view swizzles, format-listed allocations and incompatible native 1D images. Run it once with `DM_VK_UBWC=0` and once with `DM_VK_UBWC=1`. `NX2A_POOL_MAP_ONLY=1` rejects standalone and mixed serialized internal-map records. The host topology regression executes the production selection block across point/line/point draws without rebinding, including variants whose optimized resource interfaces differ.

Final validation: 340 host tests (9 skipped), all 192 Android host sources built, translator build with zero warnings/errors, and 2,048 sample checks in each render-pass/dynamic mode. The separate view, map, feature, limit and allocator invocations all passed on Android 15 ARM64 SwiftShader.

## Qualification limits

Verified on Android 15 ARM64 SwiftShader and with the host tests. No physical GPU or game playability is claimed. Big Brain Academy's arbitrary-handle shaders are a concrete consumer, but its assets were unavailable for this qualification. Native geometry-stage pool use has not been exercised on the emulator (it has no geometry-shader support).

This is the existing sampled-image/sampler pool implementation, not every descriptor-related feature from the donor. Uniform-buffer handle tables, wrapped/mixed buffer textures, narrow integer pool formats, and attachment-feedback handling need separate ports. Pool image views support float/normalized formats and the donor's raw 32-bit integer-to-float views; incompatible dimensions and unsupported narrow integer views use the dummy. Required Vulkan runtime-array/nonuniform-indexing support and device descriptor limits still apply. Per-game performance has not been measured.
