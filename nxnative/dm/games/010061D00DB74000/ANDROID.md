# Ori and the Blind Forest: Definitive Edition on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.0.2 (`v131072`)**, title ID `010061D00DB74000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Exact profile uses supported UnityFS shaders, existing SDK 8.2.0 fixed cuts and normal virtual-memory layout; no game-specific patch required.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010061D00DB74000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010061D00DB74000/game.tsv). The local profile is authored from that build metadata, with the shader input tables consumed by this fork.

Relevant existing paths:

- `nxnative/dm/tools/sdk_patches.py:203`
- `nxnative/dm/gfx/tools/unityfs.py:173`
- `porter/pipeline.py:441`
- `nxnative/dm/android/tools/shader_vk.py:109`
- `nxnative/dm/android/gfx_vk_shader.c:144`
- `nxnative/dm/android/gfx_vk_exec.c:1039`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `9c4f2a47916b6a5478548adc62a3a87c` |
| `subsdk0` | `8df876f80ce4ceee62360b31405c452f` |
| `sdk` | `3ae34e689d7a35f13cbf42bfd672af8b` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

The first loading screen prewarms shaders and may take several minutes with this fork's present pacing. The upstream notes record completion without the newer interval-0 optimization. Later areas and the Unity stream-buffer behavior remain untested on Android.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
