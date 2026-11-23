"""The adaptive governor and the frame step on one game (issue #377): the real controller from perf_android.c and the real nvn_fstep.h."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HOOK = '__attribute__((weak)) int dm_nvn_frame_step_owns(void) { return 0; }'

HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_fstep.h"
#define LOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#ifdef _WIN32  // MinGW (the Windows release runner) has no setenv; "NAME=" removes the variable
static int setenv(const char *k, const char *v, int o) { char s[256]; (void)o; snprintf(s, sizeof s, "%s=%s", k, v); return _putenv(s); }
static int unsetenv(const char *k) { char s[256]; snprintf(s, sizeof s, "%s=", k); return _putenv(s); }
#endif
static int g_owns;
'''

MAIN = r'''
#define VSYNC 16666667ull
// One frame: the frame step decides the hold and the game's step, the governor the period that hold is shown for
static double frame(nvn_fstep *s, double shown, double cost, int fixed, int *step) {
    game_mode_sync();
    *step = nvn_fstep_next(s, shown, cost, 1, FSTEP_MAX, fixed, 0);
    return (double)dm_present_period_adapt((uint64_t)s->k * VSYNC) / VSYNC;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *mode = argv[1];
    unsetenv("DM_FPS_PIN"); unsetenv("DM_ADAPTIVE_FPS");
    if (!strcmp(mode, "fixed_step_keeps_game_speed")) {   // BotW's VFR override fixes the step at 1: at 30 fps it would run at half speed
        g_owns = 1; g_step = 2;
        nvn_fstep s = {0}; double shown = 1, real = 0, game = 0; int step;
        for (int i = 0; i < 600; i++) { shown = frame(&s, shown, 0.3, 1, &step); real += shown; game += step; }
        assert(fabs(game / real - 1.0) < 0.01);
    } else if (!strcmp(mode, "hold_returns_to_kmin")) {   // cheap frames at hold 2: the trial back to 1 must hold
        g_owns = 1; g_step = 1;
        nvn_fstep s = {0}; s.k = s.step = 2; s.kmin = 1; s.hold = FSTEP_HOLD0;
        double shown = 2; int step, at1 = 0;
        for (int i = 0; i < 3600; i++) { shown = frame(&s, shown, 0.3, 0, &step); at1 += s.k == 1; }
        assert(s.k == 1 && s.failed == 0 && at1 > 3400);
    } else if (!strcmp(mode, "hold_off_keeps_governor")) {   // Z-A, TTYD: the governor is what slows them
        g_owns = 0; game_mode_sync(); g_step = 2; game_mode_sync();
        assert(adaptive_enabled() && dm_present_period_adapt(VSYNC) == 33333333);
    } else if (!strcmp(mode, "pin_wins")) {
        setenv("DM_FPS_PIN", "30", 1); g_owns = 1; game_mode_sync();
        assert(adaptive_enabled());
        adapt_frame(10000000, 1000000000ll);
        assert(g_step == 2 && dm_present_period_adapt(VSYNC) == 33333333);
    } else if (!strcmp(mode, "explicit_governor_wins")) {
        setenv("DM_ADAPTIVE_FPS", "1", 1); g_owns = 1; game_mode_sync(); g_step = 2; game_mode_sync();
        assert(adaptive_enabled() && dm_present_period_adapt(VSYNC) == 33333333);
    } else if (!strcmp(mode, "handing_back_starts_fresh")) {   // the frames judged before the frame step took over say nothing about the frames after
        game_mode_sync(); g_step = 2; g_nwork = 64; g_nshown = 64; g_ok_frames = 9; g_last_change = 5;
        g_owns = 1; game_mode_sync();
        assert(!adaptive_enabled() && g_step == 0 && g_nwork == 0 && g_nshown == 0 && g_ok_frames == 0 && g_last_change == 0);
        g_nwork = 3; g_owns = 0; game_mode_sync();
        assert(adaptive_enabled() && g_step == 0 && g_nwork == 0);
    } else return 2;
    return 0;
}
'''


@unittest.skipUnless(shutil.which('cc') or shutil.which('clang'), 'a C compiler is required')
class FrameStepOwnsThePeriod(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        src = (ROOT / 'nxnative/dm/android/perf_android.c').read_text(encoding='utf-8')
        controller = csrc.between(src, 'static const int64_t STEPS[]', '// hle_nvn.c, the first acquire')
        hint, shown = controller.index('// The frame\'s work to the CPU hint'), controller.index('// The frames\' time on screen')
        controller = controller[:hint] + controller[shown:]
        controller = controller.replace(HOOK, 'static int dm_nvn_frame_step_owns(void) { return g_owns; }')
        cls._tmp = tempfile.TemporaryDirectory(prefix='nx2a_fstep_governor_')
        c = Path(cls._tmp.name) / 'check.c'; cls.exe = c.with_suffix('')
        c.write_text(HARNESS + csrc.PACING_INCLUDE + controller + MAIN, encoding='utf-8')
        csrc.pacing_files(cls._tmp.name)   # perf_android.c asks dm_pacing.c
        made = subprocess.run([cc, '-std=gnu11', f'-I{cls._tmp.name}', f'-I{ROOT / "nxnative/dm/gfx/nvn"}', str(c), '-lm', '-o', str(cls.exe)], capture_output=True, text=True)
        if made.returncode: cls._tmp.cleanup(); raise AssertionError(made.stdout + made.stderr)

    @classmethod
    def tearDownClass(cls): cls._tmp.cleanup()

    def test_scenarios(self):
        for case in ('fixed_step_keeps_game_speed', 'hold_returns_to_kmin', 'hold_off_keeps_governor', 'pin_wins', 'explicit_governor_wins', 'handing_back_starts_fresh'):
            with self.subTest(case=case):
                res = subprocess.run([str(self.exe), case], capture_output=True, text=True)
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)


if __name__ == '__main__':
    unittest.main()
