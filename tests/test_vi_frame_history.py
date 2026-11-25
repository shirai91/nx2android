"""The layer's frame history (nn::vi::ListFrameHistory, hle_vi.c) and the present interval a game reads back (nvnWindowGetPresentInterval, hle_nvn.c).

Unreal Engine 5's Switch RHI takes the first shown entry of the history as its latest flip; with no entries Minecraft Dungeons II waited for ever at its first frame-end
sync. Luigi's Mansion 3 paces itself by the newest entry and the window's interval, so in display mode, once the game has asked for the history, the window answers the
interval it presents at; a game that never asks reads back the interval it set, as before the history. The harnesses cut the real code out of the core sources (csrc.py)
and drive it with stub backends: the history ring with the present queue's progress, flip times, GPU completion and the pacer's vsync grid, and the interval with the
history's own ListFrameHistory, the pacing policy's display mode, the cap and the frame step. Adapted from seedds/nxnative's t_vi_history.c and t_nvn.c (MIT).
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402
import host_cc  # noqa: E402


HISTORY_HEAD = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
static void log_noop(const char *fmt, ...) { (void)fmt; }
#define LOG(...) log_noop(__VA_ARGS__)
static uint64_t g_count, g_in, g_done, g_gns = 5000000000ull, g_hns = 9000000000ull;
static double g_pt[64];   // flip time per ticket (seconds on the now_ns clock), 0 not yet, -1 dropped
static int g_gd[64]; static uint64_t g_gde[64];   // mtl_gpu_done per GPU frame: state (-1 no record, 0 running, 1 done) and end time
uint64_t dm_nvn_frame_count(void) { return g_count; }
void mtl_present_progress(uint64_t *q, uint64_t *d) { *q = g_in; *d = g_done; }
double mtl_flip_time(uint64_t n) { return n < 64 ? g_pt[n] : 0; }
int mtl_gpu_done(uint64_t f, uint64_t *t1) { if (f >= 64) return -1; *t1 = g_gde[f]; return g_gd[f]; }
uint64_t guest_ns(void) { return g_gns; }
uint64_t now_ns(void) { return g_hns; }
'''

HISTORY_MAIN = r'''
static void present(void) { g_in++; g_count++; dm_vi_frame_queued(g_count, 0, 0, 0); dm_vi_frame_ticket(g_count, g_in); }   // no pacer grid (DM_DETCLOCK, unpaced): first-seen stays
static uint64_t g_at, g_grid;   // the present time and the pacer grid a paced present is queued with
static void ppresent(void) { g_in++; g_count++; g_done = g_in; g_pt[g_count] = 0; dm_vi_frame_queued(g_count, g_at, g_grid, 16666667ull); dm_vi_frame_ticket(g_count, g_in); }   // handed on, no flip time
static uint64_t ticks(uint64_t ns) { return ns * 12 / 625; }

int main(void) {
    uint64_t fi[10][5];
    for (int i = 0; i < 64; i++) g_gd[i] = -1;   // no GPU record until a case sets one
    assert(!dm_vi_history_asked());                                                   // no game has asked yet
    assert(h_vi_FrameHist(NULL, 0, NULL) == 8 && h_vi_FrameHist(fi, 0, NULL) == 8);   // the SDK's count query
    assert(dm_vi_history_asked());                                                    // which counts as asking (hle_nvn.c window_interval)
    assert(h_vi_FrameHist(fi, 10, NULL) == 0);                                        // nothing presented yet
    for (int i = 0; i < 10; i++) { present(); g_gns += 16666667; g_hns += 16666667; }  // frames 1-10, tickets 1-10
    g_done = 7;                                   // 1-7 handed on, 8-10 still queued
    g_pt[7] = (g_hns - 100000000ull) / 1e9;       // 7 shown 0.1 s before host now
    g_pt[6] = -1;                                 // 6 dropped
    memset(fi, 0xa5, sizeof fi);
    int n = h_vi_FrameHist(fi, 10, NULL);
    assert(n == 8);                               // the last 8: frames 10 down to 3
    for (int k = 0; k < n; k++) assert(fi[k][1] == (uint64_t)(10 - k));
    for (int k = 0; k < 3; k++) assert(fi[k][0] == 1 && fi[k][3] == 0 && fi[k][4] == 0);   // 10, 9, 8: queued
    assert(fi[3][0] == 2 && fi[3][3] == fi[3][4] && fi[3][3] == ticks(g_gns - 100000000ull));   // 7: its flip time on the guest clock
    assert(fi[4][0] == 2 && fi[4][3] == ticks(g_gns));   // 6: dropped, shown from when it was first seen handed on (Orbitals' frame-end sync waited for its flip for ever)
    assert(fi[5][0] == 2 && fi[5][3] == ticks(g_gns));   // 5: handed on, no flip time and no grid: the time it was first seen
    assert(fi[0][2] == ticks(5000000000ull + 9 * 16666667ull));   // queue tick of frame 10
    assert(fi[8][0] == 0xa5a5a5a5a5a5a5a5ull);    // nothing written past the entries returned
    g_gns += 1000000000ull;
    n = h_vi_FrameHist(fi, 10, NULL);
    assert(fi[5][3] == ticks(g_gns - 1000000000ull));   // the first-seen time stays
    assert(h_vi_FrameHist(fi, 2, NULL) == 2 && fi[0][1] == 10 && fi[1][1] == 9);   // a smaller buffer gets the newest

    // --- the pacer's vsync grid: frames 11 on fill the ring of 8 with paced presents ---
    uint64_t const T = 16666667ull, G0 = 10000000000ull;   // one vsync and the pacer's grid, on the now_ns clock
    g_gns = 6000000000ull; g_hns = 9500000000ull;          // the guest clock apart from the host one, as they run
    g_at = G0; g_grid = G0; ppresent(); ppresent();        // frames 11, 12
    g_at = G0 - 20000000ull; g_gd[12] = 1; g_gde[12] = G0 - 10000000ull;   // frame 13 presented before the grid, its GPU work done before the vsync
    ppresent(); g_hns = G0 - 1000000ull;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 1 && fi[0][3] == 0 && fi[0][4] == 0);   // its vsync still to come: not shown yet
    g_hns = G0 + 5000000ull;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == fi[0][4] && fi[0][3] == ticks(g_gns - (g_hns - G0)));   // shown at the grid's vsync, as the guest clock dates it
    g_gd[12] = 0; g_gde[12] = G0 + 50000000ull;            // latched: a later GPU-state change cannot revert it or move its flip tick
    g_hns += 1000000ull; g_gns += 1000000ull;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == ticks(g_gns - (g_hns - G0)));
    g_at = G0 + 1000000ull; g_gd[13] = 1; g_gde[13] = G0 + 20000000ull;   // frame 14 presented late, its GPU work ending later still
    ppresent(); g_hns = G0 + 3 * T;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == ticks(g_gns - (g_hns - (G0 + 2 * T))));   // shown at the second vsync past the grid, past its GPU end
    g_at = G0; g_gd[14] = 0;                              // frame 15's GPU work still running
    ppresent(); g_hns = G0 + 1000000000ull;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 1 && fi[0][3] == 0);               // however late the query, not shown while its GPU work runs
    g_at = G0 + 10000000ull;                              // frame 16: no GPU record (g_gd[15] stays -1)
    ppresent(); g_hns = G0 + 2 * T;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == ticks(g_gns - (g_hns - (G0 + T))));   // no record: the first grid tick at or after its present
    g_at = G0 - 5000000ull; g_gd[16] = 0; g_gd[17] = 1; g_gde[17] = G0 - 1000000ull;   // frame 17: the GPU work asked about is 16's (running), not 17's
    ppresent(); g_hns = G0 + T;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 1 && fi[0][3] == 0);
    g_at = G0;                                            // frame 18: the backend's flip time comes back
    ppresent(); g_pt[18] = (g_hns - 30000000ull) / 1e9;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == ticks(g_gns - 30000000ull));   // a flip time wins over the grid
    g_at = G0 - 2000000ull; g_gd[18] = 1; g_gde[18] = G0 - 1000000ull;   // frame 19: dropped on a paced grid still resolves at the grid's vsync
    ppresent(); g_pt[19] = -1; g_hns = G0 + 5000000ull;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == ticks(g_gns - (g_hns - G0)));

    // --- the present's own ticket (dm_vi_frame_ticket, from the game thread or the DM_NVN_ASYNC replay thread) ---
    g_count++; g_hns = G0 + 5000000ull;                   // frame 20 queued before its ticket is known: queued, though the present queue is past frame 19
    dm_vi_frame_queued(20, G0 - 2000000ull, G0, T);
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 20 && fi[0][0] == 1 && fi[0][3] == 0 && fi[0][4] == 0);
    g_in++; dm_vi_frame_ticket(20, g_in); g_pt[20] = (g_hns - 12000000ull) / 1e9;   // its ticket, not handed on yet: still queued
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 20 && fi[0][0] == 1 && fi[0][3] == 0);
    g_done = g_in;                                        // handed on: its own flip time
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 20 && fi[0][0] == 2 && fi[0][3] == ticks(g_gns - 12000000ull));
    g_in++; g_done = g_in; g_pt[21] = (g_hns - 8000000ull) / 1e9;   // frame 21: the ticket comes before the game thread's record
    dm_vi_frame_ticket(21, g_in);
    g_count++;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 20);                               // 21 not recorded yet: skipped
    dm_vi_frame_queued(21, 0, 0, 0);
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 21 && fi[0][0] == 2 && fi[0][3] == ticks(g_gns - 8000000ull));   // the ticket kept
    dm_vi_frame_ticket(13, 13);                           // a stale ticket for frame 13 (slot 21 % 8) leaves 21 alone
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 21 && fi[0][0] == 2 && fi[0][3] == ticks(g_gns - 8000000ull));
    g_count++; dm_vi_frame_queued(22, 0, 0, 0); dm_vi_frame_ticket(22, 0);   // frame 22 presented nothing (ticket 0): shown
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 22 && fi[0][0] == 2 && fi[0][3] == ticks(g_gns));
    puts("vi frame history: PASS");
    return 0;
}
'''

INTERVAL_HEAD = r'''
typedef struct { int interval; } win_t;
typedef struct NVNwindow NVNwindow;
#define NVN(name, ret, ...) static ret name(__VA_ARGS__)
static int g_hz = -1, g_display_mode;
static _Atomic int g_fs_k;
static int g_cap, g_fstep, g_display;
static int fps_step_cap(void) { return g_cap; }
static int fstep_on(void) { return g_fstep; }
static int mtl_display_hz(void) { return 60; }
static int dm_pacing_display(const char **src) { *src = "test"; return g_display; }   // the pacing policy's display mode (dm_pacing.c)
'''

INTERVAL_MAIN = r'''
int main(void) {
    win_t w = {2}; const NVNwindow *nw = (const NVNwindow *)&w;
    // Before the game asks for the layer's frame history: the interval it set, whatever the mode, the cap or the frame step, exactly as before the history.
    // A present_interval display game that sets 2 and never asks reads back its 2
    g_display = 1;                                   // display mode, as the pacing policy says
    assert(nvnWindowGetPresentInterval(nw) == 2);
    g_cap = 30; assert(nvnWindowGetPresentInterval(nw) == 2);
    g_fstep = 1; atomic_store(&g_fs_k, 3); assert(nvnWindowGetPresentInterval(nw) == 2);
    w.interval = 1; assert(nvnWindowGetPresentInterval(nw) == 1);
    w.interval = 0; assert(nvnWindowGetPresentInterval(nw) == 0);
    assert(g_hz == -1 && !g_display_mode);           // nor does the getter read the pacing policy (present_cfg still runs at the first present)
    // The game asks (Unreal's count query; Luigi's Mansion 3 before each present): from then on the window's own interval in display mode
    w.interval = 2; g_cap = 0; g_fstep = 0; atomic_store(&g_fs_k, 0); g_display = 0;
    assert(h_vi_FrameHist(NULL, 0, NULL) == 8);
    assert(nvnWindowGetPresentInterval(nw) == 2);    // game mode: the interval the game set
    g_cap = 30; assert(nvnWindowGetPresentInterval(nw) == 2);   // game mode under a cap: still the game's own
    g_hz = -1; g_display = 1; g_cap = 0;             // display mode, as the pacing policy says
    assert(nvnWindowGetPresentInterval(nw) == 1);    // one frame per tick whatever the game set (Luigi's Mansion 3 no longer paces for its 2)
    g_cap = 30; assert(nvnWindowGetPresentInterval(nw) == 2);   // under a 30 cap: the cap's interval
    g_fstep = 1; atomic_store(&g_fs_k, 3); assert(nvnWindowGetPresentInterval(nw) == 3);   // the frame step's hold, which presentation follows
    w.interval = 1; g_cap = 0; g_fstep = 0; assert(nvnWindowGetPresentInterval(nw) == 1);
    puts("window present interval: PASS");
    return 0;
}
'''


def history_cut():
    vi = csrc.read('nxnative/dm/gfx/hle_vi.c')
    return csrc.between(vi, '#define VFH 8', '// Rhythm Heaven Groove', 'hle_vi.c frame history')


def history_source():
    return HISTORY_HEAD + history_cut() + HISTORY_MAIN


def interval_source():
    nvn = csrc.read('nxnative/dm/gfx/nvn/hle_nvn.c')
    getter = csrc.between(nvn, 'static int window_interval(const win_t *o);\n', 'NVN(nvnWindowSetPresentInterval', 'hle_nvn.c nvnWindowGetPresentInterval')
    policy = csrc.between(nvn, 'static void present_cfg(const win_t *o) {', 'static uint64_t present_period(', 'hle_nvn.c present_cfg .. window_interval')
    return HISTORY_HEAD + history_cut() + INTERVAL_HEAD + getter + policy + INTERVAL_MAIN   # the real ListFrameHistory tells the getter the game asked


def run(source, name):
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / f'{name}.c'; exe = Path(td) / name
        src.write_text(source, encoding='utf-8')
        b = subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Werror', '-Wno-unused-function', '-pthread', str(src), '-o', str(exe)], capture_output=True, text=True)
        if b.returncode: raise AssertionError(f'{name} does not build:\n{b.stderr}')
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        if r.returncode: raise AssertionError(f'{name} failed ({r.returncode}):\n{r.stdout}{r.stderr}')
        return r.stdout


@unittest.skipUnless(shutil.which('cc') and host_cc.posix(), 'host C compiler with pthreads required')
class ViFrameHistory(unittest.TestCase):
    def test_history_ring(self):
        self.assertIn('PASS', run(history_source(), 'vi_history'))

    def test_window_present_interval(self):
        self.assertIn('PASS', run(interval_source(), 'window_interval'))

    def test_backends_supply_the_history(self):
        # Every graphics backend gives hle_vi.c what it asks for, and both of hle_nvn.c's present paths tell it each present's ticket
        need = {'nxnative/dm/android/gfx_vk_present.c': ('void mtl_present_progress(', 'double mtl_flip_time('),
                'nxnative/dm/android/gfx_null.c': ('void mtl_present_progress(', 'double mtl_flip_time('),
                'nxnative/dm/android/gfx_vk_core.c': ('int mtl_gpu_done(',),
                'nxnative/dm/android/gfx_common.c': ('int mtl_gpu_done(',),
                'nxnative/dm/gfx/nvn/nvn_mtl.m': ('void mtl_present_progress(', 'double mtl_flip_time(', 'int mtl_gpu_done(')}
        for rel, heads in need.items():
            text = csrc.read(rel)
            for h in heads: self.assertIn(h, text, f'{rel} lacks {h}')
        nvn = csrc.read('nxnative/dm/gfx/nvn/hle_nvn.c')
        self.assertIn('dm_vi_frame_ticket(j.frame, tk)', nvn)            # the DM_NVN_ASYNC replay thread
        self.assertIn('dm_vi_frame_ticket(g_frame + 1, o->ticket[idx])', nvn)   # the game thread
        self.assertIn('dm_vi_frame_queued(g_frame,', nvn)


if __name__ == '__main__':
    unittest.main()
