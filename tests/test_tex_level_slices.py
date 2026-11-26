"""The level-and-layer slices of a block-linear texture's storage (hle_nvn.c level_slice, which WriteTexels prepares; gfx_vk_tex.c pkg_slice, which a partial
upload checks for writes) against the swizzler: every byte a level of a layer swizzles lies in its slice, the slices do not overlap, and both copies agree."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / 'nxnative/dm/gfx/tex'
NVN = ROOT / 'nxnative/dm/gfx/nvn'
VK = ROOT / 'nxnative/dm/android'


def function(text, signature):
    at = text.index(signature)
    return text[at:text.index('\n}', at) + 2] + '\n'


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_deswizzle.h"

typedef struct { nvn_texdesc d; nvn_texlayout L; uint8_t *mem; } tex_t;   // the fields the two slice functions read
@LEVEL_SLICE@
@PKG_SLICE@
static uint32_t seed;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
static uint32_t pick(uint32_t lo, uint32_t hi) { return lo + rnd() % (hi - lo + 1); }
#define FAIL(...) do { fprintf(stderr, __VA_ARGS__); fputc(10, stderr); exit(1); } while (0)

static const int fmts[] = { 0x01, 0x0D, 0x25, 0x29, 0x2E, 0x42 };   // R8, RG8, RGBA8, RGBA16F, RGBA32F, BC1
static unsigned seen_targets, seen_layers, seen_levels;

static void geometry(int n) {
    static const int targets[] = { 1, 2, 4, 8, 9 };   // 2D, 3D, 2D array, cube, cube array
    int target = targets[rnd() % 5], fmt = fmts[rnd() % 6];
    int bw, bh, bpb = nvn_fmt_bpb(fmt, &bw, &bh);
    uint32_t w = pick(1, 400), h = pick(1, 200), depth = 1;
    if (target == 8 || target == 9) h = w;
    if (target == 2) depth = pick(1, 24);
    if (target == 4) depth = pick(1, 40);
    if (target == 9) depth = 6 * pick(1, 4);
    unsigned maxl = 1; for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) maxl++;
    nvn_texdesc d = { target, fmt, 0, 0, w, h, depth, pick(1, maxl), 0, 0 };
    tex_t t = { d, {0}, NULL }; nvn_tex_layout(&d, &t.L);
    if (!t.L.size) return;
    seen_targets |= 1u << target; if (t.L.layers > 1) seen_layers++; if (d.levels > 1) seen_levels++;
    uint8_t *owner = calloc(t.L.size, 1), *st = malloc(t.L.size); assert(owner && st);   // owner: 1 + the slice a byte belongs to (mod 255), 0 = none
    for (unsigned layer = 0; layer < t.L.layers; layer++) for (unsigned l = 0; l < d.levels; l++) {
        uint64_t off, len, off2, len2;
        if (!level_slice(&t, l, layer, &off, &len)) FAIL("geometry %d: no slice for level %u layer %u", n, l, layer);
        if (!pkg_slice(&t, l, layer, &off2, &len2) || off2 != off || len2 != len) FAIL("geometry %d: the two slice functions differ at level %u layer %u", n, l, layer);
        if (off + len > t.L.size) FAIL("geometry %d: slice past the storage", n);
        uint8_t id = (uint8_t)(1 + (layer * d.levels + l) % 255);
        for (uint64_t i = off; i < off + len; i++) { if (owner[i]) FAIL("geometry %d: slices of level %u layer %u overlap", n, l, layer); owner[i] = id; }
        // every byte the swizzler writes for this level and layer: storage zeroed, texels all ones
        uint32_t rb, rows, sl, gbpb; uint64_t base; unsigned th, td;
        if (nvn_deswizzle_geom(&d, l, layer, &rb, &rows, &sl, &base, &th, &td, &gbpb)) FAIL("geometry %d: no geometry", n);
        if (base != off) FAIL("geometry %d: level %u layer %u starts at %#llx, its slice at %#llx", n, l, layer, (unsigned long long)base, (unsigned long long)off);
        size_t n_lin = (size_t)rb * rows * sl; uint8_t *lin = malloc(n_lin); assert(lin); memset(lin, 0xFF, n_lin);
        memset(st, 0, t.L.size);
        if (nvn_swizzle(&d, l, layer, lin, rb, st)) FAIL("geometry %d: swizzle failed", n);
        for (uint64_t i = 0; i < t.L.size; i++) if (st[i] && (i < off || i >= off + len))
            FAIL("geometry %d: %dx%dx%d target %d fmt %#x level %u layer %u writes byte %#llx outside its slice %#llx+%#llx", n, (int)w, (int)h, (int)depth, target, fmt, l, layer,
                 (unsigned long long)i, (unsigned long long)off, (unsigned long long)len);
        free(lin);
    }
    (void)bw; (void)bh; (void)bpb;
    free(owner); free(st);
}

int main(int argc, char **argv) {
    assert(argc == 3); seed = (uint32_t)strtoul(argv[1], 0, 10); int count = atoi(argv[2]);
    for (int n = 0; n < count; n++) geometry(n);
    printf("targets %#x, with layers %u, with levels %u\n", seen_targets, seen_layers, seen_levels);
    return !(seen_targets == ((1u << 1) | (1u << 2) | (1u << 4) | (1u << 8) | (1u << 9)) && seen_layers && seen_levels);
}
'''


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class TexLevelSlices(unittest.TestCase):
    def test_slices_hold_every_swizzled_byte(self):
        hle = (NVN / 'hle_nvn.c').read_text(encoding='utf-8')
        vk = (VK / 'gfx_vk_tex.c').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory(prefix='nx2a-tex-slices-') as tmp:
            c = Path(tmp) / 'harness.c'
            c.write_text(HARNESS.replace('@LEVEL_SLICE@', function(hle, 'static int level_slice('))
                                .replace('@PKG_SLICE@', function(vk, 'static int pkg_slice(')), encoding='utf-8')
            exe = c.with_suffix('.exe' if os.name == 'nt' else '')
            subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Werror', '-I', str(TEX), '-I', str(NVN),
                            str(c), str(TEX / 'nvn_deswizzle.c'), str(NVN / 'nvn_tex.c'), '-o', str(exe)], check=True, capture_output=True, text=True)
            for s in (1, 2, 3):
                r = subprocess.run([str(exe), str(s), '40'], capture_output=True, text=True, timeout=300)
                self.assertEqual(r.returncode, 0, r.stderr + r.stdout)


if __name__ == '__main__':
    unittest.main()
