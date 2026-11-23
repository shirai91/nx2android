"""DM_GPU_TURBO=need and DM_PIN_REPLAY=need (nxnative/dm/android/perf_android.c turbo_need, replay_need, pin_set): the turbo and the prime cores only
while a game needs them. The functions are cut from the source and run against stubbed clocks, frame states and core lists."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402


@unittest.skipUnless(sys.platform.startswith('linux') and (shutil.which('cc') or shutil.which('clang')), 'Linux and a C compiler (sched_setaffinity, cpu_set_t)')
class PerfNeed(unittest.TestCase):
    def test_turbo_and_pinning_follow_need(self):
        src = csrc.read('nxnative/dm/android/perf_android.c')
        need_off = csrc.between(src, '#define NEED_OFF_NS', 'static int frame_held(void);', 'perf_android.c')
        turbo = csrc.function(src, 'static int turbo_need(int on) {', 'perf_android.c')
        pin_defs = csrc.between(src, '#define PIN_ON_NS', 'static int64_t g_replay_mean;', 'perf_android.c')
        replay = csrc.function(src, 'static void replay_need(void) {', 'perf_android.c')
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int nlog;
#define LOG(...) do { nlog++; if (getenv("V")) { printf(__VA_ARGS__); printf("\n"); } } while (0)
#include <stdlib.h>
static int64_t t_now; static int64_t perf_now(void) { return t_now; }
static int late_n, held_n; static int shown_late(void) { return late_n; } static int frame_held(void) { return held_n; }
static int64_t g_gpu_mean;
''' + need_off + '\n' + turbo + r'''
static int64_t g_replay_mean;
static int pins[8], npins, pin_result; static long pin_hi = 4320;
static int pin_set(int which, char *l, size_t ln, long *hi) { pins[npins++] = which; snprintf(l, ln, which ? " 6 7" : " 0 1 2 3 4 5 6 7"); *hi = pin_hi; if (pin_result == -1) errno = EINVAL; return pin_result; }
''' + pin_defs + replay + r'''
static void calls(int n) { for (int i = 0; i < n; i++) replay_need(); }
#define S 1000000000ll
int main(void) {
    /* turbo: off while the game is at its target and nothing is late */
    t_now = 1 * S; assert(turbo_need(0) == 0);
    /* the frame step holds frames below the cap: on, though no frame is late */
    held_n = 1; assert(turbo_need(0) == 1);
    /* at the target again with room at the highest clock: kept on for its hold (10 s) */
    held_n = 0; g_gpu_mean = 9000000; t_now = 5 * S; assert(turbo_need(1) == 1);
    t_now = 12 * S; assert(turbo_need(1) == 0);
    /* late frames alone also turn it on; back on within 30 s doubles the hold to 20 s */
    late_n = 4; t_now = 20 * S; assert(turbo_need(0) == 1);
    late_n = 0; t_now = 35 * S; assert(turbo_need(1) == 1);   /* 15 s: still within the 20 s hold */
    t_now = 41 * S; assert(turbo_need(1) == 0);
    /* a GPU at the highest clock without room (over 70% of a 60 fps frame) keeps it on */
    held_n = 1; t_now = 100 * S; assert(turbo_need(0) == 1);
    held_n = 0; g_gpu_mean = 13000000; t_now = 200 * S; assert(turbo_need(1) == 1);
    /* still held below the target keeps it on, room or not */
    g_gpu_mean = 9000000; held_n = 1; assert(turbo_need(1) == 1);
    /* any late frame keeps it on, held or not */
    held_n = 0; late_n = 1; assert(turbo_need(1) == 1);
    late_n = 0; assert(turbo_need(1) == 0);
    /* the limit was lowered by a fifth at the quick return above (11.7 -> 9.36 ms): 10 ms no longer turns it off */
    held_n = 1; t_now = 260 * S; assert(turbo_need(0) == 1);   /* 60 s after the last off: no further lowering */
    held_n = 0; g_gpu_mean = 10000000; t_now = 400 * S; assert(turbo_need(1) == 1);
    g_gpu_mean = 9000000; assert(turbo_need(1) == 0);
    /* back within 30 s: lowered again (7.49 ms), and never under 6 ms however often */
    held_n = 1; t_now = 410 * S; assert(turbo_need(0) == 1);
    held_n = 0; g_gpu_mean = 8000000; t_now = 1000 * S; assert(turbo_need(1) == 1);
    for (int i = 0; i < 6; i++) { g_gpu_mean = 5900000; t_now += 400 * S; assert(turbo_need(1) == 0); held_n = 1; t_now += 1 * S; assert(turbo_need(0) == 1); held_n = 0; }
    g_gpu_mean = 6100000; t_now += 400 * S; assert(turbo_need(1) == 1);

    /* pinning: looked at every 60 calls; nothing while the replay time is not known */
    t_now = 300 * S; calls(60); assert(npins == 0);
    /* a fast replay (Odin 3: 5 ms) stays on every core */
    g_replay_mean = 5000000; calls(600); assert(npins == 0);
    /* over 9 ms: the prime cores */
    g_replay_mean = 10000000; calls(60); assert(npins == 1 && pins[0] == 1);
    /* under 6 ms, but within 20 s of the change: kept */
    g_replay_mean = 5000000; t_now = 310 * S; calls(60); assert(npins == 1);
    /* past the 20 s but not under 6 ms (just over it, and a little under the pin's 9): kept there */
    g_replay_mean = 6100000; t_now = 330 * S; calls(600); assert(npins == 1);
    /* under 6 ms after the 20 s: every core again */
    g_replay_mean = 5000000; t_now = 340 * S; calls(60); assert(npins == 2 && pins[1] == 0);
    /* refused: logged once, tried again at the next look */
    pin_result = -1; g_replay_mean = 12000000; int l0 = nlog; calls(120); assert(npins == 4 && nlog == l0 + 1);
    pin_result = 0; calls(60); assert(npins == 5 && pins[4] == 1);
    puts("ok"); return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a-perf-need-') as tmp:
            c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
            c.write_text(harness, encoding='utf-8')
            cc = shutil.which('cc') or shutil.which('clang')
            r = subprocess.run([cc, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_pin_set_picks_the_cores(self):
        """pin_set itself on this machine: every core always works; prime and big say when the clocks are unknown or alike"""
        src = csrc.read('nxnative/dm/android/perf_android.c')
        pin_set = csrc.function(src, 'static int pin_set(int which, char *l, size_t ln, long *hi) {', 'perf_android.c')
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
''' + pin_set + r'''
int main(void) {
    char l[160]; long hi = -1;
    assert(pin_set(0, l, sizeof l, &hi) == 0 && strlen(l) > 0);   /* every core: allowed to any thread */
    int r = pin_set(1, l, sizeof l, &hi); assert(r == 0 || r == -2 || r == -1);
    assert(pin_set(0, l, sizeof l, &hi) == 0);
    char small[4]; assert(pin_set(0, small, sizeof small, &hi) == 0 && strlen(small) < sizeof small);   /* the list is cut, never overrun */
    puts("ok"); return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a-pin-set-') as tmp:
            c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
            c.write_text(harness, encoding='utf-8')
            cc = shutil.which('cc') or shutil.which('clang')
            r = subprocess.run([cc, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
