# Patrick's Parabox on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.0.1 (`v65536`)**, title ID `0100E44016C22000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Existing identical v65536 Unity profile with matching SDK cuts and Unity 2020.3 shader layout already present.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100E44016C22000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100E44016C22000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/tools/sdk_patches.py:30`
- `nxnative/dm/gfx/tools/unityfs.py:160`
- `nxnative/dm/host/hle_sdk16.c:61`
- `nxnative/dm/games/0100E44016C22000/game.tsv`
- `porter/pipeline.py:441`
- `nxnative/dm/android/tools/shader_vk.py:109`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `86171cbde77fb44989fbc97905ffc8f4` |
| `subsdk0` | `525feff80957be306b46b74636256582` |
| `sdk` | `9c73fb73f529fedc9ffefa28246711cc` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
