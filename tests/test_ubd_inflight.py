"""In-flight uniform updates (gfx_vk_core.c vk_ubd_land / vk_ubd_read): an update that lands over exactly the range
of one still in flight replaces it in the list. A reader must get the same bytes as with every update kept."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void VKLOG(const char *f, ...) { (void)f; }
typedef struct { uint64_t lo, hi; const uint8_t *p; uint8_t *own; } ubd_t;
static ubd_t g_ubd[64]; static int g_nubd;
static uint64_t g_done, g_fence, g_ubd_racy; static int g_x_inflkeep;
static uint64_t vk_fence_done(void) { return g_done; }
static int vk_gpu_pending(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
@INFL@
static ubd_infl_t *g_infl; static int g_ninfl, g_capinfl;
@READ@
// every landed update, never dropped: what a reader saw before
static ubd_infl_t *g_all; static int g_nall, g_capall;
static void ref_read(uint64_t a, uint64_t n, uint8_t *dst) {
    memcpy(dst, (const void *)(uintptr_t)a, n);
    for (int i = 0; i < g_nall; i++) { if (g_all[i].fence <= g_done) continue; uint64_t lo = g_all[i].lo > a ? g_all[i].lo : a, hi = g_all[i].hi < a + n ? g_all[i].hi : a + n;
        if (lo < hi) memcpy(dst + (lo - a), g_all[i].data + (lo - g_all[i].lo), hi - lo); }
    for (int i = 0; i < g_nubd; i++) { uint64_t lo = g_ubd[i].lo > a ? g_ubd[i].lo : a, hi = g_ubd[i].hi < a + n ? g_ubd[i].hi : a + n;
        if (lo < hi) memcpy(dst + (lo - a), g_ubd[i].p + (lo - g_ubd[i].lo), hi - lo); }
}
static void land(void) {   // vk_ubd_land's list part
    for (int i = 0; i < g_nubd; i++) {
        if (g_nall == g_capall) { g_capall = g_capall ? 2 * g_capall : 256; g_all = realloc(g_all, sizeof *g_all * (size_t)g_capall); }
        g_all[g_nall++] = (ubd_infl_t){g_ubd[i].lo, g_ubd[i].hi, g_fence + 1, g_ubd[i].p, NULL};
        if (g_ninfl == g_capinfl) { g_capinfl = g_capinfl ? 2 * g_capinfl : 256; g_infl = realloc(g_infl, sizeof *g_infl * (size_t)g_capinfl); }
        @DEDUPE@
        g_infl[g_ninfl++] = (ubd_infl_t){g_ubd[i].lo, g_ubd[i].hi, g_fence + 1, g_ubd[i].p, NULL};
    }
    g_nubd = 0;
}
static uint32_t rs = 12345; static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
int main(void) {
    enum { MEM = 1024 }; static uint8_t mem[MEM]; uint64_t base = (uint64_t)(uintptr_t)mem; int most = 0, reads = 0;
    // a few ranges that recur (the game rewrites the same parts of a block), nested and partly overlapping ones among them
    static const struct { uint32_t o, n; } R[] = {{0, 64}, {0, 64}, {0, 64}, {16, 32}, {48, 64}, {0, 256}, {512, 16}, {200, 100}, {960, 64}};
    for (int step = 0; step < 200000; step++) {
        uint32_t op = rnd() % 16;
        if (op < 7) {   // an update: one of the same range goes first, as vk_sub_update does
            uint32_t r = rnd() % (sizeof R / sizeof R[0]); uint64_t lo = base + R[r].o, hi = lo + R[r].n;
            for (int i = g_nubd - 1; i >= 0; i--) if (g_ubd[i].lo == lo && g_ubd[i].hi == hi) { memmove(&g_ubd[i], &g_ubd[i + 1], sizeof *g_ubd * (size_t)(g_nubd - i - 1)); g_nubd--; break; }
            if (g_nubd == 64) land();
            uint8_t *p = malloc(R[r].n); for (uint32_t k = 0; k < R[r].n; k++) p[k] = (uint8_t)rnd();   // (never freed: the lists keep pointing at it)
            g_ubd[g_nubd++] = (ubd_t){lo, hi, p, NULL};
        } else if (op < 10) land();
        else if (op == 10) g_fence++;                                   // the submission ends: later landings carry the next fence
        else if (op == 11 && g_done < g_fence) g_done++;                // one more submission completes
        else if (op == 12) { for (int k = 0; k < 8; k++) mem[rnd() % MEM] = (uint8_t)rnd(); }   // the game writes memory
        else { uint64_t o = rnd() % MEM, n = 1 + rnd() % (MEM - o); uint8_t a[MEM], b[MEM];
            vk_ubd_read(base + o, n, a); ref_read(base + o, n, b); reads++;
            if (memcmp(a, b, n)) { printf("step %d: a read of +%llu %llu differs (%d in flight, %d kept by the reference)\n", step, (unsigned long long)o, (unsigned long long)n, g_ninfl, g_nall); return 1; } }
        if (g_ninfl > most) most = g_ninfl;
    }
    // the list holds one entry a range at most, however many landed
    if (most > (int)(sizeof R / sizeof R[0]) || g_nall < 1000 || reads < 1000) { printf("most in flight %d, landed %d, reads %d\n", most, g_nall, reads); return 1; }
    return 0;
}
'''


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class UbdInflight(unittest.TestCase):
    def test_same_range_replaces_the_one_in_flight(self):
        src = (ROOT / 'nxnative/dm/android/gfx_vk_core.c').read_text(encoding='utf-8')
        a = src.index('typedef struct { uint64_t lo, hi, fence; const uint8_t *data; uint8_t *own; } ubd_infl_t;'); infl = src[a:src.index('\n', a)]
        a = src.index('void vk_ubd_read(uint64_t a, uint64_t n, uint8_t *dst) {'); read = 'static ' + src[a:src.index('\n}\n', a) + 3]
        a = src.index('for (int k = g_x_inflkeep ? -1 : g_ninfl - 1; k >= 0; k--) if (g_infl[k].lo == g_ubd[i].lo'); dedupe = src[a:src.index('\n', a)]
        code = HARNESS.replace('@INFL@', infl).replace('@READ@', read).replace('@DEDUPE@', dedupe)
        with tempfile.TemporaryDirectory(prefix='nx2a_ubd_inflight_') as d:
            c = Path(d) / 't.c'; c.write_text(code, encoding='utf-8')
            exe = Path(d) / ('t.exe' if shutil.which('cc').lower().endswith('.exe') else 't')
            subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-o', str(exe), str(c)], check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
