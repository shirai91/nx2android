"""Exercise fstep_tick recovery without a display link (Android present-queue path)."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402


HARNESS_HEAD = r'''
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nvn_fstep.h"

typedef struct { uint64_t presents; } win_t;

static uint64_t g_fake_now_ns = 10000000000ULL;
static uint64_t g_frame;
static uint64_t g_fs_start, g_fs_wait, g_fs_pq;
static _Atomic uint64_t g_fs_replay_ns;
static _Atomic int g_fs_k;
static nvn_fstep g_fs;
static nvn_fgpu g_fg;
static int g_fs_pshare = 100;
static int g_link_active;
static int g_screen_hz = 120;
static int g_cap = 60;
static int g_hold = 1;
static double g_gpu_ms = -1.0;
static uint8_t g_fake_fps_state[0x40];
static uint8_t *dm_fps_state = g_fake_fps_state;

static void log_noop(const char *fmt, ...) { (void)fmt; }
#define LOG(...) log_noop(__VA_ARGS__)

static uint64_t now_ns(void) { return g_fake_now_ns; }
static double mtl_gpu_ms(uint64_t f) { (void)f; return g_gpu_ms; }
static int mtl_link_active(void) { return g_link_active; }
static int fps_step_cap(void) { return g_cap; }
static int fstep_hold(void) { return g_hold; }
static int front_screen_max_fps(void) { return g_screen_hz; }
static int fstep_cpu_slowed(uint64_t presents) { (void)presents; return 0; }
static int dm_thermal_now(void) { return 0; }
static int dm_low_power_now(void) { return 0; }
'''

HARNESS_TAIL = r'''
static void step_frame(win_t *w, double shown_vs, double pw_vs, double pq_vs, double rpl_vs, double gpu_ms) {
    const double v = 1e9 / 60.0;
    g_fake_now_ns += (uint64_t)llround(shown_vs * v);
    g_fs_start = g_fake_now_ns;
    g_fs_wait = (uint64_t)llround(pw_vs * v);
    g_fs_pq = (uint64_t)llround(pq_vs * v);
    atomic_store(&g_fs_replay_ns, (uint64_t)llround(rpl_vs * v));
    g_gpu_ms = gpu_ms;
    g_frame++;
    w->presents++;
    fstep_tick(w);
}

static void init_state(int link, int hz) {
    g_link_active = link;
    g_screen_hz = hz;
    float cap = (float)g_cap;
    memcpy(dm_fps_state, &cap, sizeof cap);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *mode = argv[1];
    win_t w = {0};

    if (strcmp(mode, "android_recover_20_to_60") == 0) {
        init_state(0, 120);
        // Baseline 60 fps so g_fg learns a clean kmin reference before overload.
        for (int i = 0; i < 40; i++) step_frame(&w, 1.0, 0.0, 0.25, 0.30, 12.0);
        assert(g_fs.k == 1 && atomic_load(&g_fs_k) == 1);
        assert(g_fs.shown_slack == 0.0);

        // Heavy scene pushes 60 -> 30 -> 20 fps.
        for (int i = 0; i < 4; i++) step_frame(&w, 1.60, 0.0, 0.0, 1.55, 26.0);
        assert(g_fs.k == 2);
        for (int i = 0; i < 4; i++) step_frame(&w, 2.60, 0.0, 0.0, 2.55, 42.0);
        assert(g_fs.k == 3 && atomic_load(&g_fs_k) == 3);

        // Load clears, while Android triple buffering holds pq ~2.2 vsyncs and
        // acquire-to-acquire shown intervals jitter up to ~3.45 vsyncs (~57.5 ms).
        for (int i = 0; i < 300 && g_fs.k > 2; i++) {
            double jitter = (i % 4 == 0) ? 0.35 : 0.05;
            step_frame(&w, 3.05 + jitter, 0.0, 2.20, 0.25, 8.0);
        }
        assert(g_fs.k == 2);

        // At 30 fps with minor acquire jitter (~36 ms = 2.18 vsyncs), it must
        // keep advancing fit_run and recover cleanly to 60 fps.
        for (int i = 0; i < 300 && g_fs.k > 1; i++) {
            double jitter = (i % 5 == 0) ? 0.18 : 0.02;
            step_frame(&w, 2.00 + jitter, 0.0, 1.40, 0.25, 6.5);
        }
        assert(g_fs.k == 1 && atomic_load(&g_fs_k) == 1);
        assert(fabs(g_fs.shown_slack - 0.45) < 1e-9);
        step_frame(&w, 1.0, 0.0, 0.25, 0.25, 6.5);
        assert(g_fs.k == 1 && g_fs.shown_slack == 0.0);
    } else if (strcmp(mode, "android_replay_bound_holds_30") == 0) {
        init_state(0, 120);
        for (int i = 0; i < 20; i++) step_frame(&w, 1.0, 0.0, 0.25, 0.30, 8.0);
        for (int i = 0; i < 4; i++) step_frame(&w, 1.60, 0.0, 0.0, 1.55, 10.0);
        assert(g_fs.k == 2);

        // Replay thread saturates 1.45 vsyncs (~24 ms) even though gpu=6.5 ms and
        // shown-pq=0.50 vsyncs look cheap: must NOT step up to 60 fps and flap.
        for (int i = 0; i < 120; i++) {
            step_frame(&w, 2.10, 0.0, 1.60, 1.45, 6.5);
            assert(g_fs.k == 2);
        }
    } else if (strcmp(mode, "android_no_gpu_ts_steps_up_at_kmin") == 0) {
        init_state(0, 120);
        // First frame seeds prev, then k == kmin stays strict (shown_slack == 0).
        step_frame(&w, 1.0, 0.0, 0.0, 0.20, -1.0);
        assert(g_fs.k == 1 && g_fs.shown_slack == 0.0);

        // With no GPU timestamps (-1) and shown=1.25 (> 1.10 but < 1.55), strict
        // kmin check must still detect late frames and step down to 30 fps.
        for (int i = 0; i < 4; i++) step_frame(&w, 1.25, 0.0, 0.60, 0.30, -1.0);
        assert(g_fs.k == 2);
    } else if (strcmp(mode, "android_real_overrun_at_30_steps_to_20_then_recovers") == 0) {
        init_state(0, 120);
        for (int i = 0; i < 30; i++) step_frame(&w, 1.0, 0.0, 0.25, 0.30, 10.0);
        for (int i = 0; i < 4; i++) step_frame(&w, 1.60, 0.0, 0.0, 1.50, 24.0);
        assert(g_fs.k == 2);

        // Genuine 30 fps overrun: shown=2.65 (> 2.55) AND cost=2.30 (> 2.10).
        for (int i = 0; i < 4; i++) step_frame(&w, 2.65, 0.0, 0.0, 2.30, 38.0);
        assert(g_fs.k == 3);

        for (int i = 0; i < 300 && g_fs.k > 2; i++) step_frame(&w, 3.10, 0.0, 2.10, 0.25, 7.0);
        assert(g_fs.k == 2);
        for (int i = 0; i < 300 && g_fs.k > 1; i++) step_frame(&w, 2.05, 0.0, 1.35, 0.25, 6.0);
        assert(g_fs.k == 1);
    } else if (strcmp(mode, "display_link_unchanged") == 0) {
        init_state(1, 165);
        for (int i = 0; i < 20; i++) step_frame(&w, 1.0, 0.1, 0.0, 0.30, 10.0);
        assert(fabs(g_fs.shown_slack - (60.0 / 165.0)) < 1e-9);

        // On a display link, pq is NOT subtracted from cpu_raw (pw=0 => cost stays high).
        for (int i = 0; i < 4; i++) step_frame(&w, 1.60, 0.0, 1.20, 0.20, 6.0);
        assert(g_fs.k == 2);
    } else {
        return 2;
    }
    return 0;
}
'''


class FstepAndroidTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            raise unittest.SkipTest('a C compiler is needed for the fstep_tick check')
        src = csrc.read('nxnative/dm/gfx/nvn/hle_nvn.c')
        fstep_tick = csrc.function(src, 'static void fstep_tick(', 'hle_nvn.c')
        cls._tmp = tempfile.TemporaryDirectory(prefix='nx2a_fstep_android_')
        tmp = Path(cls._tmp.name)
        c, cls.exe = tmp / 'check.c', tmp / 'check'
        c.write_text(HARNESS_HEAD + fstep_tick + HARNESS_TAIL, encoding='utf-8')
        nvn_dir = csrc.REPO / 'nxnative/dm/gfx/nvn'
        res = subprocess.run(
            [cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', f'-I{nvn_dir}', str(c), '-lm', '-o', str(cls.exe)],
            capture_output=True, text=True,
        )
        if res.returncode != 0:
            cls._tmp.cleanup()
            raise AssertionError(res.stdout + res.stderr)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, '_tmp'):
            cls._tmp.cleanup()

    def test_scenarios(self):
        cases = (
            'android_recover_20_to_60',
            'android_replay_bound_holds_30',
            'android_no_gpu_ts_steps_up_at_kmin',
            'android_real_overrun_at_30_steps_to_20_then_recovers',
            'display_link_unchanged',
        )
        for case in cases:
            with self.subTest(case=case):
                res = subprocess.run([str(self.exe), case], capture_output=True, text=True)
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)


if __name__ == '__main__':
    unittest.main()
