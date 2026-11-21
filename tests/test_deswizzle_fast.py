"""DM_MOVIE_FAST's whole-GOB copies (nvn_deswizzle.c move, hle_nvn.c write_region_bl) against nvn_bl_offset, byte by byte, on random geometries."""
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

typedef struct { nvn_texdesc d; uint8_t *mem; } tex_t;   // the fields write_region_bl reads
typedef struct { int xoffset, yoffset, zoffset, width, height, depth; } NVNcopyRegion;
@WRITE_REGION_BL@
static uint32_t seed;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
static uint32_t pick(uint32_t lo, uint32_t hi) { return lo + rnd() % (hi - lo + 1); }
#define FAIL(...) do { fprintf(stderr, __VA_ARGS__); fputc(10, stderr); exit(1); } while (0)

static const int fmts[] = { 0x01, 0x0D, 0x25, 0x29, 0x2E };   // R8, RG8, RGBA8, RGBA16F, RGBA32F: 1 to 16 bytes per texel
static unsigned seen_th, seen_td, seen_tail, seen_whole, seen_region_fast, seen_region_slow;

// The storage nvn_bl_offset says a write leaves: every byte of the level at its own address, the rest as before
static void expect_level(uint8_t *st, const nvn_texdesc *d, unsigned l, unsigned layer, const uint8_t *lin, size_t pitch) {
    uint32_t rb, rows, sl, bpb; uint64_t base; unsigned th, td;
    assert(!nvn_deswizzle_geom(d, l, layer, &rb, &rows, &sl, &base, &th, &td, &bpb));
    uint32_t lb = nvn_tex_level_gobs(d, l) * 64; if (lb < rb) lb = rb;
    for (uint32_t z = 0; z < sl; z++) for (uint32_t y = 0; y < rows; y++) for (uint32_t x = 0; x < rb; x++)
        st[base + nvn_bl_offset(x, y, z, lb, rows, th, td)] = lin[((size_t)z * rows + y) * pitch + x];
}

static void geometry(int n) {
    int target = rnd() % 3 ? 1 : 2, fmt = fmts[rnd() % 5];   // 2D or 3D
    int bw, bh, bpb = nvn_fmt_bpb(fmt, &bw, &bh);
    uint32_t w = pick(65, 700) / (uint32_t)bpb + 1, h = pick(1, 300), depth = target == 2 ? pick(1, 40) : 1;
    if (rnd() % 4 == 0) w = 64 * pick(2, 6) / (uint32_t)bpb;   // rows of whole GOBs only
    nvn_texdesc d = { target, fmt, 0, 0, w, h, depth, pick(1, 3), 0, 0 };
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    size_t guard = 4096; uint8_t *st = malloc(L.size + guard), *want = malloc(L.size + guard); assert(st && want);
    for (size_t i = 0; i < L.size + guard; i++) st[i] = (uint8_t)rnd();
    memcpy(want, st, L.size + guard);
    for (unsigned layer = 0; layer < L.layers; layer++) for (unsigned l = 0; l < d.levels; l++) {
        uint32_t rb, rows, sl, gbpb; uint64_t base; unsigned th, td;
        if (nvn_deswizzle_geom(&d, l, layer, &rb, &rows, &sl, &base, &th, &td, &gbpb)) FAIL("geometry %d: no geometry", n);
        seen_th |= 1u << th; seen_td |= 1u << td;
        if (rb > 64 && rb % 64) seen_tail++;
        if (rb >= 64 && rb % 64 == 0) seen_whole++;
        size_t pitch = rb + pick(0, 3) * 16, n_lin = pitch * rows * sl;
        uint8_t *lin = malloc(n_lin), *back = malloc(n_lin); assert(lin && back);
        for (size_t i = 0; i < n_lin; i++) lin[i] = (uint8_t)rnd();
        if (nvn_swizzle(&d, l, layer, lin, pitch, st)) FAIL("geometry %d: swizzle failed", n);
        expect_level(want, &d, l, layer, lin, pitch);
        if (memcmp(st, want, L.size + guard))
            FAIL("geometry %d: swizzle of %dx%dx%d fmt %#x level %u layer %u (row %u bytes, %u rows, tileH %u tileD %u) differs from nvn_bl_offset",
                 n, (int)w, (int)h, (int)depth, fmt, l, layer, rb, rows, th, td);
        memset(back, 0xCD, n_lin);
        if (nvn_deswizzle(&d, l, layer, st, back, pitch)) FAIL("geometry %d: deswizzle failed", n);
        for (uint32_t z = 0; z < sl; z++) for (uint32_t y = 0; y < rows; y++) {
            size_t o = ((size_t)z * rows + y) * pitch;
            if (memcmp(back + o, lin + o, rb)) FAIL("geometry %d: deswizzle level %u layer %u row %u slice %u differs", n, l, layer, y, z);
            for (size_t x = rb; x < pitch; x++) if (back[o + x] != 0xCD) FAIL("geometry %d: deswizzle wrote past the row", n);
        }
        free(lin); free(back);
    }
    if (target == 1) {   // write_region_bl: 2D only (write_texels sends 3D textures the whole-level way)
        tex_t t = { d, st };
        for (int k = 0; k < 8; k++) {
            unsigned l = rnd() % d.levels, layer = rnd() % L.layers;
            uint32_t rb, rows, sl, gbpb; uint64_t base; unsigned th, td;
            assert(!nvn_deswizzle_geom(&d, l, layer, &rb, &rows, &sl, &base, &th, &td, &gbpb));
            uint32_t texels = rb / (uint32_t)bpb, gob = 64 / (uint32_t)bpb, x0, wt;
            if (k % 2 && texels >= gob) {   // whole GOB columns: the fast path
                uint32_t cols = texels / gob, c0 = rnd() % cols; x0 = c0 * gob; wt = pick(1, cols - c0) * gob;
            } else { x0 = rnd() % texels; wt = pick(1, texels - x0); }
            uint32_t y0 = rnd() % rows, ht = pick(1, rows - y0);
            size_t wb = (size_t)wt * bpb, row = wb + pick(0, 2) * 16;
            uint8_t *src = malloc(row * ht); assert(src);
            for (size_t i = 0; i < row * ht; i++) src[i] = (uint8_t)rnd();
            NVNcopyRegion r = { (int)x0, (int)y0, 0, (int)wt, (int)ht, 1 };
            if (write_region_bl(&t, l, layer, &r, src, row, bw, bh, bpb)) FAIL("geometry %d: write_region_bl refused", n);
            uint32_t lb = nvn_tex_level_gobs(&d, l) * 64; if (lb < rb) lb = rb;
            for (uint32_t y = 0; y < ht; y++) for (uint32_t x = 0; x < wb; x++)
                want[base + nvn_bl_offset(x0 * (uint32_t)bpb + x, y0 + y, 0, lb, rows, th, td)] = src[(size_t)y * row + x];
            if (memcmp(st, want, L.size + guard))
                FAIL("geometry %d: write_region_bl %u,%u %ux%u of %dx%d fmt %#x level %u (row %u bytes, tileH %u) differs from nvn_bl_offset",
                     n, x0, y0, wt, ht, (int)w, (int)h, fmt, l, rb, th);
            if (x0 * bpb % 64 == 0 && wb % 64 == 0) seen_region_fast++; else seen_region_slow++;
            free(src);
        }
    }
    free(st); free(want);
}

int main(int argc, char **argv) {
    assert(argc == 3); seed = (uint32_t)strtoul(argv[1], 0, 10); int count = atoi(argv[2]);
    for (int n = 0; n < count; n++) geometry(n);
    // the geometries must reach what the review asked for: rows wider than 64 bytes with a tail, whole-GOB rows, several block heights and depths
    printf("tileH %#x tileD %#x tail %u whole %u region fast %u slow %u\n", seen_th, seen_td, seen_tail, seen_whole, seen_region_fast, seen_region_slow);
    return !(seen_tail && seen_whole && __builtin_popcount(seen_th) >= 4 && __builtin_popcount(seen_td) >= 3 && seen_region_fast && seen_region_slow);
}
'''


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class DeswizzleFast(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a-deswizzle-fast-')
        cls.addClassCleanup(cls.tmp.cleanup)
        hle = (NVN / 'hle_nvn.c').read_text(encoding='utf-8')
        c = Path(cls.tmp.name) / 'harness.c'
        c.write_text(HARNESS.replace('@WRITE_REGION_BL@', function(hle, 'static int write_region_bl(')), encoding='utf-8')
        cls.binary = c.with_suffix('.exe' if os.name == 'nt' else '')
        # UBSan where the compiler links it (not the Windows runner's MinGW): the fast paths' pointer steps stay in bounds
        sanitize = ['-fsanitize=undefined', '-fno-sanitize-recover=all'] if host_cc.ubsan() else []
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Werror', *sanitize, '-I', str(TEX), '-I', str(NVN),
                        str(c), str(TEX / 'nvn_deswizzle.c'), str(NVN / 'nvn_tex.c'), '-o', str(cls.binary)],
                       check=True, capture_output=True, text=True)

    def run_harness(self, fast, seed, count):
        env = {k: v for k, v in os.environ.items() if k != 'DM_MOVIE_FAST'}
        if fast:
            env['DM_MOVIE_FAST'] = '1'   # read once per process: set before the first call
        r = subprocess.run([str(self.binary), str(seed), str(count)], capture_output=True, text=True, env=env, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_movie_fast_matches_bl_offset(self):
        for seed in (1, 2, 3, 4):
            with self.subTest(seed=seed):
                self.run_harness(True, seed, 150)

    def test_address_by_address_matches_bl_offset(self):
        self.run_harness(False, 5, 150)


if __name__ == '__main__':
    unittest.main()
