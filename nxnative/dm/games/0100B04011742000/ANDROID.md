# Monster Hunter Rise on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **16.0.2 (`v2228224`)**, title ID `0100B04011742000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Existing registered 16.0.2 local profile plus Android sparse-pool upload/page table, dirty textures, RE frame cap, big-module rewriting, and separate texture/sampler paths exist.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100B04011742000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100B04011742000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/0100B04011742000/game.tsv:3`
- `nxnative/dm/android/gfx_vk_tex.c:258`
- `nxnative/dm/gfx/nvn/hle_nvn.c:394`
- `nxnative/dm/host/hle_fpslock.c:28`
- `nxnative/dm/tools/patch_nso.py:44`
- `nxnative/dm/games/0100B04011742000/game.tsv`
- `porter/pipeline.py:441`
- `nxnative/dm/android/tools/shader_vk.py:109`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `c9a3dd7702075ecd90101f4555389582` |
| `subsdk0` | `507f19d1240d09d00992248e8a0fd77e` |
| `sdk` | `2d369d9999e3e10e665106f840ffff9b` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

The local add-on ownership table holds at most 64 entries, while the upstream notes describe 252 licenses. This candidate covers the base game with the registered update; complete Sunbreak/cosmetic DLC availability is not claimed. Preserve the existing order_keep setting.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
