# ROMANCE OF THE THREE KINGDOMS 8 REMAKE on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.1.3 (`v851968`)**, title ID `0100C2A01A730000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Reuse local 1.1.3 profile. Newer PDRK/.fdata IDRK LZ4/zlib chunks, SDK 17 movie/audio renderer and primary-program extraction already exist locally. Optional ExecuteProgram1 remains a known limitation; baseline profile supports program0 as upstream initial bringup.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100C2A01A730000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100C2A01A730000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/0100C2A01A730000/game.tsv`
- `nxnative/dm/games/0100C2A01A730000/game.tsv; nxnative/dm/gfx/tools/shader_split.py:215,:233 PDRK/IDRK; nxnative/dm/tools/sdk_patches.py 311deab1f062b504f3d646a49ddcd4e8; nxnative/dm/host/hle_audren.c t_in_open.`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `3c737e2d67d57d03ced6019f17f6e47d` |
| `subsdk0` | `2f9e7f4ec017bb00e214af7c2fd70f4b` |
| `sdk` | `311deab1f062b504f3d646a49ddcd4e8` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

This profile covers the main game, program index 0. The optional second program launched by ExecuteProgram(1) is outside this assessment. Use the on-screen or physical controller; this game reads mouse input rather than handheld touch.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
