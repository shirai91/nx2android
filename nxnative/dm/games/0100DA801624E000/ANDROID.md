# TUNIC on Android

Status: **experimental — requires testing**. No game was built, launched or played for this change.

This source audit covers **1.0.3 (`v196608`)**, Switch 1 title ID `0100DA801624E000`, against nx2android `00f41ec9c0f4`. It identifies existing tool and runtime paths for an experimental port; it does not establish Android playability. Other game versions are outside this assessment.

## Compatibility evidence

The [upstream game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100DA801624E000/README.md) describe Unity 2020.3 IL2CPP, SDK 14.3.0, NVN rendering and FMOD audio, with no game-specific binary patch. The profile records the exact `main`, movie-library `subsdk0` and `sdk` builds. The local Unity asset readers and static GLSLC-output splitter handle these containers; the 11 player shaders embedded in `main` are static inputs, not runtime shader capture.

Existing paths: `nxnative/dm/gfx/tools/unityfs.py`, `shader_split.py`, `nxnative/dm/android/tools/shader_vk.py`, the Vulkan NVN backend, and the shared AudioOut/movie hooks with Android decoding. Shader input tables come from the pinned upstream profile. Apple pipeline caches and playback claims are not imported.

## Memory without new SDK patches

The exact SDK build lacks upstream's node-area compaction in this fork. The original node reservation is `0x3fe00000` (1,022 MiB); upstream reduces it to `0x3e00000` (62 MiB), saving **960 MiB of virtual address space**. This profile supplies additional address capacity using existing `mem_alias` and `mem_reserve` keys.

SDK 14 also retains larger allocation-alignment classes. The upstream 1 GiB application pool is preserved, keeping the initial StandardAllocator request below 1 GiB. The alias region grows by **8 GiB**, matching the porter's existing default headroom for unmeasured versions: `mem_alias 0x318000000` (12.375 GiB), `mem_reserve 0x340000000` (13 GiB). This provides margin for the unreduced node area and alignment gaps; it is not a measured late-game bound. No SDK compaction or generic Unity frame-rate changes are imported.

The [upstream memory reference](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/docs/reference/memory.md#the-sdks-address-region-node-area) explains the node reservation and alignment classes. Locally, `porter/pipeline.py::alias_headroom` already supports this sizing transformation, `game.py::header` emits it, and `hle_svc.c::mem_init` uses it with `MAP_NORESERVE`. Stack capacity stays 256 MiB and heap capacity 128 MiB. Reserved address space is not committed physical RAM; actual memory use and later allocation patterns still need testing.

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `0909b4ac280d9d77ecd1fae840e4b295` |
| `subsdk0` | `c781dd63daf0ddcdb9975da50a198f93` |
| `sdk` | `2aef2cffb104ee7c9e212fd2f4c731e9` |

Android starts docked with the on-screen gamepad visible. Use an arm64 Android device with BC texture support and the Vulkan features needed by the shaders. The profile requests no frame-rate override and supplies no `tested` row.

## Required testing

1. Port the exact version from the player's own dump and keys; verify extraction, rewriting and shader translation complete.
2. Test the title, menus, movies, audio and gameplay with the intended controls; check for missing shaders or rendering errors.
3. Save, quit, restart and load. Exercise later areas and monitor address-region failures, physical memory and pacing.
4. Record the device, driver, version and tested route before raising the status.
