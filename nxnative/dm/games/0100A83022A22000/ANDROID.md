# Absolum on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.1.1 (`v393216`)**, title ID `0100A83022A22000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Use exact LOCAL 1.1.1 v393216. Its main-specific 40 GB to 2.5GB CUT, SDK 18 node/alignment cuts, game_env GC range, viewport swizzle, loose GLSLC reader and NVN API path already exist. Do not import upstream 1.1.2 package row/stdalloc_cap: that key and shared allocator cap do not exist locally.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100A83022A22000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100A83022A22000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/0100A83022A22000/game.tsv`
- `nxnative/dm/games/0100A83022A22000/game.tsv; patches.py:1; nxnative/dm/tools/sdk_patches.py f118ae1c2e7728471aaf1a0d58484968; nxnative/dm/host/hle_os.c:117-134 game_env; nxnative/dm/gfx/tools/shader_split.py:1559 loose GLSLC fallback.`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `916f5b8d61baa16d0939404ea047e129` |
| `subsdk2` | `ca1378cdd05a63438ef12ee7fbd5ca9c` |
| `sdk` | `f118ae1c2e7728471aaf1a0d58484968` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

This assessment covers update 1.1.1 only. The newer upstream 1.1.2 profile relies on allocator-cap support absent from this fork. Keep the 1.1.1 dump separate from newer updates when porting.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
