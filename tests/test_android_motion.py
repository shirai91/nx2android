"""Phone flicks retain their direction through the real Android and HID sensor code.

Only the Android sensor queue, display rotation and guest clock are simulated;
mapping, automatic shake detection, motion history and six-axis sampling are C
from the production sources. No phone or game is needed.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class AndroidMotion(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            raise unittest.SkipTest('a C compiler is required')
        front = (ROOT / 'nxnative/dm/android/front_android.c').read_text()
        hid = (ROOT / 'nxnative/dm/host/hle_hid.c').read_text()
        shake = hid[hid.index('#define SHAKE_HZ'):hid.index('void dm_input_set(')]
        motion = hid[hid.index('typedef struct { int64_t delta_ns'):hid.index('// SixAxisSensorHandle:')]
        sensor = front[front.index('static void motion_feed('):front.index('static void *input_thread(')]
        harness = r'''
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "dm_input.h"
#define LOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
static uint64_t clock_ns = 1000000000;
static uint64_t guest_ns(void) { return clock_ns; }
static _Atomic uint64_t g_btn, g_pad_btn[DM_PLAYERS];
''' + shake + motion + r'''
enum { ASENSOR_TYPE_ACCELEROMETER = 1, ASENSOR_TYPE_GYROSCOPE = 4 };
#define ASENSOR_STANDARD_GRAVITY 9.80665f
typedef int ASensor, ASensorManager, ASensorEventQueue;
typedef struct {
    int type;
    int64_t timestamp;
    union { struct { float x, y, z; } vector, acceleration; };
} ASensorEvent;
static ASensor device_accel, device_gyro;
static ASensorEventQueue queue;
static ASensorEventQueue *g_sq;
static const ASensor *g_accel, *g_gyro;
static int has_gyro, g_rot;
static void *g_ilooper;
static _Atomic int g_bg;
static uint64_t g_shake_btn;
static ASensorEvent pending;
static int available;
static ASensorManager *ASensorManager_getInstanceForPackage(const char *p) { (void)p; return &queue; }
static const ASensor *ASensorManager_getDefaultSensor(ASensorManager *m, int type) {
    (void)m; return type == ASENSOR_TYPE_ACCELEROMETER ? &device_accel : has_gyro ? &device_gyro : NULL;
}
static ASensorEventQueue *ASensorManager_createEventQueue(ASensorManager *m, void *l, int id,
                                                       int (*cb)(int, int, void *), void *data) {
    (void)m; (void)l; (void)id; (void)cb; (void)data; return &queue;
}
static const char *ASensor_getName(const ASensor *s) { (void)s; return "synthetic"; }
static ssize_t ASensorEventQueue_getEvents(ASensorEventQueue *q, ASensorEvent *out, size_t count) {
    assert(q == &queue && count > 0);
    if (!available) return 0;
    *out = pending; available = 0; clock_ns = pending.timestamp; return 1;
}
static void rotation_start(void) {}
static void rotation_poll(int64_t now) { (void)now; }
static void shake_sensor(int on) { (void)on; }
static void pad_publish(void) { atomic_store(&g_pad_btn[0], g_shake_btn); shake_note(); }
''' + sensor + r'''
static void event(int type, uint64_t time, float x, float y, float z) {
    pending = (ASensorEvent){.type = type, .timestamp = time, .vector = {x, y, z}};
    available = 1; assert(on_sensor(0, 0, NULL) == 1 && !available);
}
static void near(float got, float want) {
    if (fabsf(got - want) > 0.00001f) { fprintf(stderr, "got %g, expected %g\n", got, want); exit(1); }
}
int main(int argc, char **argv) {
    assert(argc == 5);
    has_gyro = atoi(argv[1]); g_rot = atoi(argv[2]);
    float flick = (float)atof(argv[3]); int manual = atoi(argv[4]);
    shake_start();
    const float gravity = ASENSOR_STANDARD_GRAVITY;
    event(ASENSOR_TYPE_ACCELEROMETER, 1000000000, 0, 0, gravity);
    if (g_gyro) event(ASENSOR_TYPE_GYROSCOPE, 1000000000, 0, 0, 0);
    event(ASENSOR_TYPE_ACCELEROMETER, 1025000000, 0, flick * gravity, gravity);
    if (g_gyro) event(ASENSOR_TYPE_GYROSCOPE, 1030000000, 0, (float)M_PI, 0);
    if (manual) { atomic_store(&g_btn, DM_BTN_SHAKE); shake_note(); }
    SixAxisState s;
    six_sample(&s, clock_ns / SIX_PERIOD_NS, 1);
    if (g_gyro && !manual) {
        // Literal screen-frame mapping for a natural-Y flick at each rotation.
        const float ax[] = {0, flick, 0, -flick}, ay[] = {-flick, 0, flick, 0};
        const float gx[] = {0, -0.5f, 0, 0.5f}, gy[] = {0.5f, 0, -0.5f, 0};
        near(s.acc[0], ax[g_rot]); near(s.acc[1], ay[g_rot]); near(s.acc[2], -1);
        near(s.gyro[0], gx[g_rot]); near(s.gyro[1], gy[g_rot]); near(s.gyro[2], 0);
        assert(!g_shake_btn && !shaking(clock_ns) && mo_moving());
    } else if (manual || fabsf(flick) > 1.5f) {
        assert(shaking(clock_ns));
        if (!manual) assert(g_shake_btn == DM_BTN_SHAKE);
        six_sample(&s, (clock_ns + 25000000) / SIX_PERIOD_NS, 1);
        assert(fabsf(s.acc[0]) > 0.1f && fabsf(s.gyro[2]) > 0.1f);
        near(s.acc[1], 0); near(s.gyro[1], 0);
    } else {
        assert(!g_shake_btn && !shaking(clock_ns));
        near(s.acc[0], 0); near(s.acc[1], 0); near(s.acc[2], -1);
    }
    // Player 2 never inherits the phone's motion or explicit player-1 shake.
    six_sample(&s, clock_ns / SIX_PERIOD_NS, 0);
    near(s.acc[0], 0); near(s.acc[1], 0); near(s.acc[2], -1);
    return 0;
}
'''
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_motion_')
        cls.addClassCleanup(cls.tmp.cleanup)
        c = Path(cls.tmp.name) / 'motion.c'
        cls.exe = Path(cls.tmp.name) / 'motion'
        c.write_text(harness)
        result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                 '-I' + str(ROOT / 'nxnative/dm/host'), str(c),
                                 '-lm', '-pthread', '-o', str(cls.exe)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, gyro, rotation, flick, manual=False, motion='1'):
        result = subprocess.run([str(self.exe), str(gyro), str(rotation), str(flick), str(int(manual))],
                                env=dict(os.environ, DM_MOTION=motion, DM_SHAKE_G='1.5', DM_MOTION_LOG='0'),
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_directional_flicks_with_real_motion(self):
        for rotation in range(4):
            for flick in (-3, 3):
                with self.subTest(rotation=rotation, flick=flick):
                    self.run_case(1, rotation, flick)

    def test_accelerometer_only_fallback(self):
        for flick in (-3, 3):
            with self.subTest(flick=flick):
                self.run_case(0, 0, flick)

    def test_motion_disabled_fallback(self):
        self.run_case(1, 0, 3, motion='0')

    def test_below_threshold_is_not_a_shake(self):
        self.run_case(0, 0, 0.5)

    def test_explicit_shake_overrides_real_motion(self):
        self.run_case(1, 0, 0.5, manual=True)


if __name__ == '__main__':
    unittest.main()
