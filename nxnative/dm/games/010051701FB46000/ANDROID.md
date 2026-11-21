# Monster Train 2 on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.1.1 (`v131072`)**, title ID `010051701FB46000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Reuse exact local v131072 profile. Existing node-area SDK cut, Unity 2022 Shader/ComputeShader known trees and splitter, FreeThreadCount, movie slots and EFAULT read preparation satisfy documented needs. Native Vulkan geometry is supported on capable devices; built-in unused VR geometry failure on Apple does not force Android tool changes.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010051701FB46000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010051701FB46000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/010051701FB46000/game.tsv`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `b337cdea2be300e638a0dfb74e9115a9` |
| `subsdk0` | `8a72a4dcbfd375d2952288284259da94` |
| `sdk` | `6075151627016eef437c1312f2e3ed56` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
