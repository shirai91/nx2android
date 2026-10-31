"""Exercise the Android adaptive controller's recovery using its actual C code."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/perf_android.c'


class AdaptiveFrameRate(unittest.TestCase):
    def test_downshift_veto_allows_median_recovery(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is required')
        src = SOURCE.read_text(encoding='utf-8')
        controller = src[src.index('static const int64_t STEPS[]'):src.index('// hle_nvn.c, the first acquire')]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#ifdef _WIN32  // MinGW (the Windows release runner) has no setenv; "NAME=" removes the variable
static int setenv(const char *k, const char *v, int o) { char s[256]; (void)o; snprintf(s, sizeof s, "%s=%s", k, v); return _putenv(s); }
static int unsetenv(const char *k) { char s[256]; snprintf(s, sizeof s, "%s=", k); return _putenv(s); }
#endif
''' + controller + r'''
static int64_t mixed_work(int i) { return i % 64 < 48 ? 15000000 : 27000000; }
int main(int argc, char **argv) {
    (void)argv;
    int median = argc > 1;
    unsetenv("DM_FPS_PIN"); unsetenv("DM_ADAPTIVE_FPS");
    if (median) setenv("DM_ADAPT_UP_PCT", "50", 1); else unsetenv("DM_ADAPT_UP_PCT");
    assert(dm_present_period_adapt(16666667) == 16666667);
    g_step = 1; g_last_change = 1000000000ll; g_up_hold = 20000000000ll;
    g_nwork = 64; g_nshown = 64;
    for (int i = 0; i < 64; i++) g_work_hist[i] = mixed_work(i);
    assert(work_pct(50) == 15000000 && work_pct(90) == 27000000);
    adapt_frame(mixed_work(0), 2000000000ll);  // four-second settling hold
    assert(g_step == 1 && g_ok_frames == 0);
    for (int i = 1; i <= 121; i++) adapt_frame(mixed_work(i), 6000000000ll + i * 25000000ll);
    assert(g_step == 1);  // the longer recovery hold still applies
    assert(g_held_back == 121);
    assert(g_ok_frames == (median ? 121 : 0));
    adapt_frame(mixed_work(122), 21000000000ll);
    assert(g_step == (median ? 0 : 1));  // default p90 policy stays at 40

    g_step = 0; g_last_change = g_last_up = 21000000000ll;
    g_nwork = 64; g_nshown = 64;
    for (int i = 0; i < 64; i++) { g_work_hist[i] = 19000000; g_late[i] = i < 7; }
    adapt_frame(19000000, 24000000000ll);
    assert(g_step == 0);  // no early downshift within four seconds
    adapt_frame(19000000, 26000000000ll);
    assert(g_step == 1 && g_last_change == 26000000000ll);
    assert(g_up_hold == 40000000000ll);  // a failed recovery doubles the hold
    assert(g_nwork == 0 && g_nshown == 0 && g_ok_frames == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_adaptive_') as tmp:
            c = Path(tmp) / 'check.c'
            exe = Path(tmp) / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for args in ([], ['median']):
                with self.subTest(policy=args or 'default'):
                    result = subprocess.run([str(exe), *args], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
