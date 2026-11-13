# Android AVC frame layout probe

Source: [`thehobbit85/nxnative` at `1f01e28c87da5233dfa14fb31fabf1535df33013`](https://github.com/thehobbit85/nxnative/tree/1f01e28c87da5233dfa14fb31fabf1535df33013),
`dm/host/movie_avc_android.inc` (`copy_picture` and odd chroma strides) and
`dm/host/movie_mediacodec.h` (output buffer bounds/config exclusion).
The crop/padding oracle and synthetic H.264 stream come from
`t_android_avc_metadata.c`, `t_android_avc.c`, and `h264_synthetic.h` there.
The donor is the fork; this does not attribute that revision to `seedds/nxnative`.

The port validates I420/NV12 dimensions, aligned crop origins and the last plane
byte actually read. It keeps odd visible dimensions and ceil-sized chroma planes.
Format 19 is I420; all other formats retain nx2android's NV12 interpretation.
Unknown formats log once, independently of dropped-picture diagnostics; format
21 and Qualcomm Venus (`0x7fa30c04`) are known NV12 formats.
Missing or undersized stride/slice height falls back to the picture dimensions.
Missing, inverted or out-of-frame crop metadata falls back to the full frame.
Nonpositive dimensions, oversized layouts, unaligned crop origins and short
buffers are still rejected, with one diagnostic per decoder giving byte size,
stride, slice height and crop. Codec buffer offsets and lengths are validated
before pointer formation, and codec-config output is not a picture.

Existing queue/timestamp logic and color conversion remain. This does not implement
new decoding algorithms, formats, timestamp policy, or EOS handling. Per-picture
color metadata and matrix/range conversion are separate from the layout copy.

Host regression tests:

```sh
NX2A_TEST_SANITIZE=1 python -m unittest discover -s tests -p 'test_avc_*.py'
```

These cover the donor's padded 6x6 image with a visible 3x3 crop, exact last-byte
and one-byte-truncated buffers, I420 odd strides/plane heights, NV12/Venus and
unknown-format fallback, invalid geometry, format/crop/stride defaults, once-only
diagnostics, guarded NV12/RGBA destinations, and decoder output bounds/config
buffers with unconditional release. The extraction includes production SPS
helpers when present, so color-metadata changes can use the same layout fixture.

The optional MediaCodec probe checks all six synthetic frame colors, planes,
output padding and guards, before and after flush, in NV12 and RGBA modes. Build
it after the regular Vulkan host build (to provide the generated profile):

```sh
python probes/run_avc_layout_device.py --ndk "$ANDROID_NDK_HOME" --host-build /tmp/avc-host --out /tmp/avc-layout-device
adb -s "$TEST_DEVICE" push /tmp/avc-layout-device /data/local/tmp/avc-layout-device
adb -s "$TEST_DEVICE" shell 'chmod 755 /data/local/tmp/avc-layout-device; /data/local/tmp/avc-layout-device'
```

The build script supports macOS, Linux and Windows NDK host packages; Windows
uses `windows-x86_64/bin/clang.exe`. It builds only and never contacts a device.
Use appropriate local output paths and environment-variable syntax in Windows
shells. Device execution in the separate `adb` commands is a manual step.

The fixture uses nx2android's AVCC configuration interface, one coded sequence per
flush, and identifies frames by their distinct colors. It does not qualify
B-frame timestamp ordering, physical decoder behavior or commercial gameplay.
Host checks and emulator results do not replace a physical Android device run.
