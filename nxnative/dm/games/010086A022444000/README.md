# BALL x PIT on Android

Status: **experimental — requires testing**. The registered 1.293 profile has not been qualified on Android. Community testing of derived 1.301 reaches gameplay with missing graphics. A retest of the separate performance-configuration change reported that switching to handheld no longer crashes; the registered build remains untested.

This source audit covers **1.293 (`v393216`)**, Switch 1 title ID `010086A022444000`, against nx2android `00f41ec9c0f4`. It identifies existing tool and runtime paths for an experimental port; it does not establish Android playability. The later 1.301 report below does not qualify the registered version.

## Game version

Registered: **1.293 (`v393216`)**. The community's **1.301 (`v458752`)** report uses a derived profile with local SDK node-area cuts and does not qualify this registered build.

## Frame rate

No measured frame-rate claim for the registered version. Missing player and scenery rendering in the community run prevents treating that run as normal gameplay performance.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Community device; model not specified | Samsung Xclipse 920 | 2.0.0, Vulkan 1.3.231 | Derived 1.301 | `42a80e6` retest | @YGOmod reports handheld switching no longer crashes; player and much of the scenery remain missing |

The registered 1.293 build has no device qualification. Audio, saves/reload, controls, later snow/ice/floor effects and a 1280×720 gameplay frame still need explicit coverage. I do not have the game files; community testing with your own copy is needed.

## Options

Start with docked mode and the full gamepad layout. `--experimental` permits a derived profile but does not establish compatibility. Keep any local SDK edits and the exact source revision documented when comparing builds.

## Quirks and known issues

- Player and scenery rendering remain incomplete in the reported 1.301 run despite all 38,099 translated shaders completing with `failed: {}`.
- The handheld retest is positive, but its quoted log is the existing two-argument configuration hook. It does not show that the added one-argument hook executed or establish which SDK function caused the earlier crash.
- The shared one-argument hook has been separated from this profile at the maintainer's request; it is not included in this branch. Its isolated fix still needs the requested mode-switch log identifying that overload.
- Hull/domain use and later effects remain unverified; see the inventory below.

## Compatibility evidence

The [upstream game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010086A022444000/README.md) describe Unity 6 IL2CPP, SDK 20.5.19, NVN rendering and FMOD audio, with no game-specific binary patch. The profile records the exact `main`, movie-library `subsdk0` and `sdk` builds. The local Unity asset readers and static GLSLC-output splitter handle these containers; the 11 player shaders embedded in `main` are static inputs, not runtime shader capture.

Existing paths: `nxnative/dm/gfx/tools/unityfs.py`, `shader_split.py`, `nxnative/dm/android/tools/shader_vk.py`, the Vulkan NVN backend, and the shared AudioOut/movie hooks with Android decoding. Shader input tables come from the pinned upstream profile. Apple pipeline caches and playback claims are not imported.

The Addressables bundles supply type trees for the loose Unity assets through the existing deferred `NoTree` pass. `pad_layout full` preserves right-stick aiming; the existing NVN window-crop path handles the documented 1280×720 crop.

The shader inventory also contains 76 hull and 76 domain keys for `IceShieldBaseParticle` and `FloorDisplacement`. None appear in upstream's recorded first-dive/base route. The current Android draw path has no active tessellation support, so later snow/ice and floor effects require particular attention: if those stages are invoked, they will need a runtime change. Their presence alone does not establish that the game invokes them.

## Memory without new SDK patches

The exact SDK build lacks upstream's node-area compaction in this fork. The original node reservation is `0x3fe00000` (1,022 MiB); upstream reduces it to `0x3e00000` (62 MiB), saving **960 MiB of virtual address space**. This profile supplies additional address capacity using existing `mem_alias` and `mem_reserve` keys.

SDK 20 already uses 2 MiB alignment for its large regions, so it does not need the older SDK alignment cut. Adding **1 GiB** restores the missing 960 MiB and leaves another 64 MiB: `mem_alias 0x158000000` (5.375 GiB), `mem_reserve 0x180000000` (6 GiB). Post-node capacity is 4.376953125 GiB, compared with 4.314453125 GiB in upstream's compacted 4.375 GiB alias region. The hardware application pool is retained.

The [upstream memory reference](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/docs/reference/memory.md#the-sdks-address-region-node-area) explains the node reservation and alignment classes. Locally, `porter/pipeline.py::alias_headroom` already supports this sizing transformation, `game.py::header` emits it, and `hle_svc.c::mem_init` uses it with `MAP_NORESERVE`. Stack capacity stays 256 MiB and heap capacity 128 MiB. Reserved address space is not committed physical RAM; actual memory use and later allocation patterns still need testing.

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `ed9090f18ae37a5a0966f5cfe5a07cc2` |
| `subsdk0` | `b1b31ebc17b9417164a4f5fea049367e` |
| `sdk` | `8bfebccd6f5c1392597b364b9ab22a8d` |

Android starts docked with the on-screen gamepad visible. Use an arm64 Android device with BC texture support and the Vulkan features needed by the shaders. The profile requests no frame-rate override and supplies no `tested` row.

## Required testing

1. Port the exact version from the player's own dump and keys; verify extraction, rewriting and shader translation complete.
2. Test the title, menus, movies, audio and gameplay with the intended controls; check for missing shaders or rendering errors.
3. Save, quit, restart and load. Exercise later areas and monitor address-region failures, physical memory and pacing.
4. Record the device, driver, version and tested route before raising the status.

## Community bring-up and remaining issues

The reported 1.301 (`v458752`) build uses `main=ab2259988e930c928a0d6fceef29d1d4` and the same movie-library and SDK builds listed above. With `--experimental` and local SDK node-area cuts, it reached gameplay on Samsung Xclipse 920, driver 2.0.0, but the player and scenery were not rendering correctly. This is not a tested/playable claim for either version.

Its supplied shader-table diff contains real key changes: 68 vertex/pixel keys for `TimeSnareAOE` disappear and 64 for `TimeSnareFresnel` appear; the matching variation-pair changes are 34 removed and 32 added. Other differences include shifted table indices. The registered 1.293 tables remain the baseline; accepting locally generated 1.301 tables with `--experimental` does not validate their rendering. The supplied diff alone does not explain the missing player or scenery, The later translation log reports 38,099 of 38,099 shaders, including five geometry variants, with no translation failures; this does not prove correct shader semantics or explain the rendering failure.

The original handheld switch delivered operation/performance notifications before an SDK null-session dereference. [The retest](https://github.com/shirai91/nx2android/pull/346#issuecomment-7080822396) reports no crash after applying `42a80e6`, but quotes only `SetPerformanceConfiguration mode=1 cfg=0x10001`, the pre-existing two-argument hook. The maintainer requested separating the one-argument hook and an exact log from the mode change before accepting it as the cause-specific fix.

The contributor does not have the game files. Community testing of the mode switch, rendering, controls, audio, saves and later effects is still needed before changing the experimental status.
