# The frame-pacing policy (nxnative/dm/host/dm_pacing.c, docs/ARCHITECTURE.md Frame pacing): the profile's keys and the switches over them, compiled from the
# real source; and the android.tsv `pacing` presets.
import os, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'tests'))
sys.path.insert(0, str(REPO / 'nxnative/dm/android/tools'))
import csrc              # noqa: E402
import android_profile   # noqa: E402
import settings_registry # noqa: E402

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32  /* MinGW has no setenv; "NAME=" removes the variable */
static int setenv(const char *k, const char *v, int o) { char s[256]; (void)o; snprintf(s, sizeof s, "%s=%s", k, v); return _putenv(s); }
static int unsetenv(const char *k) { char s[256]; snprintf(s, sizeof s, "%s=", k); return _putenv(s); }
#endif
''' + csrc.PACING_INCLUDE + r'''
int main(void) {
    const char *src;
    /* the stub profile: fps_target 0, ceiling 0, hold on, present_interval game */
    assert(dm_pacing_target(&src) == 0 && !strcmp(src, "game profile fps_target"));
    setenv("DM_FPS_TARGET", "60", 1); assert(dm_pacing_target(&src) == 60 && !strcmp(src, "DM_FPS_TARGET"));
    setenv("DM_FPS_TARGET", "45", 1); assert(dm_pacing_target(NULL) == 0);   /* only 0, 30 or 60 */
    unsetenv("DM_FPS_TARGET");
    assert(dm_pacing_ceiling() == 0); setenv("DM_FPS_CEILING", "30", 1); assert(dm_pacing_ceiling() == 30); unsetenv("DM_FPS_CEILING");
    assert(dm_pacing_hold(&src) == 1); setenv("DM_FPS_HOLD", "off", 1); assert(dm_pacing_hold(&src) == 0 && !strcmp(src, "DM_FPS_HOLD"));
    setenv("DM_FPS_HOLD", "on", 1); assert(dm_pacing_hold(NULL) == 1); unsetenv("DM_FPS_HOLD");
    assert(dm_pacing_display(NULL) == 0); setenv("DM_PRESENT_INTERVAL", "display", 1); assert(dm_pacing_display(NULL) == 1);
    setenv("DM_PRESENT_INTERVAL", "game", 1); assert(dm_pacing_display(NULL) == 0); unsetenv("DM_PRESENT_INTERVAL");
    int ex; assert(dm_pacing_adaptive(&ex) == 1 && !ex);
    setenv("DM_ADAPTIVE_FPS", "0", 1); assert(dm_pacing_adaptive(&ex) == 0 && ex);
    setenv("DM_ADAPTIVE_FPS", "1", 1); assert(dm_pacing_adaptive(&ex) == 1 && ex); unsetenv("DM_ADAPTIVE_FPS");
    assert(dm_pacing_pin() == 0);
    setenv("DM_FPS_PIN", "40", 1); assert(dm_pacing_pin() == 40); setenv("DM_FPS_PIN", "50", 1); assert(dm_pacing_pin() == 0); unsetenv("DM_FPS_PIN");
    assert(dm_pacing_up_pct() == 90); setenv("DM_ADAPT_UP_PCT", "50", 1); assert(dm_pacing_up_pct() == 50);
    setenv("DM_ADAPT_UP_PCT", "100", 1); assert(dm_pacing_up_pct() == 90); unsetenv("DM_ADAPT_UP_PCT");
    assert(!dm_pacing_window_game_rate()); setenv("DM_VK_WINDOW_GAME_RATE", "1", 1); assert(dm_pacing_window_game_rate()); unsetenv("DM_VK_WINDOW_GAME_RATE");
    assert(dm_pacing_turbo() == 0); setenv("DM_GPU_TURBO", "auto", 1); assert(dm_pacing_turbo() == 2);
    setenv("DM_GPU_TURBO", "1", 1); assert(dm_pacing_turbo() == 1); unsetenv("DM_GPU_TURBO");
    dm_pacing_log();
    puts("pacing ok"); return 0;
}
'''


class PacingPolicy(unittest.TestCase):
    def test_policy_from_the_real_source(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: self.skipTest('a C compiler is required')
        with tempfile.TemporaryDirectory(prefix='nx2a_pacing_') as tmp:
            csrc.pacing_files(tmp); c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(HARNESS, encoding='utf-8')
            r = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I' + tmp, str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            env = {k: v for k, v in os.environ.items() if not k.startswith('DM_')}
            r = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('pacing ok', r.stdout)
            self.assertIn('pacing: target', r.stderr)

    def test_presets_are_registry_switches_of_profile_scope(self):
        for name, rows in android_profile.PACING.items():
            self.assertFalse(settings_registry.check_env(rows, ('profile',)), name)
        self.assertEqual(android_profile.PACING['adaptive'], [])


if __name__ == '__main__':
    unittest.main()
