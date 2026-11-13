# STEINS;GATE RE:BOOT on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.0.3 (`v131072`)**, title ID `01008BC024FDE000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Existing identical profile; BNSH shaders and per-output volume support already exist locally.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/01008BC024FDE000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/01008BC024FDE000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/01008BC024FDE000/game.tsv:19`
- `nxnative/dm/host/hle_audio.c:394`
- `nxnative/dm/games/01008BC024FDE000/game.tsv`
- `porter/pipeline.py:441`
- `nxnative/dm/android/tools/shader_vk.py:109`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `e9016a42a1b2bfa323bc987215c30af3` |
| `subsdk0` | `ab62fe8edd07a6a293f885d54359e559` |
| `sdk` | `0023a1b06130cd795f7bc4613772af0a` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
