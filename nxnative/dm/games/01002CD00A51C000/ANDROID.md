# Baba Is You on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.0.12 (`v786432`)**, title ID `01002CD00A51C000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

All shaders are whole multi-stage GLSLC programs in Assets.dat. Existing glslc_program/glslc_section readers support these; no game cuts, virtual-memory allocator or runtime GLSLC capture required. Preserve mem_used 0x6000000.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/01002CD00A51C000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/01002CD00A51C000/game.tsv). The local profile is authored from that build metadata, with the shader input tables consumed by this fork.

Relevant existing paths:

- `nxnative/dm/gfx/tools/shader_split.py`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `4e1e4c7bc925853c998b8af07726c87d` |
| `sdk` | `a65883c2863f42f93e0f43f2d7aba0e8` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
