"""Host C tests for batched GS-split range hazards and write-set coalescing."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gfx_vk_gs_batch.h"

int main(void) {
    // 1. Basic range overlap edge cases
    assert(!gs_ranges_overlap(0, 64, 0, 64));
    assert(!gs_ranges_overlap(0x1000, 0, 0x1000, 64));
    assert(!gs_ranges_overlap(0x1000, 64, 0x1040, 64)); // adjacent, non-overlapping
    assert(!gs_ranges_overlap(0x1040, 64, 0x1000, 64));
    assert(gs_ranges_overlap(0x1000, 64, 0x103f, 64));  // 1-byte overlap
    assert(gs_ranges_overlap(0x103f, 64, 0x1000, 64));
    assert(gs_ranges_overlap(0x1000, 256, 0x1020, 16)); // contained
    assert(gs_ranges_overlap(UINT64_MAX - 15, 16, UINT64_MAX - 7, 8)); // top of address space

    // 2. Write-set deduplication, containment, adjacency and multi-entry bridge coalescing
    gs_wr_set_t w; memset(&w, 0, sizeof w);
    assert(!gs_wr_add(&w, 0, 64));
    assert(!gs_wr_add(&w, 0x1000, 0));
    assert(!gs_wr_add(&w, UINT64_MAX - 10, 20)); // overflow guard
    assert(w.n == 0);

    assert(gs_wr_add(&w, 0x1000, 0x100));
    assert(w.n == 1);
    assert(gs_wr_add(&w, 0x1000, 0x100)); // exact duplicate
    assert(w.n == 1);
    assert(gs_wr_add(&w, 0x1020, 0x40));  // contained subset
    assert(w.n == 1);
    assert(gs_wr_add(&w, 0x2000, 0x100)); // disjoint
    assert(w.n == 2);
    assert(gs_wr_add(&w, 0x1100, 0xf00)); // bridges [0x1000,0x1100) and [0x2000,0x2100)
    assert(w.n == 1);
    assert(w.r[0].addr == 0x1000 && w.r[0].size == 0x1100);

    // 3. Overlap queries against coalesced set
    assert(!gs_wr_overlaps(&w, 0x0f00, 0x100));
    assert(gs_wr_overlaps(&w, 0x0f00, 0x101));
    assert(gs_wr_overlaps(&w, 0x1800, 0x10));
    assert(!gs_wr_overlaps(&w, 0x2100, 0x100));
    assert(!gs_wr_overlaps(&w, 0, 0x1000));
    assert(!gs_wr_overlaps(&w, 0x1000, 0));

    // 4. Capacity limit cleanly refuses once GS_WR_MAX disjoint ranges are filled
    memset(&w, 0, sizeof w);
    for (int i = 0; i < GS_WR_MAX; i++)
        assert(gs_wr_add(&w, 0x10000ull + (uint64_t)i * 0x1000ull, 0x100));
    assert(w.n == GS_WR_MAX);
    assert(!gs_wr_add(&w, 0x90000000ull, 0x100)); // full -> caller must flush
    assert(gs_wr_add(&w, 0x10000ull, 0x100));     // existing range still fits

    puts("gs batch helpers: all cases passed");
    return 0;
}
'''


@unittest.skipUnless(CC, 'host C compiler required')
class GeometrySplitBatch(unittest.TestCase):
    def test_range_hazards_and_write_set_coalescing(self):
        with tempfile.TemporaryDirectory(prefix='nx2a_gs_batch_') as tmp:
            src = Path(tmp) / 'harness.c'
            exe = Path(tmp) / 'harness'
            src.write_text(HARNESS, encoding='utf-8')
            build = subprocess.run(
                [CC, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-I', str(ROOT / 'nxnative/dm/android'), str(src), '-o', str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('all cases passed', run.stdout)


if __name__ == '__main__':
    unittest.main()
