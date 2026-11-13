# Existing Android AVC frame layout port

Source: `seedds/nxnative` at `1f01e28c87da5233dfa14fb31fabf1535df33013`,
`dm/host/movie_avc_android.inc` (`copy_picture` and odd chroma strides) and
`dm/host/movie_mediacodec.h` (output buffer bounds/config exclusion).
The crop/padding oracle and synthetic H.264 stream come from
`t_android_avc_metadata.c`, `t_android_avc.c`, and `h264_synthetic.h` there.

The port validates I420/NV12 dimensions, strides, crop origins and the last plane
byte actually read. It keeps odd visible dimensions and ceil-sized chroma planes.
The existing Qualcomm Venus NV12 format is explicitly retained. An unspecified
slice height falls back to the picture height; invalid reported geometry is
rejected rather than silently substituted. Codec buffer offsets and lengths are
validated before pointer formation, and codec-config output is not a picture.

Existing queue/timestamp logic and color conversion remain. This does not duplicate
PR #174's per-picture color metadata or matrix/range changes. It does not implement
new decoding algorithms, formats, timestamp policy, or EOS handling.

A temporary combination with PR #174 also passed the real MediaCodec probe.
There is one textual conflict in `read_format`: retain this port's raw crop
validation inputs/defaults and #174's expanded color-metadata log. All other
hunks merge automatically; no color-metadata code is copied into this PR.

Host regression tests:

```sh
NX2A_TEST_SANITIZE=1 python -m unittest discover -s tests -p 'test_avc_*.py'
```

These cover the donor's padded 6x6 image with a visible 3x3 crop, exact last-byte
and one-byte-truncated buffers, I420 odd strides/plane heights, NV12/Venus,
invalid origins/geometry/formats, format defaults, guarded NV12/RGBA destinations,
and decoder output bounds/config buffers with unconditional release.

An optional real MediaCodec probe checks all six synthetic frame colors, planes,
output padding and guards, before and after flush, in NV12 and RGBA modes. Build
it after the regular Vulkan host build (to provide the generated profile):

```sh
python tests/run_avc_layout_device.py --ndk "$ANDROID_NDK_HOME" --host-build /tmp/avc-host --out /tmp/avc-layout-device
adb -s "$TEST_DEVICE" push /tmp/avc-layout-device /data/local/tmp/avc-layout-device
adb -s "$TEST_DEVICE" shell 'chmod 755 /data/local/tmp/avc-layout-device; /data/local/tmp/avc-layout-device'
```

The fixture uses nx2android's AVCC configuration interface, one coded sequence per
flush, and identifies frames by their distinct colors. The donor's B-frame PTS
order assertion fails on unchanged main too: nx2android's synthetic timestamp
policy changes this behavior. Timing/order is intentionally not qualified here.
The plane/padding probe passed on the task's Android 15 ARM64 emulator using
`c2.goldfish.h264.decoder`; physical Qualcomm behavior remains unqualified.
