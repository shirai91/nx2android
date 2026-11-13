# ANIMAL WELL on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.0.10 (`v655360`)**, title ID `010020D01AD24000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Existing identical local profile; embedded BNSH shaders, empty RomFS, no game-specific host/rewriter fixes.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010020D01AD24000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010020D01AD24000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/010020D01AD24000/game.tsv:23`
- `nxnative/dm/gfx/tools/shader_split.py:1492`
- `nxnative/dm/games/010020D01AD24000/game.tsv`
- `porter/pipeline.py:441`
- `nxnative/dm/android/tools/shader_vk.py:109`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `6a6ec42ffb98642a11f17a0d5e23c702` |
| `sdk` | `112ea664a8df622911758ef8abc94802` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
