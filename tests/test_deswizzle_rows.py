"""nvn_deswizzle / nvn_swizzle's row copier against the copy it replaced, byte for byte, on random data.

The reference below is move() as it was before the row copier: one nvn_bl_offset per 16-byte segment. Both run on the same
random storage and random linear rows for many shapes (1 to 16 bytes per block, BC and ASTC, odd widths with a byte tail,
automatic, packaged and 3D block heights and depths, arrays, cubes, sparse tiled levels and mip tails, padded pitches),
and both directions must leave every byte of storage and of the linear buffer identical.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

import host_cc

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / 'nxnative/dm/gfx/tex'
NVN = ROOT / 'nxnative/dm/gfx/nvn'

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_deswizzle.h"

// move() before the row copier: an address per 16-byte segment
static int old_move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, int to_linear) {
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    bl += base; uint32_t lb = nvn_tex_level_gobs(d, level) * 64;
    if (lb < row_bytes) lb = row_bytes;
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t *row = lin + ((size_t)z * rows + y) * pitch;
            for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint8_t *p = bl + nvn_bl_offset(x, y, z, lb, rows, th, td);
                if (to_linear) memcpy(row + x, p, n); else memcpy(p, row + x, n);
            }
        }
    return 0;
}

static uint64_t seed;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (uint32_t)(seed >> 11); }
#define FAIL(...) do { fprintf(stderr, __VA_ARGS__); fputc(10, stderr); exit(1); } while (0)

// 1, 2, 3, 4, 6, 8, 12, 16 bytes per texel, BC1 / BC3 (8 / 16-byte 4x4 blocks), ASTC 4x4 / 6x6 / 10x8
static const int fmts[] = { 0x01, 0x0D, 0x19, 0x25, 0x1D, 0x29, 0x22, 0x2E, 0x42, 0x44, 0x79, 0x7D, 0x83 };
static const int targets[] = { 0, 1, 1, 1, 2, 2, 3, 4, 8, 9 };
static unsigned seen_th, seen_td, seen_tail, seen_sparse, seen_pkg, cases;

static void one(int n) {
    nvn_texdesc d = {0};
    d.target = targets[rnd() % 10]; d.format = fmts[rnd() % 13];
    int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh); assert(bpb);
    d.width = 1 + rnd() % (rnd() % 4 ? 300 : 1100); d.height = 1 + rnd() % (rnd() % 4 ? 200 : 700);
    d.depth = d.target == 2 ? 1 + rnd() % 40 : d.target == 4 ? 1 + rnd() % 4 : d.target == 9 ? 12 : 1;
    if (d.target == 3) d.height = 1 + rnd() % 4;
    if (d.target == 8 || d.target == 9) d.height = d.width;
    d.levels = 1 + rnd() % 5;
    unsigned k = rnd() % 6;
    if (k == 1) { d.pkg_block_h = 1 + rnd() % 6 + (d.target == 2 ? (rnd() % 4) * 16 : 0); seen_pkg++; }   // packaged block height / depth
    if (k == 3) d.flags = 0x20;                                                                              // sparse
    if (k == 4) d.flags = 0x200;                                                                             // MINIMAL_LAYOUT
    if (k == 5 && rnd() % 2) { d.flags = 0x20; d.width = 512 + rnd() % 1500; d.height = 256 + rnd() % 900; } // sparse, wide enough for tiled levels
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    assert(L.size);
    size_t guard = 256, nst = L.size + guard;
    uint8_t *st0 = malloc(nst), *st1 = malloc(nst); assert(st0 && st1);
    for (size_t i = 0; i < nst; i++) st0[i] = (uint8_t)rnd();
    for (unsigned layer = 0; layer < L.layers; layer += 1 + L.layers / 3) for (unsigned l = 0; l < d.levels; l++) {
        uint32_t rb, rows, sl, gbpb; uint64_t base; unsigned th, td;
        if (nvn_deswizzle_geom(&d, l, layer, &rb, &rows, &sl, &base, &th, &td, &gbpb)) FAIL("case %d: no geometry", n);
        seen_th |= 1u << th; seen_td |= 1u << td; if (rb % 16) seen_tail++; if (nvn_tex_level_gobs(&d, l) * 64 > (rb + 63) / 64 * 64) seen_sparse++;
        size_t pitch = rb + (rnd() % 3) * (1 + rnd() % 40), nlin = pitch * rows * sl + guard;
        uint8_t *a = malloc(nlin), *b = malloc(nlin), *src = malloc(nlin); assert(a && b && src);
        memset(a, 0x5A, nlin); memset(b, 0x5A, nlin);
        int ra = old_move(&d, l, layer, st0, a, pitch, 1), rb_ = nvn_deswizzle(&d, l, layer, st0, b, pitch);
        if (ra || rb_ || memcmp(a, b, nlin))
            FAIL("case %d: deswizzle differs: target %d %ux%ux%u fmt %#x flags %#x pkg %#x level %u layer %u (row %u bytes, %u rows, tileH %u tileD %u, pitch %zu)",
                 n, d.target, d.width, d.height, d.depth, d.format, d.flags, d.pkg_block_h, l, layer, rb, rows, th, td, pitch);
        for (size_t i = 0; i < nlin; i++) src[i] = (uint8_t)rnd();
        memcpy(st1, st0, nst);
        ra = old_move(&d, l, layer, st0, src, pitch, 0); rb_ = nvn_swizzle(&d, l, layer, src, pitch, st1);
        if (ra || rb_ || memcmp(st0, st1, nst))
            FAIL("case %d: swizzle differs: target %d %ux%ux%u fmt %#x flags %#x pkg %#x level %u layer %u (row %u bytes, tileH %u tileD %u)",
                 n, d.target, d.width, d.height, d.depth, d.format, d.flags, d.pkg_block_h, l, layer, rb, th, td);
        if (pitch > rb && nvn_deswizzle(&d, l, layer, st0, b, rb - 1) != -1) FAIL("case %d: short pitch accepted", n);
        free(a); free(b); free(src); cases++;
    }
    free(st0); free(st1);
}

int main(int argc, char **argv) {
    assert(argc == 3); seed = strtoull(argv[1], 0, 10) * 0x9E3779B97F4A7C15ull + 1; int count = atoi(argv[2]);
    for (int n = 0; n < count; n++) one(n);
    printf("levels %u: tileH %#x tileD %#x tail %u padded %u packaged %u\n", cases, seen_th, seen_td, seen_tail, seen_sparse, seen_pkg);
    // the shapes must reach the parts of the copier that differ: byte tails, padded rows, every block height and several depths
    return !(seen_tail && seen_sparse && seen_pkg && __builtin_popcount(seen_th) >= 5 && __builtin_popcount(seen_td) >= 4);
}
'''


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class DeswizzleRows(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a-deswizzle-rows-')
        cls.addClassCleanup(cls.tmp.cleanup)
        c = Path(cls.tmp.name) / 'harness.c'
        c.write_text(HARNESS, encoding='utf-8')
        cls.binary = c.with_suffix('.exe' if os.name == 'nt' else '')
        sanitize = ['-fsanitize=undefined', '-fno-sanitize-recover=all'] if host_cc.ubsan() else []
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Werror', *sanitize, '-I', str(TEX), '-I', str(NVN),
                        str(c), str(TEX / 'nvn_deswizzle.c'), str(NVN / 'nvn_tex.c'), '-o', str(cls.binary)],
                       check=True, capture_output=True, text=True)

    def test_row_copier_matches_segment_copier(self):
        for seed in (1, 2, 3):
            with self.subTest(seed=seed):
                r = subprocess.run([str(self.binary), str(seed), '400'], capture_output=True, text=True, timeout=600)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
