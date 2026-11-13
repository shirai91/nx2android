# The Rogue Prince of Persia on Android

Status: **experimental — requires testing**. No game was built, launched or played for this change.

This source audit covers **1.1.0 (`v262144`)**, Switch 1 title ID `01008D9022462000`, against nx2android `00f41ec9c0f4`. It identifies existing tool and runtime paths for an experimental port; it does not establish Android playability. Other game versions are outside this assessment.

## Compatibility evidence

The [upstream game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/01008D9022462000/README.md) describe Unity 6 IL2CPP, SDK 20.5.29, NVN rendering and FMOD audio, with no game-specific binary patch. The profile records the exact `main`, movie-library `subsdk0` and `sdk` builds. The local Unity asset readers and static GLSLC-output splitter handle these containers; the 11 player shaders embedded in `main` are static inputs, not runtime shader capture.

Existing paths: `nxnative/dm/gfx/tools/unityfs.py`, `shader_split.py`, `nxnative/dm/android/tools/shader_vk.py`, the Vulkan NVN backend, and the shared AudioOut/movie hooks with Android decoding. Shader input tables come from the pinned upstream profile. Apple pipeline caches and playback claims are not imported.

The RomFS plugin uses the existing NRO collection, rewrite, data-pack and loader path (`nro_collect.py`, `build_ipa_from_nsp.py`, `porter/datapack.py`, `hle_ro.c` and `loader.c`). Its load and gameplay remain device-test items.

## Memory without new SDK patches

The exact SDK build lacks upstream's node-area compaction in this fork. The original node reservation is `0x3fe00000` (1,022 MiB); upstream reduces it to `0x3e00000` (62 MiB), saving **960 MiB of virtual address space**. This profile supplies additional address capacity using existing `mem_alias` and `mem_reserve` keys.

SDK 20 already uses 2 MiB alignment for its large regions, so it does not need the older SDK alignment cut. Adding **1 GiB** restores the missing 960 MiB and leaves another 64 MiB: `mem_alias 0x158000000` (5.375 GiB), `mem_reserve 0x180000000` (6 GiB). Post-node capacity is 4.376953125 GiB, compared with 4.314453125 GiB in upstream's compacted 4.375 GiB alias region. The hardware application pool is retained.

The [upstream memory reference](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/docs/reference/memory.md#the-sdks-address-region-node-area) explains the node reservation and alignment classes. Locally, `porter/pipeline.py::alias_headroom` already supports this sizing transformation, `game.py::header` emits it, and `hle_svc.c::mem_init` uses it with `MAP_NORESERVE`. Stack capacity stays 256 MiB and heap capacity 128 MiB. Reserved address space is not committed physical RAM; actual memory use and later allocation patterns still need testing.

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `292cbdeb9e2b141bec2ddd8504c747b1` |
| `subsdk0` | `1e59dde5383b9cfb60f3e67e92add78b` |
| `sdk` | `a5813da32d12ef17099152ca79c9b24d` |

Android starts docked with the on-screen gamepad visible. Use an arm64 Android device with BC texture support and the Vulkan features needed by the shaders. The profile requests no frame-rate override and supplies no `tested` row.

## Required testing

1. Port the exact version from the player's own dump and keys; verify extraction, rewriting and shader translation complete.
2. Test the title, menus, movies, audio and gameplay with the intended controls; check for missing shaders or rendering errors.
3. Save, quit, restart and load. Exercise later areas and monitor address-region failures, physical memory and pacing.
4. Record the device, driver, version and tested route before raising the status.
