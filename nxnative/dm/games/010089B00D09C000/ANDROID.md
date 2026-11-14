# AI: THE SOMNIUM FILES on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.0.1 (`v65536`)**, title ID `010089B00D09C000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Reuse local 1.0.1. Unity 2017 shaders borrow existing bundle layouts; FMOD AudioOut, SDK 6 H.264 drain and dirty/flush movie planes use existing runtime. Three geometry stages unsupported by old Metal can translate as native Vulkan geometry stages. No own patch needed.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010089B00D09C000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010089B00D09C000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/010089B00D09C000/game.tsv`
- `nxnative/dm/games/010089B00D09C000/game.tsv; nxnative/dm/android/tools/shader_vk.py:5 native Vulkan geometry; nxnative/dm/gfx/tools/shader_split.py:1299 unity_shaders; nxnative/dm/host/hle_movie.c.`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `53153920243843459f88ace4c34cee46` |
| `subsdk0` | `16577266bb9797abf6029f85025f6d35` |
| `sdk` | `184751379007f9c50fc96ba4d26a5dca` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
