# Taiko Risshiden V DX on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

## Game version

The source audit covers **1.1.1 (`v131072`)**, title ID `0100346017304000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

No game version is verified on Android.

| Module | Build ID |
|---|---|
| `main` | `f5ab74b9d45f955de2dc91da8e039c57` |
| `subsdk0` | `434729719bfacada3bd0ed4842a10e55` |
| `sdk` | `dfa12b7019fffbe725fa4b4f1335988c` |

## Frame rate

Not measured on Android. No frame-rate or pacing claim is available without a device run.

## Tested devices

None. No APK build, Android launch, gameplay session or device screenshot is available for this profile.

## Options

Android starts handheld, with the gamepad hidden. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

Use the exact version above and the player's own dump and keys. An arm64 Android device with BC texture support and the Vulkan features required by the shaders is needed. No driver or settings combination has been validated on Android; these are initial profile defaults.

## Quirks and known issues

The profile retains handheld touch input and starts with the gamepad hidden. The registered 1.1.1 limiter cut uses upstream's corrected bytes: N=1 follows present pacing, while N=2 battle scenes retain their own wait. Only the registered build is covered; function-matched patches for other versions are not imported. Japanese and Chinese are the game's supported languages.

Booting, rendering, audio, movies, controls, save/reload, frame pacing and long-session stability are unverified. Source compatibility is not a device result.

## Source compatibility evidence

Reuse local v131072 profile and game-specific limiter CUTS. For this exact build, upstream improved CUTS replacement is supported by the existing loader (CUTS bytes assert), while MATCH_CUTS is not needed for this registered version. Current HID gesture/overclock services exist. Handheld touch layout is supported.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100346017304000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/0100346017304000/game.tsv). The existing local game profile and shader tables are retained.

Relevant existing paths:

- `nxnative/dm/games/0100346017304000/game.tsv`

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
5. Capture a 1280x720 gameplay frame on the device without an overlay, and record the nx2android revision or release used. Only after a real run should the tested-device details, `tested`, `readme_image` and `readme_tag` be added and the generated game lists updated.
