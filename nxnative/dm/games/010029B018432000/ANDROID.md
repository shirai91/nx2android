# Ghost Trick: Phantom Detective on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.0.0 (`v0`)**, title ID `010029B018432000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Upstream profile is compatible data: RE Engine KPKA/SDF shaders, CPU-written texture tracking and heap memory keys already exist locally.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010029B018432000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010029B018432000/game.tsv). The local profile is authored from that build metadata, with the shader input tables consumed by this fork.

Relevant existing paths:

- `nxnative/dm/gfx/tools/shader_split.py:353`
- `nxnative/dm/android/gfx_vk_tex.c:388`
- `nxnative/dm/games/game.py:114`
- `porter/pipeline.py:441`
- `nxnative/dm/android/tools/shader_vk.py:109`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `9f52de8549d91cfe840e6d1094f5e3b4` |
| `subsdk0` | `7e9f31f59e001cf5d9b546ffb233f480` |
| `sdk` | `2bf3519216f5b9d3e8ce26863c2e98b6` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

Upstream records shader bindless-loss diagnostics that still need Android review. Native Vulkan export avoids the Metal-specific image-atomic translation failures, but this has not been verified with the game on Android.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
