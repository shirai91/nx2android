# ROMANCE OF THE THREE KINGDOMS XIV: Diplomacy and Strategy Expansion Pack on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.0.10 (`v458752`)**, title ID `0100ED7012DB2000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Import data-only profile 1.0.10. Its memory keys are standard, no game cuts/run-time keys; SDK 10.4.1 is the existing Great Ace Attorney build. Same Age of Calamity IDRK/G2S reader handles shader archives. Upstream deferred-release drain fix is Metal-specific; Android Vulkan uses its own lifetime path.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100ED7012DB2000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100ED7012DB2000/game.tsv). The local profile is authored from that build metadata, with the shader input tables consumed by this fork.

Relevant existing paths:

- `nxnative/dm/gfx/tools/shader_split.py:215 idrk_entries,:255 idrk_shaders,:1507 IDRK dispatch; nxnative/dm/tools/sdk_patches.py d9add76fc264863d85fbbd1d70076c76; nxnative/dm/games/game.py accepts imported profile header.`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `1a971cc40b6d5b3f7cc3317856d822fe` |
| `subsdk0` | `84e50460493a12f26100e5150fbff271` |
| `sdk` | `d9add76fc264863d85fbbd1d70076c76` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
