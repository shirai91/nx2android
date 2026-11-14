# BALL x PIT on Android

Status: **experimental — requires testing**. No game was built, launched or played for this change.

This source audit covers **1.293 (`v393216`)**, Switch 1 title ID `010086A022444000`, against nx2android `00f41ec9c0f4`. It identifies existing tool and runtime paths for an experimental port; it does not establish Android playability. Other game versions are outside this assessment.

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
