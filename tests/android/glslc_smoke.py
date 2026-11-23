#!/usr/bin/env python3
"""Exercise Android GLSLC capture transport/publication using the production writer and synthetic output, without game files."""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from porter import pipeline
from porter.adb import Adb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--adb', type=Path, required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if not args.serial.startswith('emulator-'): parser.error('use a dedicated ARM64 emulator')
    args.out.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='glslc-smoke-', dir=args.out))
    source = (ROOT / 'nxnative/dm/host/hle_glslc.c').read_text()
    capture = source[source.index('static void capture('):source.index('// The GPU code version')]
    c = root / 'fixture.c'
    c.write_text('''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\\n', stderr); } while (0)
''' + capture + '''
int main(void) {
    uint8_t status[16] = {0}, output[0x48] = {0};
    uint32_t magic = 0x19866891, length = sizeof output;
    memcpy(output, &magic, 4); memcpy(output + 0x44, &length, 4);
    status[12] = 1;
    const uint8_t *results[] = {status, output};
    const uint8_t **object = results;
    assert(getenv("DM_SAVE") && getenv("DM_TEMP") && !access("mods", F_OK));
    capture(&object);
    // Exercise the real device-side deadline after emitting a complete output.
    sleep(3600);
    return 0;
}
''')
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    cc = args.ndk / 'toolchains/llvm/prebuilt' / host / 'bin/clang'
    fixture = root / 'fixture'
    subprocess.run([str(cc), '--target=aarch64-linux-android31', '-Wall', '-Werror', str(c), '-o', str(fixture)], check=True)
    data = bytearray(0x48); struct.pack_into('<I', data, 0, 0x19866891); struct.pack_into('<I', data, 0x44, len(data))
    digest = 0xcbf29ce484222325
    for byte in data: digest = ((digest ^ byte) * 0x100000001b3) & ((1 << 64) - 1)
    name = f'{digest:016x}.glslc'
    table = root / 'shader_map.tsv'; table.write_text(f'x\t0\tvertex\tkey\t/glslc_capture/{name}#GLSLC@0\n')
    p = object.__new__(pipeline.Port)
    p.work = root / 'work'; p.aw = p.work / 'android'; p.logs = p.work / 'logs'; p.logs.mkdir(parents=True)
    p.game = root / 'extract/v0'; (p.game / 'RomFS').mkdir(parents=True)
    p.patched = p.aw / 'patched_v0'; p.patched.mkdir(parents=True)
    p.pkg_ver = 'v0'; p.versions = ['v0']; p.dirs = {'v0': p.game}
    p.g = SimpleNamespace(id='0100000000010000', table=lambda _: table)
    p.o = pipeline.Options(folder=root, device=args.serial); p.env = dict(os.environ); p.cfg = {}
    p.tools = {'adb': args.adb, 'ndk': args.ndk}; p.app_name = 'Synthetic GLSLC fixture'; p.GLSLC_SECS = 1
    remote_dirs = []
    class TrackedAdb(Adb):
        def push_file(self, src, dst, **kw):
            if dst.endswith('/dmhost'): remote_dirs.append(dst.rsplit('/', 1)[0])
            return super().push_file(src, dst, **kw)
    def fixture_build(command, *unused, **kw):
        assert command[command.index('--gfx') + 1] == 'null'
        target = Path(command[command.index('--out') + 1]); target.mkdir(parents=True, exist_ok=True)
        shutil.copy2(fixture, target / 'dmhost')
    with patch.object(pipeline.nsp, 'run', side_effect=fixture_build), patch.object(pipeline, 'Adb', TrackedAdb):
        p.device_glslc()
    assert (p.game / 'glslc_capture' / name).read_bytes() == data
    assert 'saved a run-time compile output' in (p.logs / 'android_glslc.log').read_text()
    adb = Adb(args.adb, args.serial)
    assert len(remote_dirs) == 1
    assert adb.shell(f'test ! -e {remote_dirs[0]} && echo clean').strip() == 'clean'
    # Reuse must not need a connected device or rebuild the tool.
    p.tools.pop('adb')
    with patch.object(pipeline.nsp, 'run', side_effect=AssertionError('unexpected rebuild')): p.device_glslc()
    print(f'PASS: native writer, device deadline, adb pull, capture publication/reuse and remote cleanup; evidence: {root}')


if __name__ == '__main__': main()
