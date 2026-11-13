# Brain Memory Training Academy on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.0.0 (`v0`)**, title ID `0100F5D01DA3A000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Reuse exact local v0 profile. Known Unity 2022 Shader tree and compute layouts, existing SDK node-area build cut, 1 GB pool and automatic font behavior satisfy documented dependencies; default 2D controller layout.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100F5D01DA3A000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100F5D01DA3A000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/0100F5D01DA3A000/game.tsv`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `b0a32fd69b176ec50000000000000000` |
| `subsdk0` | `091bc184412bfb0ec5e395eafa4f7dd1` |
| `sdk` | `12f161039300861f1054371f0e36b8c9` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
