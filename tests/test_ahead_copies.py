"""Replay ahead's scratch copies of guest memory are brought up to date at the game's submit (gfx_vk_core.c copies_refresh, hc_write), through the production C.

A submission replayed ahead (DM_NVN_AHEAD) copied a uniform block into scratch memory before the game's submit; Breath of the Wild writes that block on
the CPU before it submits. At the submit the bytes that changed go into every copy of the range, except the bytes a deferred or in-flight update covered
in that copy (the command stream's). No device or game is needed.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_cc   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def function(s, signature):
    i = s.index(signature); j = s.index('{', i) + 1; depth = 1
    while depth:
        depth += (s[j] == '{') - (s[j] == '}'); j += 1
    return s[i:j] + '\n'


@unittest.skipUnless(shutil.which('cc') and host_cc.posix(), 'a POSIX host C compiler is required')
class AheadCopies(unittest.TestCase):
    def test_changed_bytes_reach_each_copy_but_its_overlays(self):
        core = (ROOT / 'nxnative/dm/android/gfx_vk_core.c').read_text(encoding='utf-8')
        i = core.index('enum { HC_RANGES')
        state = core[i:core.index('static void reads_reset(void) {', i)]
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
''' + state + function(core, 'static void hc_write(') + function(core, 'static void copies_refresh(') + r'''
int main(void) {
    static uint8_t mem[256], dst1[256], dst2[256]; static uint32_t ov[2] = {16, 32};
    for (int i = 0; i < 256; i++) mem[i] = (uint8_t)i;
    memcpy(dst1, mem, 256); memset(dst1 + 16, 0xAA, 16); memcpy(dst2, mem, 256);   /* copy 1: a deferred update over [16, 32) */
    uint8_t *snap = malloc(256); memcpy(snap, mem, 256);
    g_hcr[0] = (hc_range){(uint64_t)(uintptr_t)mem, 256, snap}; g_nhcr = 1;
    static hc_copy copies[2]; g_hcc = copies; g_nhcc = 2; g_hco = ov;
    copies[0] = (hc_copy){dst1, 0, 0, 1}; copies[1] = (hc_copy){dst2, 0, 0, 0};
    copies_refresh(); assert(g_hc_changed == 0 && g_hc_patched == 0 && g_hc_n == 0);   /* nothing written since */
    memset(mem + 8, 0x55, 32); mem[200] = 7;   /* the game writes [8, 40) and one byte at 200 before its submit */
    copies_refresh();
    for (int i = 0; i < 256; i++) {
        uint8_t want2 = mem[i], want1 = i >= 16 && i < 32 ? 0xAA : mem[i];
        if (dst1[i] != want1 || dst2[i] != want2) { printf("byte %d: %02x %02x, want %02x %02x\n", i, dst1[i], dst2[i], want1, want2); return 1; }
    }
    printf("%llu %llu %llu\n", (unsigned long long)g_hc_changed, (unsigned long long)g_hc_patched, (unsigned long long)g_hc_n);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_ahead_copies_') as d:
            c = Path(d) / 't.c'; exe = Path(d) / 't'
            c.write_text(code, encoding='utf-8')
            build = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable', str(c), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            changed, patched, subs = (int(v) for v in run.stdout.split())
            self.assertEqual(changed, 33)              # [8, 40) and byte 200
            self.assertEqual(patched, 33 + 33 - 16)    # copy 2 all of it; copy 1 all but its update's [16, 32)
            self.assertEqual(subs, 1)


if __name__ == '__main__':
    unittest.main()
