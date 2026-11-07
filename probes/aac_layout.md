# AAC speaker-order probe

`python -m unittest tests.test_aac_layout` compiles the actual pending-PCM writer
and verifies known AAC layouts, unchanged mono/stereo/PCE/unknown layouts,
decoder downmix fallback, appended PCM chunks and bounded ASC parsing.

For a real decoder check, with FFmpeg, NDK r27 and an ARM64 Android device or
emulator available:

```sh
python probes/aac_layout.py --ndk <NDK> --out <scratch> --serial <adb-serial>
```

Use `--adb <path>` and `--ffmpeg <path>` if these tools are not on PATH. The probe
builds the production AudioConverter/MediaCodec adapter, generates one distinct
tone per speaker, decodes it on Android, and compares guest-order PCM with an
FFmpeg reference. Each channel must reach 35 dB SNR and the full PCM lengths must
match. Layouts: mono, stereo, 3.0, 4.0, 5.0, 5.1 and the observed configuration-7
7.1-wide Android mask. Configuration and mask both matter: channel count alone
does not identify PCE layouts or a decoder downmix.

No game files or APK installation are needed. The uniquely named device scratch
directory is removed on completion or failure; host fixtures remain in `--out`
for inspection. This checks decoded PCM ordering, not the physical speaker/audio
route, performance, or complete movie playback timing. EOS and pending-PCM code
are exercised but not changed by the channel-order fix.
