# AVC matrix/range regression

`movie_avc.c` previously guessed BT.709 at 720 lines and above, BT.601 otherwise,
and always treated decoded luma as limited range. A 64-line BT.709 stream therefore
got the BT.601 matrix; a full-range movie got incorrect contrast. The adapter now
reads `color-standard` / `color-range` and retains them with each queued picture.
It accepts the decoder's standard only when the SPS VUI declares a colour
description with a specified matrix, and its range only when the SPS VUI declares
a video signal type. A signalled `matrix_coefficients=2` remains unspecified and
retains the height heuristic, independently of a declared range.
An absent, malformed or unsupported SPS retains the height heuristic and limited
range. A bounded SPS reader follows H.264 sections 7.3.2.1.1 and E.1.1, including
emulation-prevention bytes, scaling lists, POC modes and optional VUI fields.
The conversion and per-picture metadata handling adapt `dm/host/movie_avc_android.inc`
from the public `thehobbit85/nxnative` fork at
`1f01e28c87da5233dfa14fb31fabf1535df33013`. Git credits that source to seedds.
It uses the MIT license with `Copyright (c) 2026 seedds`, already retained in
`nxnative/LICENSE`. The adaptation uses nx2android's existing picture queue and
timestamp handling. [Source](https://github.com/thehobbit85/nxnative/blob/1f01e28c87da5233dfa14fb31fabf1535df33013/dm/host/movie_avc_android.inc)
and [license](https://github.com/thehobbit85/nxnative/blob/1f01e28c87da5233dfa14fb31fabf1535df33013/LICENSE).

Known standards are Android 1 (BT.709), 2/4 (BT.601), and 6 (BT.2020 non-constant
luminance). Missing or unknown standards retain the old height heuristic; only
range 1 means full, with missing/unknown ranges remaining limited. RGBA padding
is opaque black; NV12 padding uses Y=0 for full or Y=16 for limited, with UV=128.
This is 8-bit Y'CbCr conversion, not HDR tone mapping or display-gamut conversion.

The existing `[movie] AVC: output` diagnostic includes `color-standard` (the
reported Android value, or 0 when absent), `full-range` (1 for full, otherwise
0), and the `declared` mask of usable SPS declarations. Range contributes 1,
and a specified color matrix contributes 2. A value of 0 identifies the height
heuristic and limited range even when `color-standard` reports a decoder default.
Capture this line on the target device when checking untagged streams:
some decoders supply size-based defaults, and the codec is initially configured
at 1920x1080 before reporting the actual picture dimensions. The synthetic
tagged-stream checks below do not establish those defaults for a physical device.

The maintainer reported a Lenovo Legion Y700 (TB322FC, SM8750P) check with
`c2.qti.avc.decoder` on 13 Sentinels: Aegis Rim's 1024x576 intro movie.
The decoder reported `color-standard 1 full-range 0` for this untagged stream.
The earlier revision trusted that BT.709 default. With SPS declaration gating,
the same untagged stream retains the sub-720p BT.601 heuristic.
The game used raw NV12 output (`output 4 mode 1`), so the check found no
regression while leaving the RGBA conversion unexercised. A physical-device
check using RGBA texture output with an SD or untagged stream remains pending.
[Maintainer's device report](https://github.com/shirai91/nx2android/pull/174#issuecomment-6788771842)

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
driver behavior. The targeted Goldfish workaround remains separate work.
`--codec default --ranges full` can expose that limitation.

The guest raw-NV12 metadata interface is unchanged; this fix covers CPU RGBA
conversion and padding, not propagation of color metadata through the guest API.
Existing vendor-layout handling, EOS/seek behavior and buffering are unchanged.
