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
unchanged for the seven applicable passes. It runs 84 cases / 1344 sampled
pixels plus output guards and conditional-clear checks through the production
NVN replay and real Vulkan:

- CPU updates, current and prior compute-written texture/sampler handles,
  including a sampler-only producer at array element 7;
- two later known overwrite ranges, with a no-extra-submission assertion;
- graphics and compute consumers, pool rebasing, holes, short bindings;
- an independently GPU-written 64-bit storage pointer;
- a still-open linear R32UI render target producing the handle;
- older inline updates followed by linear or transform-feedback GPU writes,
  checked independently against the pre-fix host;
- disabled rendering surviving a compute-triggered internal submission cut.

Target adapters use aligned registered memory pools, recorded UBO updates in
place of source push constants, and the existing 256-byte storage alignment.
The independent sampler array now uses the original donor offset 128 and
selects element k from offset+8*k, matching the texture array. This additional
port comes from nxnative `a603c99542b56bf4b351ba34a6bca6ff8db2f633`
(`sampled_handle`, `S_SAMPU`). Fixed sampler slots/IDs and same-word combined
handles stay unchanged. The packed DMSV register/offset format is unchanged;
its per-element adapter checks signed overflow. Preflight covers the complete
sampler array before any allocations. The parent reads the first word for
every element and fails the CPU border-color case before any GPU producer. Donor pass 4 (incompatible 2D
view used by a 3D sampler) fails on the baseline before any GPU producer and
is excluded here. No new dimensional-view behavior is claimed. The later-known sampler
overwrite covers all eight bytes, matching the donor known-fill case, using
the target deferred inline-update path.

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
`1` the current compute producer with a graphics consumer; `16` the
transform-feedback producer; `17` the sampler-only producer.

Validated on the task-owned Android 15 ARM64 emulator with Lavapipe. Baseline
current-compute and linear-producer cases return zero instead of green; the
port passes both render-pass modes. Independent review found that graphics
producers could leave an older overlapping update deferred; the real linear
and transform-feedback regressions failed before the fix. The target now
lands those older overlapping updates before the producing draw (and before
transform-feedback control-buffer copies), preserving donor write order. The baseline includes only unrelated
PR #361 output-mask changes; its readiness paths match main.

Limits: this covers CPU-decoded descriptors using producers already tracked
by this backend. It does not add 8192-entry uniform tables, general CPU
readiness or indirect index conversion, additional graphics-storage write tracking, or clear-only linear
attachment writeback. Fills and landed updates can cause a conservative wait;
the donor's known-fill snapshot optimization is not part of this port. Existing
`vk_ubd_read` diagnostics still log covered writes as pending even when the
later inline bytes make the descriptor read safe. No physical-GPU performance,
gameplay, new playable-game count or Switch 2 support is claimed.


## Indexed-quad follow-up

Depends on #369 and #368. nxnative's pinned
`1f01e28c87da5233dfa14fb31fabf1535df33013` calls `cpu_ready` before its
CPU index conversion (`nvn_vk.c:3686`); `cpu_access_ready` also materializes
later known writes. This follow-up applies that existing contract to the
target's direct indexed-quad CPU conversion.

Preflight checks the complete declared index range and waits before any
draw-specific descriptors or scratch allocations. Native GPU-index draws
and indirect draws keep their existing path. After preflight, the adapter
uses the target's existing uniform-shadow snapshot pattern
(`vk_scratch` + `vk_ubd_read`) once for the input range. Conversion reads
the snapshot using the donor's byte-copy index load, retaining the existing
quad triangle pattern and supporting unaligned 8-, 16-, and 32-bit input.

The snapshot costs one transient input-sized scratch allocation/copy per
indexed-quad draw; it avoids scanning GPU/deferred-write records for every
expanded index. Physical-device performance is unmeasured.

The focused ASan/UBSan host test checks all index widths, unaligned reads,
deferred bytes, full-span bounds/overflow, ignored non-quad/indirect paths,
and unchanged generated nonindexed quads. Device fixtures reuse the existing
producer and independent color expectations. Six modes cover CPU memory,
CPU deferred updates, current/prior GPU writes, and later full/partial known
overwrites. Each runs three widths, two base-vertex arrangements, and enabled/
disabled rendering, with two instances and a nonzero base instance recorded.

The parent passes CPU memory/prior-GPU controls but fails the other four
modes with red pixels instead of green. The follow-up passes all 72 cases
(1,152 pixels plus guards) in each rendering mode, alongside the original
84 descriptor cases. Both DM_VK_RP=0 and 1 pass: 312 total cases, 4,992 pixels,
plus guards and conditional-state checks. The normal native build compiles
192 sources; the changed replay source is rebuilt after the final adapter.

`NX2A_INDEX_ONLY=1` selects the new cases; `NX2A_INDEX_CASE=0..5` selects
one producer mode. The same build/run commands above apply.

This does not add new primitive conversion, custom restart, 8-bit index
fallback, indirect quad conversion, new GPU-write producer tracking, or a
game-specific workaround. There is no physical-GPU or gameplay qualification.
