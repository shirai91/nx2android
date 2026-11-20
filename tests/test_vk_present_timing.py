"""Exercise the Android presentation history without a Vulkan device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402


SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_present.c'


class PresentTiming(unittest.TestCase):
    def test_frame_pairs_without_driver_display_times(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: self.skipTest('a C compiler is required')
        src = SOURCE.read_text(encoding='utf-8')
        front = SOURCE.with_name('front_android.c').read_text(encoding='utf-8')
        start = src.index('enum framegen_outcome {')
        cadence = src[start:src.index('static framegen_cadence g_fg_cadence;', start)]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <math.h>
#define VKLOG(...) ((void)0)
typedef struct { int unused; } vkimg_t;
static struct { int has_dtiming; } V;
static uint64_t g_pace_anchor, g_refresh_ns, g_fg_sample_ns, g_fg_cpu_present_ns;
static uint64_t platform_refresh = 16666667;
static double window_vote, real_period;
static int g_pace_on = 1, held, menu = -1, captures, followups, histories, forgets, goal, usable_rate, fail_setup;
static _Atomic int g_set_framegen = 1;
static int available;
static int vk_lsfg_available(void) { return available; }
uint8_t *dm_fps_state;
''' + csrc.function(front, 'int dm_framegen_enabled(void)') + csrc.between(src, 'enum { PRESENT_REAL, PRESENT_CAPTURE', 'static enum present_result present_window(') + cadence + r'''
static framegen_cadence g_fg_cadence;
static enum framegen_outcome last_outcome;
static uint64_t mono_ns(void) { return 1; }
static uint64_t dm_display_refresh_ns(void) { return platform_refresh; }
static int dm_opmode(void) { return 0; }
static int dm_external_active(void) { return 0; }
static int dm_framegen_target(void) { return goal; }
static void dm_perf_framegen_rate(int fps) { usable_rate = fps; }
static int dm_game_held(void) { return held; }
static int dm_menu_state(void) { return menu; }
static int vk_lsfg_poll(uint64_t n) { (void)n; return 0; }
static int dm_pacing_window_game_rate(void) { return 0; }
int nvn_game_fps(void) { return 30; }
static void window_rate(double p, uint64_t n) { window_vote = p; (void)n; }
static void vk_lsfg_forget(void) { forgets++; }
static int swap_make(void) { return 1; }
static void log_framegen_rate(uint64_t n, enum framegen_outcome outcome, uint64_t refresh, uint64_t pair) {
    g_fg_sample_ns = n; last_outcome = outcome; (void)refresh; (void)pair;
}
static enum present_result present_window(vkimg_t *im, const int c[4], double p, uint64_t t, int m) {
    (void)im; (void)c; (void)t;
    if (m == PRESENT_REAL) real_period = p;
    if ((m == PRESENT_CAPTURE || m == PRESENT_HISTORY) && fail_setup) {
        if (fail_setup == 2) available = 0;
        return PRESENT_SETUP_FAILED;
    }
    if (m == PRESENT_CAPTURE) { captures++; return PRESENT_GENERATED; }
    if (m == PRESENT_HISTORY) histories++;
    if (m == PRESENT_AFTER_MIDPOINT) followups++;
    return PRESENT_SUBMITTED;
}
''' + csrc.between(src, 'static int framegen_display_clock(void) {', 'static void framegen_wait_followup(') + csrc.function(src, 'static void present_game_frame(') + r'''
static void run_frames(void) {
    g_fg_cadence = (framegen_cadence){0}; captures = followups = histories = 0;
    for (int i = 1; i <= 8; ++i) present_game_frame(NULL, NULL, 1.0/30, i, 1000000000ull + i * 33333333ull, 0);
}
int main(void) {
    unsetenv("DM_VK_PACE");
    V.has_dtiming = 1; goal = 60;
    run_frames(); assert(captures == 0 && histories == 0 && usable_rate == 0 && real_period == 1.0/30);
    assert(fabs(window_vote - 1.0/30) < 1e-9);  // Missing DLL retains ordinary presentation pacing.
    available = 1; run_frames(); assert(captures > 0 && captures == followups); // extension with no feedback
    assert(fabs(window_vote - 1.0/60) < 1e-9);
    available = 0; run_frames(); assert(captures == 0 && histories == 0 && usable_rate == 0);
    available = 1;
    g_pace_anchor = 1; g_refresh_ns = 16666667; platform_refresh = 0;
    run_frames(); assert(captures > 0 && captures == followups);  // actual driver timing
    g_pace_anchor = 0; run_frames(); assert(captures == 0);       // unknown refresh: no guess
    assert(last_outcome == FG_NO_REFRESH);
    platform_refresh = 16666667; g_set_framegen = 0; run_frames(); assert(captures == 0 && usable_rate == 0);
    g_set_framegen = 1; goal = 0; held = 1; run_frames(); assert(captures == 0);
    held = 0; menu = 0; run_frames(); assert(captures == 0);
    menu = -1; setenv("DM_VK_PACE", "0", 1); run_frames(); assert(captures == 0);
    unsetenv("DM_VK_PACE"); run_frames(); assert(captures > 0);
    int previous = captures;
    int previous_forgets = forgets;
    present_game_frame(NULL, NULL, 1.0/30, 9, 1300000000ull, 1); assert(captures == previous);
    assert(last_outcome == FG_QUEUED && forgets == previous_forgets + 1 && real_period == 0);
    present_game_frame(NULL, NULL, 1.0/30, 10, 1333333333ull, 0);
    assert(captures == previous + 1);  // rebuild image history immediately after the queue drains
    platform_refresh = 8333333; g_fg_cadence = (framegen_cadence){0}; captures = followups = 0;
    for (int i = 1; i <= 8; ++i) present_game_frame(NULL, NULL, 1.0/60, i, 2000000000ull + i * 16666667ull, 0);
    assert(captures > 0 && captures == followups);
    assert(fabs(window_vote - 1.0/120) < 1e-9);  // request room for 60 -> 120 too
    goal = 60; platform_refresh = 16666667; run_frames();
    assert(usable_rate == 60 && captures == 4 && captures == followups);
    goal = 0; platform_refresh = 8333333; captures = followups = 0;
    for (int i = 1; i <= 8; ++i) present_game_frame(NULL, NULL, 1.0/60, i, 3000000000ull + i * 16666667ull, 0);
    assert(usable_rate == 0 && captures == 4 && captures == followups);
    assert(fabs(window_vote - 1.0/120) < 1e-9);  // no cap: 60 -> 120 when the display permits it
    goal = 60;
    captures = followups = histories = 0;
    uint32_t patched_step[2] = {0}; dm_fps_state = (uint8_t *)patched_step;  // a game with its own timing
    for (int i = 1; i <= 12; ++i) present_game_frame(NULL, NULL, 1.0/40, i, 4000000000ull + i * 25000000ull, 0);
    assert(usable_rate == 0 && captures == 4 && followups == 4 && histories == 4);
    dm_fps_state = NULL;
    fail_setup = 1; goal = 60; platform_refresh = 16666667;
    for (int i = 1; i <= 12; ++i) present_game_frame(NULL, NULL, 1.0/30, i, 5000000000ull + i * 33333334ull, 0);
    assert(last_outcome == FG_SETUP_FAILED && usable_rate == 0);
    fail_setup = 0; g_set_framegen = 0; run_frames();
    assert(usable_rate == 0 && captures == 0);
    fail_setup = 2; g_set_framegen = 1; run_frames();
    assert(!available && usable_rate == 0 && captures == 0 && histories == 0);
    assert(framegen_followup_deadline(1000000000ull, .025) == 1025000000ull);
    assert(forgets > 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_fg_no_timing_') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(c), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_framegen_tracks_render_cadence(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the frame cadence check')
        src = SOURCE.read_text(encoding='utf-8')
        start = src.index('enum framegen_outcome {')
        cadence = src[start:src.index('static framegen_cadence g_fg_cadence;', start)]
        harness = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
''' + cadence + r'''
int main(void) {
    const uint64_t refresh = 16666667;
    assert(framegen_output_rate(60, refresh) == 60);
    assert(framegen_output_rate(60, 11111111) == 60);
    assert(framegen_output_rate(60, 20833333) == 48);
    assert(framegen_output_rate(0, 11111111) == 0);
    framegen_cadence budget = {.interval_ns = 25000000};
    assert(framegen_pair_budget(&budget, 60) == 0 && framegen_pair_budget(&budget, 60) == 1);
    framegen_cadence cadence = {0};
    uint64_t now = 1000000000;
    for (int i = 0; i < 4; ++i, now += 50000000)
        assert(framegen_step(&cadence, now, refresh) == 0);
    assert(fabs(framegen_step(&cadence, now, refresh) - 0.025) < 1e-9);
    for (int i = 0; i < 2; ++i) {
        now += 50000000;
        assert(framegen_frame_step(&cadence, now, refresh, 1) == 0);
        assert(cadence.stable == 4);
    }
    now += 50000000;
    assert(framegen_frame_step(&cadence, now, refresh, 0) > 0);
    for (int i = 0; i < 4; ++i) {
        now += 50000000;
        assert(framegen_frame_step(&cadence, now, refresh, 1) == 0);
    }
    assert(cadence.stable == 0 && cadence.queued_frames == 3);
    for (int i = 0; i < 4; ++i) {
        now += 50000000;
        assert(framegen_frame_step(&cadence, now, refresh, 0) == 0);
    }
    now += 50000000;
    assert(framegen_frame_step(&cadence, now, refresh, 0) > 0);
    now += 54000000;
    assert(fabs(framegen_step(&cadence, now, refresh) - 0.0255) < 1e-9);

    now += 101000000;
    assert(framegen_step(&cadence, now, refresh) == 0);
    assert(cadence.outcome == FG_STALL);
    for (int i = 0; i < 3; ++i) {
        now += 50000000;
        assert(framegen_step(&cadence, now, refresh) == 0);
    }
    now += 50000000;
    assert(framegen_step(&cadence, now, refresh) > 0);
    assert(framegen_step(&cadence, now + 10000000, refresh) == 0);  // bunched input
    assert(cadence.outcome == FG_BUNCHED);
    assert(framegen_step(&cadence, 0, refresh) == 0);  // pause or backlog
    assert(framegen_step(&cadence, now + 50000000, refresh) == 0);
    assert(cadence.outcome == FG_WARMUP);

    cadence = (framegen_cadence){0};
    for (int i = 0; i < 12; ++i, now += 25000000)
        assert(framegen_step(&cadence, now, refresh) == 0);  // 80 fps will not fit
    cadence = (framegen_cadence){0};
    for (int i = 0; i < 4; ++i, now += 33333333)
        assert(framegen_step(&cadence, now, refresh) == 0);
    assert(fabs(framegen_step(&cadence, now, refresh) - refresh / 1e9) < 1e-9);
    // A brief burst around a sustainable 30 fps cadence keeps its history.
    for (int i = 0; i < 3; ++i) {
        now += 28000000;
        assert(fabs(framegen_step(&cadence, now, refresh) - refresh / 1e9) < 1e-9);
    }
    now += 49200000;
    assert(framegen_step(&cadence, now, refresh) > 0);
    // Sustained excess input ends the grace period even when close to the display limit.
    cadence.interval_ns = 31000000;
    cadence.near_limit = 0;
    for (int i = 0; i < 3; ++i) {
        now += 31000000;
        assert(framegen_step(&cadence, now, refresh) > 0);
    }
    now += 31000000;
    assert(framegen_step(&cadence, now, refresh) == 0);
    assert(cadence.outcome == FG_REFRESH_LIMIT);
    assert(framegen_step(&cadence, now + 33333333, 0) == 0);
    assert(framegen_followup_deadline(1000000000ull, 0.025) == 1025000000ull);
    assert(framegen_followup_deadline(0, 0.025) == 0);
    assert(framegen_followup_deadline(1000000000ull, 0) == 0);
    assert(framegen_followup_deadline(1000000000ull, -0.1) == 0);
    assert(framegen_followup_deadline(1000000000ull, 0.1) == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_framegen_cadence_') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_swapchain_recreated_on_window_resize(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the resize check')
        src = SOURCE.read_text(encoding='utf-8')
        start = src.index('static int swap_make(void)')
        prefix = src[start:src.index('    VkAndroidSurfaceCreateInfoKHR', start)]
        harness = r'''
#include <assert.h>
#include <stdint.h>
typedef struct { int width, height; } ANativeWindow;
static ANativeWindow *g_win, *current;
static int g_swap, destroys;
static struct { uint32_t width, height; } g_sext;
static ANativeWindow *dm_front_window(void) { return current; }
static int ANativeWindow_getWidth(ANativeWindow *w) { return w->width; }
static int ANativeWindow_getHeight(ANativeWindow *w) { return w->height; }
static void swap_destroy(void) { g_swap = 0; destroys++; }
''' + prefix + r'''
    return 2;
}
int main(void) {
    ANativeWindow w = {3200, 1440}; current = g_win = &w;
    g_swap = 1; g_sext.width = 3200; g_sext.height = 1440;
    assert(swap_make() == 1 && destroys == 0);
    w.width = 1440; w.height = 3200;
    assert(swap_make() == 2 && destroys == 1);
    g_swap = 1; g_sext.width = 1440; g_sext.height = 3200;
    w.width = 3200; w.height = 1440;
    assert(swap_make() == 2 && destroys == 2);
    g_swap = 1; w.width = 0;
    assert(swap_make() == 1 && destroys == 2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_present_resize_') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ticket_history(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the presentation history check')
        src = SOURCE.read_text(encoding='utf-8')
        history = csrc.between(src, 'enum { PRES_TIMES', 'static VkSurfaceKHR g_surf')
        harness = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
''' + history + r'''
int main(void) {
    assert(present_time_get(0) == 0);
    assert(present_time_get(1) == 0);
    present_time_begin(1, 0, 950000000ull);
    assert(present_time_get(1) == 0);  // queued is not displayed
    present_time_map(42, 1, 0);
    present_time_begin(2, 0, 1980000000ull);
    present_time_map(43, 2, 0);
    present_time_record(43, 2000000000ull);  // results need not be in ticket order
    present_time_record(42, 1000000000ull);
    assert(present_time_get(1) == 1);
    assert(present_time_get(2) == 2);
    assert(g_real_latency.samples == 2 && g_real_latency.total_ns == 70000000ull && g_real_latency.max_ns == 50000000ull);
    present_time_record(43, 0);
    assert(present_time_get(2) == 2);
    present_time_map(44, 0, 0);  // a menu redraw has a Vulkan ID but no game ticket
    present_time_record(44, 3000000000ull);
    assert(present_time_get(2) == 2);
    assert(present_time_get(0) == 0);
    assert(g_real_displayed == 2 && g_generated_displayed == 0);
    present_time_map(45, 0, 1);
    present_time_record(45, 3500000000ull);
    present_time_record(45, 3500000000ull);  // driver feedback may be repeated
    assert(g_real_displayed == 2 && g_generated_displayed == 1);
    assert(g_real_latency.samples == 2);
    assert(present_time_get(2) == 2 && present_time_get(0) == 0);

    present_time_begin(3, 0, 0);
    present_time_map(42 + PRES_TIMES, 3, 0);
    present_time_record(42, 9000000000ull);  // overwritten Vulkan ID mapping
    assert(present_time_get(3) == 0);
    present_time_record(42 + PRES_TIMES, 3000000000ull);
    assert(present_time_get(3) == 3);
    present_time_begin(1 + PRES_TIMES, 0, 0);
    present_time_map(200, 1, 0);  // a delayed result must not overwrite a newer ticket
    present_time_record(200, 9000000000ull);
    assert(present_time_get(1) == 0);
    assert(present_time_get(1 + PRES_TIMES) == 0);

    present_time_begin(5, 7000000000ull, 0);  // no display times: the hand-over time stands in at once
    assert(present_time_get(5) == 7);
    present_time_reset();
    assert(present_time_get(2) == 0);
    present_time_begin(3, 0, 0);
    present_time_record(42 + PRES_TIMES, 9000000000ull);  // old swapchain feedback
    assert(present_time_get(3) == 0);
    present_time_map(UINT32_MAX, 3, 0);
    present_time_record(UINT32_MAX, 3000000000ull);
    assert(present_time_get(3) == 3);
    present_time_begin(4, 0, 0);
    present_time_map(0, 4, 0);  // Vulkan's 32-bit present ID may wrap
    present_time_record(0, 4000000000ull);
    assert(present_time_get(4) == 4);
    present_time_begin(5, 0, 0);  // failed, headless or unsupported presents get no result
    assert(present_time_get(5) == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_present_timing_') as tmp:
            c = Path(tmp) / 'check.c'
            exe = Path(tmp) / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
