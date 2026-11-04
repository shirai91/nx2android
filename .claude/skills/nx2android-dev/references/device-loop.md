# Testing on the device

The test device is a Lenovo Legion Y700 (TB322FC, Snapdragon 8 Elite, Adreno 830, Android 16) over wireless adb; its serial and the default package are in `devtools/device.json`. **Ask the user before installing anything**, say the package and the data size, and uninstall test apps afterwards.

## Build, install, launch

```
export MSYS_NO_PATHCONV=1
eval "$(NX2A_PACKAGE=org.nx2a.<short>.test python devtools/device.py --sh)"   # ADB (with -s), SERIAL, PKG, F (the app's files folder)
python -m porter port "<game folder>" --debug --install --package $PKG --device $SERIAL
```

- `--debug`: debuggable, debug info, and DM_CTL (a release build ignores it).
- The installer starts the app once before pushing (Android gives the files folder the app's group only when the app creates it; a shell-made folder leaves pushed files unreadable: "FATAL open …/mods/main.fixups.tsv"), pushes only changed files, and stages paths over 260 characters through short hard links.
- Each install **rewrites `env.txt`**: add debug lines after installing.

```
$ADB shell "am force-stop $PKG; echo DM_CTL=$F/ctl.txt >> $F/env.txt; : > $F/ctl.txt; am start -n $PKG/android.app.NativeActivity"
```

## Read what happened

- Log: `$F/log.txt` (stderr of the host; the previous run is `log.prev.txt`). Start with `grep -a '\[RED\]' log.txt` and the last lines before a crash; `[perf]` lines give the frame rate.
- Screenshot: `$ADB shell screencap -p /data/local/tmp/s.png` then `$ADB pull` it (and delete it on the device). Single screenshots miss flicker: record with `devtools/rec_flicker.py`, or use `devtools/burst.py` for per-frame stats.
- `adb logcat` for Android-side problems (input routing, activity lifecycle).

## Drive the game (DM_CTL)

Append lines to `$F/ctl.txt` (read from the end of the file, every 100 ms):

| Line | Does |
|---|---|
| `A~0.4`, `L+R~0.4`, `Up~0.3`, `Plus~0.4`, `LUp~0.8` | Press buttons / stick for the hold time. 0.2 s worked for NEO, but Viractal missed short presses: 0.4 s is the safe default. `adb shell input keyevent` presses are too short for the game. |
| `drag x0 y0 x1 y1 [s]` | One-finger drag in the game's touch screen (fractions) |
| `env KEY=VALUE` | Set a switch live (`DM_VK_TRACE_PASS=-2` lists passes, `=N` traces one; `DM_VK_PASSDUMP=1`; `DM_VK_SHOTS=N` shoots the next N presents; `DM_FPS_PIN=40`) |
| `shot <path>`, `passdump <n>`, `texdump <path%d>` | Frame captures |

DM_CTL presses go to the game's pad, not to the front end: to test the overlay, pause menu or text entry use `adb shell input tap x y` (screen pixels; enough for the front end, which acts when the finger lifts, while a held touch for the game itself needs `input swipe x y x y 150`), `input text …` (keyboard letters) and `input keyevent KEYCODE_DPAD_* / KEYCODE_BUTTON_*` (controller keys; Nintendo layout by position: `BUTTON_B` = A, `BUTTON_A` = B, `BUTTON_X` = Y, `START` = +, `SELECT` = −).

Before concluding a game is soft-locked, read the screen: Viractal's "Hey! Not that way!" was the game rejecting a wrong choice.

For finding what is wrong once it runs (validation layer, LRZ flicker, frame shots, pass dumps, performance and soak checks, the device scripts that boot a game to gameplay), use the switch-android-port skill.

## Helpers in devtools/

`device.py` (ADB / PKG / FILES from device.json or `NX2A_DEVICE` / `NX2A_PACKAGE`), `push_chunked.py` (resumable big pushes), `rec_flicker.py`, `burst.py`, `soak.sh <minutes>` (moves without pressing A, so a save's story does not advance), `trace_pass.sh <pass>`, `dmsv_extract.py` / `spv_support_use.py` (inspect the shader bundle, either version; `nxnative/dm/android/tools/dmsv.py info` shows its version and sizes, `dmsv.py pack <in> <out> [--level N]` writes it again compressed or, with `--level 0`, uncompressed), `nso_image.py`, `ppm2png.py`.

## Finish

Remove test apps (`$ADB uninstall $PKG`) and check `pm list packages | grep nx2a` shows only what the user had before. Note what was tested (device, version, date, scenes) in the profile's `android.tsv` header.
