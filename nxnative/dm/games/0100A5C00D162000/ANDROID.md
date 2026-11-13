# Cuphead on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

The source audit covers **1.3.7 (`v655360`)**, title ID `0100A5C00D162000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

## Compatibility evidence

Reuse local v655360 and mem_used 0x5400000. Unity 2017 Shader format, ZCull nonzero storage and main/UnityFS splitting already exist. New upstream jump-table fix addresses a later canonical-register regression: local jump_offsets checks x-register spelling and predates that regression, so missing null_base is not itself a blocker.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100A5C00D162000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100A5C00D162000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/0100A5C00D162000/game.tsv`

## Build and defaults

| Module | Build ID |
|---|---|
| `main` | `d02cd50295124a48968488416e7e692b` |
| `subsdk0` | `66ce9a2e7c75fc6d0a7fe1b5d6d5baee` |
| `sdk` | `5afd9bdf1a579f10081d2ba1c4f9c7ac` |

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
