"""The six-axis sensor's direction follows gravity (hle_hid.c mo_set's sensor fusion) through the production C.

Breath of the Wild aims its Sheikah Slate viewfinder by the direction. Integrated from the gyroscope alone it wandered from
the device's attitude, and a tilt up then moved the view diagonally. Fed samples go through dm_motion_set and are read back
with six_sample, as the game's GetSixAxisSensorStates reads them. No device or game is needed.
"""
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SixAxisFusion(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            raise unittest.SkipTest('a C compiler is required')
        hid = (ROOT / 'nxnative/dm/host/hle_hid.c').read_text()
        shake = hid[hid.index('#define SHAKE_HZ'):hid.index('void dm_input_set(')]
        motion = hid[hid.index('typedef struct { int64_t delta_ns'):hid.index('// SixAxisSensorHandle:')]
        harness = r'''
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"
#define LOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
static uint64_t clock_ns = 1000000000;
static uint64_t guest_ns(void) { return clock_ns; }
static _Atomic uint64_t g_btn, g_pad_btn[DM_PLAYERS];
''' + shake + motion + r'''
static float acc[3];   /* gravity's direction in the controller's frame, turned with it (a fixed world vector: dv/dt = v x w) */
static void feed(double gx, double gy, double gz, double seconds) {   /* revolutions / s, at 200 Hz */
    for (int i = 0, n = (int)(seconds * 200); i < n; i++) {
        float g[3] = {(float)gx, (float)gy, (float)gz};
        clock_ns += 5000000; dm_motion_set(acc, g, (int64_t)clock_ns);
        double w0 = 2 * M_PI * gx * 0.005, w1 = 2 * M_PI * gy * 0.005, w2 = 2 * M_PI * gz * 0.005;
        float n0 = acc[0] + (float)(acc[1] * w2 - acc[2] * w1), n1 = acc[1] + (float)(acc[2] * w0 - acc[0] * w2), n2 = acc[2] + (float)(acc[0] * w1 - acc[1] * w0);
        float m = sqrtf(n0 * n0 + n1 * n1 + n2 * n2); acc[0] = n0 / m; acc[1] = n1 / m; acc[2] = n2 / m;
    }
}
static double down_error(void) {   /* degrees between the direction's down, -(dir[2], dir[5], dir[8]), and the gravity fed */
    SixAxisState s; six_sample(&s, (int64_t)(clock_ns / SIX_PERIOD_NS), 1);
    double d[3] = {-s.dir[2], -s.dir[5], -s.dir[8]}, dot = d[0] * acc[0] + d[1] * acc[1] + d[2] * acc[2];
    return acos(fmax(-1, fmin(1, dot))) * 180 / M_PI;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    (void)shake_note; (void)mo_moving;   /* production code this harness does not call */
    SixAxisState s;
    if (!strcmp(argv[1], "tilt")) {   /* the device pitched up 30 degrees from the first sample: the direction starts lying flat and must come to it */
        acc[0] = 0; acc[1] = (float)-sin(M_PI / 6); acc[2] = (float)-cos(M_PI / 6);
        feed(0, 0, 0, 0.5); printf("%.3f\n", down_error()); return 0;
    }
    acc[0] = 0; acc[1] = 0; acc[2] = -1; feed(0, 0, 0, 0.5);
    if (!strcmp(argv[1], "pitch")) { feed(0.1, 0, 0, 1); printf("%.3f\n", down_error()); return 0; }   /* 36 degrees up, gravity turning with it */
    if (!strcmp(argv[1], "yaw")) {   /* 90 degrees about the vertical: the controller's x axis then points along world y */
        feed(0, 0, 0.25, 1); six_sample(&s, (int64_t)(clock_ns / SIX_PERIOD_NS), 1);
        printf("%.3f %.3f %.3f\n", s.dir[0], s.dir[1], s.dir[2]); return 0;
    }
    return 2;
}
'''
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_fusion_')
        cls.addClassCleanup(cls.tmp.cleanup)
        c = Path(cls.tmp.name) / 'fusion.c'
        cls.exe = Path(cls.tmp.name) / 'fusion'
        c.write_text(harness)
        result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'nxnative/dm/host'), str(c),
                                 '-lm', '-pthread', '-o', str(cls.exe)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.exe), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return [float(v) for v in result.stdout.split()]

    def test_direction_comes_to_gravity(self):
        (err,) = self.run_case('tilt')
        self.assertLess(err, 1.0, 'the direction kept lying flat while gravity said 30 degrees up')

    def test_consistent_pitch_is_not_fought(self):
        (err,) = self.run_case('pitch')
        self.assertLess(err, 1.0, 'a real pitch (gravity turning with it) left the direction off gravity')

    def test_yaw_stays_free(self):
        x = self.run_case('yaw')
        self.assertAlmostEqual(math.hypot(x[0], x[2]), 0, delta=0.05, msg=f'controller x axis {x}')
        self.assertAlmostEqual(abs(x[1]), 1, delta=0.05, msg=f'controller x axis {x}')


if __name__ == '__main__':
    unittest.main()
