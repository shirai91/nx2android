# Hyrule Warriors: Age of Calamity on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.3.0 (`v327680`)**, title ID `01002B00111A2000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Reuse local 1.3.0 profile including its sound heap CUTS and FRAME_STEP. Its memory, byte-register table, RDB/G2S, texture-upload and integer vertex requirements are already represented in the local source. Android sets `DM_FPS_TARGET=30` through the existing frame-step override in `nxnative/dm/host/loader.c`; the local patch lacks the later cutscene-counter stubs needed for correct timing at 60. Do not replace its local patches.py with newer upstream matched rules.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/01002B00111A2000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/01002B00111A2000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/01002B00111A2000/game.tsv`
- `nxnative/dm/games/01002B00111A2000/game.tsv; patches.py:1; nxnative/dm/gfx/tools/shader_split.py:255 idrk_shaders; nxnative/dm/tools/patch_nso.py:74,:913; nxnative/dm/android/gfx_vk_exec.c:1592 raster-discard writes.`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `c3cf52bf2b05d7310884971b50bcccb8` |
| `subsdk0` | `3ab92c3444d4ece2f52417b5d8ee86eb` |
| `sdk` | `95832210b36121e88b4f9b7b66239dad` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

Keep the default 30 fps cap. Upstream documents that this build's cutscenes run at double speed with voices out of sync at 60 until two additional counter stubs are present. This profile uses the existing 30 fps path; a 60 fps override is outside its compatibility assessment. The cap is a configuration choice, not a measured Android performance result.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
