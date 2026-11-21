#!/usr/bin/env python3
"""ROM-free MediaCodec color regression; compare actual RGBA output with FFmpeg."""
import argparse
from pathlib import Path
import platform
import re
import struct
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
COLORS = [(128, 70, 190), (16, 128, 128), (235, 128, 128),
          (0, 128, 128), (255, 128, 128), (128, 200, 60)]
MATRICES = [('smpte170m', 'bt601', 6, 6, 6), ('bt709', 'bt709', 1, 1, 1), ('bt2020nc', 'bt2020', 9, 14, 9)]


def packets(stream):
    starts = list(re.finditer(b'\x00\x00(?:\x00)?\x01', stream))
    groups, current = [], bytearray()
    for i, match in enumerate(starts):
        end = starts[i+1].start() if i+1 < len(starts) else len(stream)
        nal = stream[match.end():end]
        if nal and nal[0] & 31 == 9 and current:
            groups.append(bytes(current)); current.clear()
        current.extend(b'\x00\x00\x00\x01' + nal)
    if current:
        groups.append(bytes(current))
    if len(groups) != len(COLORS):
        raise AssertionError(f'expected {len(COLORS)} access units, got {len(groups)}')
    return groups


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--serial', required=True)
    p.add_argument('--adb', default='adb')
    p.add_argument('--ffmpeg', default='ffmpeg')
    p.add_argument('--codec', default='c2.android.avc.decoder', help='codec name, or default for device selection')
    p.add_argument('--ranges', nargs='+', choices=('limited', 'full'), default=['limited', 'full'])
    a = p.parse_args(); a.out.mkdir(parents=True, exist_ok=True)
    tag = {'Darwin': 'darwin-x86_64', 'Linux': 'linux-x86_64', 'Windows': 'windows-x86_64'}[platform.system()]
    compiler = a.ndk / 'toolchains/llvm/prebuilt' / tag / 'bin' / ('aarch64-linux-android30-clang' + ('.cmd' if platform.system() == 'Windows' else ''))
    (a.out / 'dm.h').write_text('#include <stdio.h>\n#include <stdlib.h>\n#include <stdbool.h>\n'
                              '#define LOG(...) (fprintf(stderr,__VA_ARGS__),fputc(10,stderr))\n'
                              '#define die(...) (fprintf(stderr,__VA_ARGS__),abort())\n')
    binary = a.out / 'avc_color_decode'
    subprocess.run([str(compiler), '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(a.out),
                    '-I', str(ROOT / 'nxnative/dm/host'), str(ROOT / 'probes/avc_color_decode.c'),
                    '-lmediandk', '-o', str(binary)], check=True)
    adb = [a.adb, '-s', a.serial]; remote = '/data/local/tmp/nx2a-avc-' + uuid.uuid4().hex
    subprocess.run(adb + ['shell', 'mkdir', remote], check=True)
    try:
        subprocess.run(adb + ['push', str(binary), remote + '/decode'], check=True)
        subprocess.run(adb + ['shell', 'chmod', '700', remote + '/decode'], check=True)
        w, h = 64, 64
        yuv = b''.join(bytes([y])*(w*h) + bytes([u])*(w*h//4) + bytes([v])*(w*h//4) for y,u,v in COLORS)
        ff = [a.ffmpeg, '-nostdin', '-loglevel', 'error', '-y']
        for matrix, scale_matrix, primaries, transfer, coefficients in MATRICES:
            for color_range in a.ranges:
                stem = a.out / f'{matrix}-{color_range}'
                stream, reference, actual, feed = (stem.with_suffix(s) for s in ('.h264', '.ref.rgba', '.got.rgba', '.packets'))
                full = color_range == 'full'
                subprocess.run([*ff, '-f', 'rawvideo', '-pixel_format', 'yuv420p', '-video_size', f'{w}x{h}',
                                '-framerate', '25', '-i', 'pipe:0', '-c:v', 'libx264', '-preset', 'veryfast',
                                '-crf', '10', '-x264-params', 'aud=1:bframes=0:keyint=30', '-colorspace', matrix,
                                '-color_primaries', 'bt2020' if matrix == 'bt2020nc' else matrix,
                                '-color_trc', 'bt2020-10' if matrix == 'bt2020nc' else matrix,
                                '-color_range', 'pc' if full else 'tv', '-bsf:v',
                                f'h264_metadata=colour_primaries={primaries}:transfer_characteristics={transfer}:'
                                f'matrix_coefficients={coefficients}:video_full_range_flag={int(full)}',
                                '-f', 'h264', str(stream)], input=yuv, check=True)
                subprocess.run([*ff, '-i', str(stream), '-vf',
                                f'scale=in_color_matrix={scale_matrix}:in_range={"full" if full else "limited"}:out_range=full',
                                '-pix_fmt', 'rgba', '-f', 'rawvideo', str(reference)], check=True)
                units = packets(stream.read_bytes())
                feed.write_bytes(struct.pack('>4I', 0x41564350, w, h, len(units)) +
                                 b''.join(struct.pack('>I', len(unit)) + unit for unit in units))
                subprocess.run(adb + ['push', str(feed), remote + '/input.packets'], check=True)
                subprocess.run(adb + ['shell', remote + '/decode', remote + '/input.packets', remote + '/output.rgba', a.codec], check=True)
                subprocess.run(adb + ['pull', remote + '/output.rgba', str(actual)], check=True)
                expected, got = reference.read_bytes(), actual.read_bytes()
                if len(expected) != len(got) or len(got) != len(COLORS)*w*h*4:
                    raise AssertionError(f'{stem.name}: unexpected frame count or output length')
                worst = max(abs(x-y) for x,y in zip(expected,got))
                print(f'{stem.name}: {len(COLORS)} frames, max RGBA error {worst}/255 ({a.codec})', flush=True)
                if worst > 3:
                    raise AssertionError(f'{stem.name}: colors differ from FFmpeg by {worst}/255')
    finally:
        subprocess.run(adb + ['shell', 'rm', '-rf', remote], check=True)


if __name__ == '__main__':
    main()
