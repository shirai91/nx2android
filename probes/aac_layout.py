#!/usr/bin/env python3
"""ROM-free AAC speaker-order regression using FFmpeg and an ARM64 Android device.

Adapts nxnative's movie_audio_layouts.py tone fixtures. Builds the real nx2android
AudioConverter adapter; checks default Android order and optional MPEG order.
These host decoder comparisons do not establish the Switch SDK's output order.
"""
from array import array
import argparse
import math
from pathlib import Path
import platform
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    1: ('mono', [443], [0]),
    2: ('stereo', [443, 709], [0, 1]),
    3: ('3.0', [443, 709, 997], [2, 0, 1]),
    4: ('4.0', [443, 709, 997, 1207], [2, 0, 1, 3]),
    5: ('5.0', [443, 709, 997, 1301, 1607], [2, 0, 1, 3, 4]),
    6: ('5.1', [443, 709, 997, 65, 1301, 1607], [2, 0, 1, 4, 5, 3]),
    8: ('7.1', [443, 709, 997, 65, 1301, 1607, 1867, 2099], [2, 0, 1, 6, 7, 4, 5, 3]),
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--serial', required=True)
    p.add_argument('--adb', default='adb')
    p.add_argument('--ffmpeg', default='ffmpeg')
    a = p.parse_args(); a.out.mkdir(parents=True, exist_ok=True)
    tag = {'Darwin': 'darwin-x86_64', 'Linux': 'linux-x86_64', 'Windows': 'windows-x86_64'}[platform.system()]
    compiler = a.ndk / 'toolchains/llvm/prebuilt' / tag / 'bin' / ('aarch64-linux-android30-clang' + ('.cmd' if platform.system() == 'Windows' else ''))
    # Only host logging/abort are shimmed. Codec configuration, PCM queue and
    # AudioConverter calls below are the production source and real MediaCodec.
    (a.out / 'dm.h').write_text('#include <stdio.h>\n#include <stdlib.h>\n#include <stdbool.h>\n'
                              '#define LOG(...) (fprintf(stderr,__VA_ARGS__),fputc(10,stderr))\n'
                              '#define die(...) (fprintf(stderr,__VA_ARGS__),abort())\n')
    compat = ROOT / 'nxnative/dm/android/compat'; binary = a.out / 'aac_decode'
    subprocess.run([str(compiler), '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(a.out),
                    '-I', str(compat / 'include'), str(compat / 'audioconverter_mediacodec.c'),
                    str(ROOT / 'probes/aac_decode.c'), '-lmediandk', '-o', str(binary)], check=True)
    adb = [a.adb, '-s', a.serial]; remote = '/data/local/tmp/nx2a-aac-' + uuid.uuid4().hex
    subprocess.run(adb + ['shell', 'mkdir', remote], check=True)
    try:
        subprocess.run(adb + ['push', str(binary), remote + '/decode'], check=True)
        subprocess.run(adb + ['shell', 'chmod', '700', remote + '/decode'], check=True)
        for ch, (layout, tones, order) in CASES.items():
            rate = 44100 if ch == 1 else 48000
            samples = array('h', (round(10000 * math.sin(i*f*2*math.pi/rate)) for i in range(32768) for f in tones))
            stream, ref = (a.out / f'{name}-{ch}.{ext}' for name, ext in (('tones','aac'),('ref','pcm')))
            ff = [a.ffmpeg, '-nostdin', '-loglevel', 'error', '-y']
            subprocess.run([*ff, '-f', 's16le', '-ar', str(rate), '-ac', str(ch), '-channel_layout', layout,
                            '-i', 'pipe:0', '-c:a', 'aac', '-b:a', str(ch*48000), '-f', 'adts', str(stream)], input=samples.tobytes(), check=True)
            subprocess.run([*ff, '-i', str(stream), '-f', 's16le', str(ref)], check=True)
            subprocess.run(adb + ['push', str(stream), remote + '/input.aac'], check=True)
            baseline = None
            for mode in ('unset', '0', '1'):
                env = ['env', '-u', 'DM_MOVIE_AAC_MPEG_ORDER'] if mode == 'unset' else ['env', 'DM_MOVIE_AAC_MPEG_ORDER=' + mode]
                got = a.out / f'android-{ch}-{mode}.pcm'
                subprocess.run(adb + ['shell', *env, remote + '/decode', remote + '/input.aac', remote + '/output.pcm'], check=True)
                subprocess.run(adb + ['pull', remote + '/output.pcm', str(got)], check=True)
                expected, actual = array('h', ref.read_bytes()), array('h', got.read_bytes())
                if len(expected) != len(actual): raise AssertionError(f'{ch} channels: PCM lengths differ ({len(expected)} vs {len(actual)})')
                if mode == 'unset': baseline = actual
                if mode == '0' and actual != baseline: raise AssertionError(f'{ch} channels: disabled output differs from default')
                # FFmpeg's configuration-7 interpretation exchanges the front
                # pairs relative to the observed Android 7.1-wide output.
                native_order = [6, 7, 2, 3, 4, 5, 0, 1] if ch == 8 else range(ch)
                scores = []
                for i, speaker in enumerate(order if mode == '1' else native_order):
                    signal, decoded = expected[speaker::ch], actual[i::ch]
                    error = sum((x-y)**2 for x, y in zip(signal, decoded)); power = sum(x*x for x in signal)
                    scores.append(10*math.log10(power/error) if error else math.inf)
                print(f'{ch} channels, MPEG order {mode}: {len(actual)//ch} frames, minimum speaker SNR {min(scores):.1f} dB', flush=True)
                if min(scores) < 35: raise AssertionError(f'{ch} channels ({mode}): wrong speaker order or decoded PCM')
    finally:
        subprocess.run(adb + ['shell', 'rm', '-rf', remote], check=True)


if __name__ == '__main__':
    main()
