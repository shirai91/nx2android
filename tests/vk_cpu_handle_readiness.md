# GPU-produced descriptor readiness

This ports nxnative `26c898e4e` and the existing `cpu_access_ready`,
`uniform_ready`, `shader_cpu_ready`, `gw_wait_prior` and live tracker growth
at `1f01e28c87da5233dfa14fb31fabf1535df33013`, from the Android parity work
in “Assess Linux bundle support”. Production adaptations use the target's
GPU-write records, deferred uniform overlays and submission ring. There is
no new translation algorithm or shader ABI.

CPU descriptor decoding now waits for overlapping GPU writes before any
scratch, descriptor sets or indirect-draw scratch are allocated. A wait
includes completion-thread readback into copied guest pools. Internal segment
restart invalidates transient Vulkan state and preserves logical NVN bindings
and conditional rendering. Live dependencies are never evicted at the former
1024-entry diagnostic limit. Later deferred inline updates can cover an earlier
GPU write without a submission cut.

The host test runs the production tracking/preflight under ASan and UBSan,
covering current/prior producers, disjoint/partial/full overwrite coverage,
2049 live writes, bounds/overflow, texture/sampler words, storage pointers,
and the target's zero-size-as-one-word uniform convention.

The manual device fixture adapts `uniform_texture_array(table=0)` from donor
`dm/gfx/nvn/vulkan/tests/t_replay.c`. Its independent color expectations are
unchanged for the seven applicable passes. It runs 70 cases / 1120 sampled
pixels plus output guards and conditional-clear checks through the production
NVN replay and real Vulkan:

- CPU updates, current and prior compute-written texture/sampler handles;
- two later known overwrite ranges, with a no-extra-submission assertion;
- graphics and compute consumers, pool rebasing, holes, short bindings;
- an independently GPU-written 64-bit storage pointer;
- a still-open linear R32UI render target producing the handle;
- disabled rendering surviving a compute-triggered internal submission cut.

Target adapters use aligned registered memory pools, recorded UBO updates in
place of source push constants, and the existing 256-byte storage alignment.
The selected independent sampler word is bound directly: indexed independent
sampler arrays are a separate missing feature. Donor pass 4 (incompatible 2D
view used by a 3D sampler) fails on the baseline before any GPU producer and
is excluded here. No new dimensional-view behavior is claimed.

Build the normal 192-source Android host, then the fixture (no device access
is performed by this Python runner):

```sh
python nxnative/dm/android/build.py --game 0100000000010000 --ndk "$NDK" --out "$HOST_BUILD" --exe --jobs 8
python tests/run_vk_cpu_handle_readiness.py --ndk "$NDK" --host-build "$HOST_BUILD" --out "$FIXTURE"
```

On an explicitly selected test device, put `cpu-handle-readiness` and
`shaders.dmsv` in a dedicated scratch directory. Run with `DM_ROOT` pointing
there and `DM_VK_NO_ZEROCOPY=1`, once each with `DM_VK_RP=0` and `1`.
`NX2A_HANDLE_CASE=15` selects the linear producer; `14` the pointer producer;
`1` the current compute producer with a graphics consumer.

Validated on the task-owned Android 15 ARM64 emulator with Lavapipe. Baseline
current-compute and linear-producer cases return zero instead of green; the
port passes both render-pass modes. The baseline includes only unrelated
PR #361 output-mask changes; its readiness paths match main.

Limits: this covers CPU-decoded descriptors using producers already tracked
by this backend. It does not add 8192-entry uniform tables, general CPU/index
readiness, additional graphics-storage write tracking, or clear-only linear
attachment writeback. Fills and landed updates can cause a conservative wait;
the donor's known-fill snapshot optimization is not part of this port. Existing
`vk_ubd_read` diagnostics still log covered writes as pending even when the
later inline bytes make the descriptor read safe. No physical-GPU performance,
gameplay, new playable-game count or Switch 2 support is claimed.
