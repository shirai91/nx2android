# AVC matrix/range regression

`movie_avc.c` previously guessed BT.709 at 720 lines and above, BT.601 otherwise,
and always treated decoded luma as limited range. A 64-line BT.709 stream therefore
got the BT.601 matrix; a full-range movie got incorrect contrast. The adapter now
reads `color-standard` / `color-range` and retains them with each queued picture.
The conversion follows the tested nxnative `movie_avc_android.inc` path, adapted
to nx2android's existing picture queue and timestamp handling.

Known standards are Android 1 (BT.709), 2/4 (BT.601), and 6 (BT.2020 non-constant
luminance). Missing or unknown standards retain the old height heuristic; only
range 1 means full, with missing/unknown ranges remaining limited. RGBA padding
is opaque black; NV12 padding uses Y=0 for full or Y=16 for limited, with UV=128.
This is 8-bit Y'CbCr conversion, not HDR tone mapping or display-gamut conversion.

## Host regression

```sh
python -m unittest discover -s tests -p test_avc_color.py
```

The fixture compiles the real format reader, picture queue, and take/conversion
functions. It checks both I420 and NV12 inputs against an independent floating
point matrix, both ranges, clipping, absent/unknown metadata, format changes with
old frames queued, timestamp sorting and synthetic-order preservation, and black
padding. MediaCodec format access is stubbed; the pixel path is production code.

## Real Android decoder

Requires FFmpeg with libx264 and `h264_metadata`, NDK r27c, and an already-running
ARM64 Android device/emulator accessible through adb. No APK installation or game
data is involved. The probe builds the full production adapter with only logging
and optional decoder selection supplied by the fixture. Six synthetic frames per
stream are encoded with explicit SPS/VUI primaries, transfer, matrix, and range.
The actual RGBA output is compared byte-for-byte with FFmpeg, allowing 3/255 for
integer rounding. It also checks output count and timestamps. Unique device
scratch directories are removed on success or failure; host outputs remain in
`--out` for inspection.

```sh
python probes/avc_color.py --ndk /path/to/android-ndk-r27c \
  --adb /path/to/adb --serial emulator-5554 --out /tmp/avc-colors
python probes/avc_color.py --ndk /path/to/android-ndk-r27c \
  --adb /path/to/adb --serial emulator-5554 --out /tmp/avc-default \
  --codec default --ranges limited
```

API 35 ARM64 emulator results: `c2.android.avc.decoder` passed BT.601, BT.709 and
BT.2020, each in limited and full range (36 frames, maximum error 1/255).
The default `c2.goldfish.h264.decoder` passed all three limited-range cases
(18 frames, maximum error 1/255). Unchanged main fails the same real-decoder
BT.709 limited-range case at 13/255. The CPU regressions also fail on unchanged
main and pass with the fix.
Both NEO/Viractal native builds pass; no physical-device or game-playthrough
qualification is implied.

## Scope limits

The emulator's Goldfish decoder is known to rescale full-range samples while
still labeling them full. This PR retains production codec selection: the probe
defaults to AOSP's software decoder to test full-range conversion without that
driver behavior. Source-side SPS parsing and the targeted Goldfish workaround
remain separate work. `--codec default --ranges full` can expose that limitation.

The guest raw-NV12 metadata interface is unchanged; this fix covers CPU RGBA
conversion and padding, not propagation of color metadata through the guest API.
Existing vendor-layout handling, EOS/seek behavior and buffering are unchanged.
