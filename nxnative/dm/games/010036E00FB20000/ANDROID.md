# The Great Ace Attorney Chronicles on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.0.0 (`v0`)**, title ID `010036E00FB20000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Reuse local base 1.0.0 profile. MT Framework SPK-in-ARC shaders, dirty/flush texture uploads and SDK 10 movie module are supported. No game patch import or new key is needed.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010036E00FB20000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010036E00FB20000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/010036E00FB20000/game.tsv`
- `nxnative/dm/games/010036E00FB20000/game.tsv; nxnative/dm/gfx/tools/shader_split.py:459 mtspk_shaders,:480 mtarc_shaders; nxnative/dm/android/gfx_vk_tex.c:388 dirty-upload handling; nxnative/dm/tools/sdk_patches.py d9add76fc264863d85fbbd1d70076c76.`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `1da748fc9499882f24dfca9f583c5574` |
| `subsdk0` | `84e50460493a12f26100e5150fbff271` |
| `sdk` | `d9add76fc264863d85fbbd1d70076c76` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
