# 13 Sentinels: Aegis Rim (Japan) on Android

Status: **experimental — requires testing on an Android device**. No game was built, launched or played for this change.

## Game version

The source audit covers **1.00 (`v0`)**, title ID `010045C014650000`, on nx2android `00f41ec9c0f4`. It found an existing runtime/tool path for this build; it does not establish that the game boots, renders correctly or completes. Use an arm64 Android device with BC texture support and the Vulkan features needed by its shaders. Other game versions are outside this assessment.

No game version is verified on Android.

| Module | Build ID |
|---|---|
| `main` | `932cabe1982fe80a2dbf7c49e09bce0a` |
| `subsdk0` | `9fafcfc111ed558a5234bd724befef48` |
| `sdk` | `0617f669301f922a811886048dd7b41a` |

## Frame rate

Not measured on Android. No frame-rate or pacing claim is available without a device run.

## Tested devices

None. No APK build, Android launch, gameplay session or device screenshot is available for this profile.

## Options

Android starts docked, with the gamepad visible. The package name includes the title ID so regional versions install separately. No `tested` value or Android frame-rate claim is supplied.

Use the exact version above and the player's own dump and keys. An arm64 Android device with BC texture support and the Vulkan features required by the shaders is needed. No driver or settings combination has been validated on Android; these are initial profile defaults.

## Quirks and known issues

Booting, rendering, audio, movies, controls, save/reload, frame pacing and long-session stability are unverified. Source compatibility is not a device result.

## Source compatibility evidence

Exact new Japanese profile uses existing GLSLC-output extraction and alloc_big_align; upstream explicitly needs no new host or SDK cut.

The game and build are supported by nxnative; see its [game notes](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010045C014650000/README.md) and [profile](https://github.com/seedds/nxnative/blob/3084f7a205bcb3a9cb74cf26180dabcc01c7abca/dm/games/010045C014650000/game.tsv). The local profile is authored from that build metadata, with the shader input tables consumed by this fork.

Relevant existing paths:

- `nxnative/dm/host/hle_os.c:139`
- `nxnative/dm/gfx/tools/shader_split.py:180`
- `porter/pipeline.py:441`
- `nxnative/dm/android/tools/shader_vk.py:109`

## Required device testing

1. Port the exact version above using the player's own dump and keys; confirm extraction, rewriting and shader translation finish without missing inputs.
2. Test a fresh save through the menus and into gameplay, including rendering, audio, movies and the intended controls.
3. Save, quit, restart and load; exercise later scenes and check for crashes, missing shaders, incorrect graphics and pacing problems.
4. Record the device, driver, game version and tested route before raising the status or adding a `tested` row.
5. Capture a 1280x720 gameplay frame on the device without an overlay, and record the nx2android revision or release used. Only after a real run should the tested-device details, `tested`, `readme_image` and `readme_tag` be added and the generated game lists updated.
