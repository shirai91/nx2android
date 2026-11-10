# AAC speaker-order probe

Android decoder PCM passes through in its existing order by default.
`DM_MOVIE_AAC_MPEG_ORDER=1` enables an experimental conversion to the MPEG
element order observed from AudioToolbox. Unset, empty, `0`, and other values
leave it disabled. The setting is read when each converter is created. To try it
for a profile, add an `env` row (tab-separated) to that profile's `android.tsv`:

```tsv
env	DM_MOVIE_AAC_MPEG_ORDER=1
```

No profile enables it in this change. The mapping and tone fixtures were adapted
from `dm/host/movie_aac_android.inc` and `dm/android/tests/movie_audio_layouts.py`
in the local nxnative `codex/cross-platform-runtime` branch, commit
`2e6a61e3bcda9b994b7f934548f1f7b962e069ac`; this is not a claim that those paths
are available on nxnative's public main branch. Apple/Android decoder comparisons
do **not** establish the real Switch SDK's PCM order. The movie HLE's note about
stagefright/software OMX also does not prove an MPEG-order contract. A hardware
capture or authoritative SDK evidence is still needed before enabling this by
default or selecting it for a game.

`python -m unittest tests.test_aac_layout` compiles the actual converter setup and
pending-PCM writer, then verifies default/disabled passthrough and explicit opt-in,
known AAC layouts, unchanged mono/stereo/PCE/unknown layouts, decoder downmix
fallback, appended PCM chunks and bounded ASC parsing.

For a real decoder check, with FFmpeg, NDK r27 and an ARM64 Android device or
emulator available:

```sh
python probes/aac_layout.py --ndk <NDK> --out <scratch> --serial <adb-serial>
```

Use `--adb <path>` and `--ffmpeg <path>` if these tools are not on PATH. The probe
builds the production AudioConverter/MediaCodec adapter, generates one distinct
tone per speaker, and decodes it with the switch unset, `0`, and `1`. Unset and
`0` must produce identical PCM. Each mode is compared with an FFmpeg reference
in the corresponding Android or experimental MPEG order: each channel must
reach 35 dB SNR and the full PCM lengths must match.
Layouts: mono, stereo, 3.0, 4.0, 5.0, 5.1 and the observed configuration-7
7.1-wide Android mask. Configuration and mask both matter: channel count alone
does not identify PCE layouts or a decoder downmix.

No game files or APK installation are needed. The uniquely named device scratch
directory is removed on completion or failure; host fixtures remain in `--out`
for inspection. This checks decoded PCM ordering, not the physical speaker/audio
route, Switch ordering, performance, or complete movie playback timing. EOS and
pending-PCM code are exercised but not changed by the optional conversion.
