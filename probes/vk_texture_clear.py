#!/usr/bin/env python3
"""Build/run a ROM-free ARM64 Android Vulkan regression probe.

Compiles the production pipeline, slice draw and ClearTexture dispatcher into a
small readback harness. Compressed images are also sampled on the GPU to
validate decoded colors independently of the block encoder. Resource allocation and submission use a single queue;
the full game's image cache and frame loop are not exercised by this probe.

The default strictly verifies GPU-decoded BC and ASTC colors. If a device fails
ASTC sampling, --astc-seed-control tests a fixed reference block at offset zero
in an aligned 16x16 image without calling the production encoder or clear path.
--astc-bytes-only explicitly skips decoded ASTC color checks while retaining
all ASTC raw-block and untouched mip/layer checks; BC sampling stays enabled.
The Android llvmpipe emulator has failed the independent ASTC seed control even
though it advertises ASTC_LDR support. Do not treat bytes-only results as proof
of correct ASTC sampling on that device.
"""
import argparse
from pathlib import Path
import platform
import struct
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial', help='run on this adb device; omit to build only')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mode', choices=('both', 'legacy', 'dynamic'), default='both')
    parser.add_argument('--only', choices=('all', 'compressed'), default='all')
    parser.add_argument('--astc-bytes-only', action='store_true', help='skip GPU-decoded ASTC checks explicitly; still check raw ASTC copies and preservation')
    parser.add_argument('--astc-seed-control', action='store_true', help='sample an independent hard-coded ASTC reference upload without production encoder/clear calls')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    tag = {'Darwin': 'darwin-x86_64', 'Linux': 'linux-x86_64',
           'Windows': 'windows-x86_64'}[platform.system()]
    suffix = '.exe' if platform.system() == 'Windows' else ''
    compiler = args.ndk / 'toolchains/llvm/prebuilt' / tag / 'bin' / (
        'aarch64-linux-android30-clang' + ('.cmd' if suffix else ''))
    glslc = args.ndk / 'shader-tools' / tag / ('glslc' + suffix)
    android = ROOT / 'nxnative/dm/android'
    source = (android / 'gfx_vk_exec.c').read_text()
    pipeline = source[source.index('typedef struct { uint32_t ncol, cfmt[8], dfmt, samples, mask, type; } mckey;'):
                      source.index('static VkPipeline mc_pipeline(uint32_t mask)')]
    clear = source[source.index('static int clear_tex_slice('):
                   source.index('// ---------------- the record walker')]
    types = (ROOT / 'tests/vk_texture_clear_fixture.c').read_text().split('static vkimg_t image;')[0]
    fixture = (ROOT / 'probes/vk_texture_clear.c').read_text()
    shaders = [(android / 'shaders' / name) for name in
               ('maskclear.vert', 'maskclear.frag', 'maskclear_uint.frag', 'maskclear_sint.frag')]
    shaders += [ROOT / 'probes' / name for name in ('clear_sample_array.comp', 'clear_sample_volume.comp')]
    for shader_path in shaders:
        shader = shader_path.name
        stem = shader.replace('.', '_') + '_spv'
        binary = args.out / (shader + '.spv')
        subprocess.run([str(glslc), str(shader_path), '-o', str(binary)], check=True)
        data = binary.read_bytes()
        words = struct.unpack('<' + 'I' * (len(data) // 4), data)
        (args.out / (shader.replace('.', '_') + '.spv.h')).write_text(
            'static const uint32_t ' + stem + '[] = {' + ','.join(hex(w) for w in words) + '};\n')
    generated = args.out / 'clear.c'
    generated.write_text(fixture.replace('/* TYPES */', types).replace('/* PIPELINE */', pipeline)
                         .replace('/* CLEAR */', clear))
    exe = args.out / 'vk_texture_clear'
    subprocess.run([str(compiler), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-missing-field-initializers', '-Wno-unused-function',
                    '-I', str(args.out), '-I', str(ROOT / 'nxnative/dm/gfx/nvn'), '-I', str(android),
                    str(generated), '-lvulkan', '-lm', '-o', str(exe)], check=True)
    print('Built', exe)
    if args.serial:
        adb = [args.adb, '-s', args.serial]
        remote = '/data/local/tmp/nx2android-clear-' + uuid.uuid4().hex
        try:
            subprocess.run(adb + ['push', str(exe), remote], check=True)
            subprocess.run(adb + ['shell', 'chmod', '700', remote], check=True)
            modes = ('legacy', 'dynamic') if args.mode == 'both' else (args.mode,)
            for mode in modes:
                subprocess.run(adb + ['shell', remote, mode, args.only,
                                      'astc-bytes-only' if args.astc_bytes_only else 'all-samples',
                                      'astc-seed-control' if args.astc_seed_control else 'clears'], check=True)
        finally:
            subprocess.run(adb + ['shell', 'rm', '-f', remote], check=True)


if __name__ == '__main__':
    main()
