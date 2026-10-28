# Android performance contribution from @freapsy

**Project direction, reproduction and physical-device testing: [@freapsy](https://github.com/freapsy).**
The implementation and automated capture/analysis were prepared with Codex
assistance. This contribution follows [issue #2](https://github.com/shirai91/nx2android/issues/2).

The observation was that a working native ARM64 port could underuse available
CPU/GPU performance. An open ADPF session did not by itself keep the serial
render/replay workload on faster CPUs. In one earlier capture, about 39.7%
of render CPU running time was on CPUs 0–3, versus about 0.47% on CPU 7.
The context-owned KGSL request provides an optional alternative to a global
profiling request that disables ordinary power scaling/slumber.

This PR carries only opt-in CPU-capacity selection, custom-driver packaging
and loading, the KGSL hook and its loader patch, and focused tests/docs.
It does not carry the earlier private overlay's shader/renderer changes,
Wonder-specific SEAD frame observer/governor, widescreen work or timing
instrumentation. No game profile is changed by default.

## Device result versus PR validation

The **hardware-tested build** used upstream base
`3ae3fa5523f38bd861a273b2b8850483b0e37858` plus the previously developed local
overlay, Wonder Switch **1.0.0**, original pre-adapted ARM64 game modules,
Turnip v26.0.0 R8 and the two performance controls. Its APK SHA-256 was
`99fdc8962f3f48ea2237a4eb57fc00f9a8a684e6691506c56341d7c8dbe69f89`;
host Build-ID `4e0448a88b7f04a6`. The device was a Motorola Edge 30 Ultra,
Snapdragon 8+ Gen 1 / Adreno 730, on a kernel matching
`LineageOS/android_kernel_motorola_sm8475` revision `8b49a4a7343d`.

| Stationary animated playable start scene | Actual presentations/s | Complete one-second bins |
| --- | ---: | ---: |
| 90.065 s, scheduler/causal capture | 53.383 | 52–55, 90 bins |
| 45.057 s, sparse capture | 53.044 | 52–55, 45 bins |

The counts use deduplicated SurfaceFlinger actual-present timestamps, not
display Hz or the on-screen counter. The sparse trace records accepted
context setup, constraint activation at the existing level 0, and 900 MHz
throughout its complete active window with ordinary GPU power control
enabled. The wider scheduler trace hit its 256-MiB file limit roughly five
seconds before the 90-s display measurement ended; it is not a complete
90-s power trace. Display timestamp collection was independently complete.

The 90-s run's HAL skin snapshots were 29.943 °C before app launch,
38.307 °C at FPS sampling start and **41.947 °C / thermal status 2** afterward.
Thermal protections were retained. No resolution option was manually lowered,
but the game's internal dynamic resolution varied: these are not controlled
equal-resolution or thermally matched A/B speedup measurements. A comparison
with a refused perfd request averaged 33.334 FPS and included lengthy 30-FPS
holds; it is not a pure-clock percentage-gain baseline.

P95 presentation spacing remained about 33.242 ms at 60 Hz. Every complete
second above 50 does not mean every frame arrives within 20 ms or perfect
frame pacing. Home/resume rendering was checked. No downward frequency event
was recorded during the three-second Home interval, so no idle-power saving
or immediate release is claimed. The trace showed the expired old constraint
clearing and a fresh activation around resume.

**This PR is rebased onto upstream `9d67edec2175ae5f4beadbd65f37eaa88105d132`.**
It is a review draft: the device results above must not be attributed to a
newly built unmodified current-main APK with only this PR. Current upstream
has additional renderer/pacing changes and its own Wonder 1.2.1 profile.
No claim is made that this small PR alone reproduces the older overlay's
frame-rate behavior. The unchanged, working device installation was retained.

The focused source checks and NDK compile results are recorded in the PR
description. The measured affinity/hook bodies are retained; loader comments
and test scratch paths were made suitable for a public contribution.
The [machine-readable evidence](evidence/freapsy-performance-20261028.json)
contains only selected presentation statistics, not system-wide device logs.
Game data, extracted shaders, keys, signing material, APKs and saves are
not part of this contribution.

## Transfer and acceptance

The controls do not depend on Wonder, title IDs, shader hashes or a specific
CPU number. CPU selection requires readable capacity data and permission
within the caller's allowed set. GPU selection requires the reviewed
Adreno/KGSL kernel contract and matching optional custom-driver integration;
it does not cover Mali or every Snapdragon kernel. Existing thermal/max
limits remain and the higher clock has a real energy/heat cost.

Moving gameplay, other courses/games, long warm sessions and battery-power
behavior remain untested. The CPU-only, GPU-only and combined effect still
need matched tests on current upstream. Runtime scope and build/rollback
instructions are in [the driver notes](../nxnative/dm/android/driver/README.md).

Primary kernel references: [context ownership/request](https://github.com/LineageOS/android_kernel_motorola_sm8475/blob/8b49a4a7343d/drivers/gpu/msm/adreno.c),
[Gen7 submission](https://github.com/LineageOS/android_kernel_motorola_sm8475/blob/8b49a4a7343d/drivers/gpu/msm/adreno_gen7_hwsched_hfi.c),
[activation](https://github.com/LineageOS/android_kernel_motorola_sm8475/blob/8b49a4a7343d/drivers/gpu/msm/adreno_ringbuffer.c),
and [limits/expiry](https://github.com/LineageOS/android_kernel_motorola_sm8475/blob/8b49a4a7343d/drivers/gpu/msm/kgsl_pwrctrl.c).
