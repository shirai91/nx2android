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
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]


class AndroidMotion(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            raise unittest.SkipTest('a C compiler is required')
        front = (ROOT / 'nxnative/dm/android/front_android.c').read_text(encoding='utf-8')
        hid = (ROOT / 'nxnative/dm/host/hle_hid.c').read_text(encoding='utf-8')
        shake = csrc.between(hid, '#define SHAKE_HZ', 'void dm_input_set(')
        motion = csrc.between(hid, 'typedef struct { int64_t delta_ns', '// SixAxisSensorHandle:')
        sensor = csrc.between(front, 'static void motion_feed(', 'static void *input_thread(')
        padfeed = csrc.between(front, 'static void pad_motion_feed(', 'JNIEXPORT void JNICALL Java_org_nx2a_PadMotion_nativeSample')
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
#include "pad_motion_map.h"
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
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // the per-player state's lock (each controller a player of its own)
static void pad_publish(int p) { (void)p; atomic_store(&g_pad_btn[0], g_shake_btn); shake_note(); }
''' + sensor + padfeed + r'''
// Two sources feed one sensor (hle_hid.c): the device's own and a controller's. The newest owns it, and DM_MOTION_SRC
// picks instead. Each case starts from an empty ring and a 1 s guest clock, as the test harness begins.
static void near(float got, float want);
static void pad_case(const char *mode) {
    const float pg[3] = {0, 0, 0.5f}, pa[3] = {0, 0, -1};   // the device turning 0.5 rev/s about z
    const float qg[3] = {-1, 0, 0}, qa[3] = {0, 0, -1};     // the pad turning 1 rev/s the other way about x
    SixAxisState s;
    if (!strcmp(mode, "padonly")) {   // DM_MOTION_SRC=pad: the device's motion is left out
        dm_motion_set(pa, pg, clock_ns);
        six_sample(&s, clock_ns / SIX_PERIOD_NS, 1);
        near(s.gyro[2], 0); near(s.acc[2], -1);
        clock_ns += 10000000; dm_motion_pad_set(qa, qg, clock_ns);
        clock_ns += 10000000; six_sample(&s, clock_ns / SIX_PERIOD_NS, 1);
        near(s.gyro[0], -1); near(s.gyro[2], 0);
        return;
    }
    if (!strcmp(mode, "phoneonly")) {   // DM_MOTION_SRC=phone: the pad's motion is left out
        clock_ns += 10000000; dm_motion_pad_set(qa, qg, clock_ns);
        clock_ns += 10000000; six_sample(&s, clock_ns / SIX_PERIOD_NS, 1);
        near(s.gyro[0], 0);
        clock_ns += 10000000; dm_motion_set(pa, pg, clock_ns);
        clock_ns += 10000000; six_sample(&s, clock_ns / SIX_PERIOD_NS, 1);
        near(s.gyro[2], 0.5f);
        return;
    }
    dm_motion_set(pa, pg, clock_ns);   // auto, nothing else feeding: the device's
    six_sample(&s, clock_ns / SIX_PERIOD_NS, 1);
    near(s.gyro[2], 0.5f); near(s.gyro[0], 0);
    clock_ns += 10000000; dm_motion_pad_set(qa, qg, clock_ns);   // the pad feeds: it owns the sensor at once
    clock_ns += 10000000; dm_motion_set(pa, pg, clock_ns);       // the device's next sample is left out
    six_sample(&s, clock_ns / SIX_PERIOD_NS, 1);
    near(s.gyro[0], -1); near(s.gyro[2], 0);
    clock_ns += 2 * 1000000000ULL; dm_motion_set(pa, pg, clock_ns);   // the pad quiet for over a second: the device's again
    six_sample(&s, clock_ns / SIX_PERIOD_NS, 1);
    near(s.gyro[2], 0.5f); near(s.gyro[0], 0);
}
// The pad's own frame (pad_motion_map.h): a spec that is a signed permutation is taken, anything else leaves Android's frame as it is, and the raw sample goes through the
// map before the host's units (rev/s, G with gravity's sign flipped) - the order front_android.c's pad_motion_feed uses
static void padmap_case(void) {
    dm_pad_map m = dm_pad_map_identity();
    const char *spec = getenv("DM_PAD_MOTION_MAP"), *want = getenv("DM_PAD_MOTION_MAP_TAKES");
    int took = dm_pad_map_parse(spec, &m);
    if (want && atoi(want) != took) { fprintf(stderr, "spec %s: taken %d, expected %s\n", spec ? spec : "(none)", took, want); exit(1); }
    // a pad turning 1 rev/s about its own +y, lying so gravity is along its own -y: identity reads that on y, "-y,+x,+z" (a Flydigi Direwolf 4 in Switch mode) on x and
    // reversed there
    pad_motion_feed(&m, 7, 0, (float)(2 * M_PI), 0, 0, (float)-ASENSOR_STANDARD_GRAVITY, 0, clock_ns);
    SixAxisState s;
    six_sample(&s, clock_ns / SIX_PERIOD_NS, 1);
    float eg[3] = {0, 1, 0}, ea[3] = {0, 1, 0};
    if (took) { eg[0] = -1; eg[1] = 0; ea[0] = -1; ea[1] = 0; }
    near(s.gyro[2], 0); near(s.acc[2], 0);
    near(s.gyro[0], eg[0]); near(s.gyro[1], eg[1]);
    near(s.acc[0], ea[0]); near(s.acc[1], ea[1]);
}
static void event(int type, uint64_t time, float x, float y, float z) {
    pending = (ASensorEvent){.type = type, .timestamp = time, .vector = {x, y, z}};
    available = 1; assert(on_sensor(0, 0, NULL) == 1 && !available);
}
static void near(float got, float want) {
    if (fabsf(got - want) > 0.00001f) { fprintf(stderr, "got %g, expected %g\n", got, want); exit(1); }
}
int main(int argc, char **argv) {
    assert(argc == 5 || argc == 6);
    has_gyro = atoi(argv[1]); g_rot = atoi(argv[2]);
    if (argc == 6) { if (!strcmp(argv[5], "padmap")) padmap_case(); else pad_case(argv[5]); return 0; }
    float flick = (float)atof(argv[3]); int manual = atoi(argv[4]);
    shake_start();
    const char *motion_env = getenv("DM_MOTION");
    int expected_gyro = has_gyro && !(motion_env && !atoi(motion_env));
    assert((g_gyro != NULL) == expected_gyro);
    const float gravity = ASENSOR_STANDARD_GRAVITY;
    event(ASENSOR_TYPE_ACCELEROMETER, 1000000000, 0, 0, gravity);
    if (g_gyro) event(ASENSOR_TYPE_GYROSCOPE, 1000000000, 0, 0, 0);
    event(ASENSOR_TYPE_ACCELEROMETER, 1025000000, 0, flick * gravity, gravity);
    if (g_gyro) event(ASENSOR_TYPE_GYROSCOPE, 1030000000, 0, (float)M_PI, 0);
    if (manual) { atomic_store(&g_btn, DM_BTN_SHAKE); shake_note(); }
    SixAxisState s;
    six_sample(&s, clock_ns / SIX_PERIOD_NS, 1);
    const char *raw = getenv("DM_MOTION_RAW");
    int raw_motion = raw && atoi(raw) > 0;
    if (g_gyro && !manual && (raw_motion || fabsf(flick) <= 1.5f)) {
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
        c.write_text(harness, encoding='utf-8')
        result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                 '-I' + str(ROOT / 'nxnative/dm/host'), '-I' + str(ROOT / 'nxnative/dm/android'), str(c),
                                 '-lm', '-pthread', '-o', str(cls.exe)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, gyro, rotation, flick, manual=False, motion='1', raw=None):
        env = dict(os.environ, DM_MOTION=motion, DM_SHAKE_G='1.5', DM_MOTION_LOG='0')
        env.pop('DM_MOTION_RAW', None)
        if raw is not None:
            env['DM_MOTION_RAW'] = raw
        result = subprocess.run([str(self.exe), str(gyro), str(rotation), str(flick), str(int(manual))],
                                env=env,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def run_pad(self, mode, src=None):
        env = dict(os.environ, DM_MOTION='1', DM_SHAKE_G='1.5', DM_MOTION_LOG='0')
        env.pop('DM_MOTION_RAW', None); env.pop('DM_MOTION_SRC', None)
        if src is not None:
            env['DM_MOTION_SRC'] = src
        result = subprocess.run([str(self.exe), '1', '0', '0.5', '0', mode], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_controller_motion_owns_the_sensor_while_it_feeds(self):
        for src in (None, 'auto'):
            with self.subTest(src=src):
                self.run_pad('pad', src)

    def test_controller_only_source(self):
        self.run_pad('padonly', 'pad')

    def test_phone_only_source(self):
        self.run_pad('phoneonly', 'phone')

    def run_padmap(self, spec, takes):
        env = dict(os.environ, DM_MOTION='1', DM_SHAKE_G='1.5', DM_MOTION_LOG='0', DM_PAD_MOTION_MAP_TAKES=str(takes))
        env.pop('DM_PAD_MOTION_MAP', None); env.pop('DM_MOTION_SRC', None); env.pop('DM_MOTION_RAW', None)
        if spec is not None:
            env['DM_PAD_MOTION_MAP'] = spec
        result = subprocess.run([str(self.exe), '1', '0', '0.5', '0', 'padmap'], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, f'{spec!r}: ' + result.stdout + result.stderr)

    def test_pad_axis_map(self):
        # taken: the measured Flydigi Direwolf 4 spec; refused (identity): empty, a repeated axis, two axes, a fourth token
        for spec, takes in ((None, 0), ('', 0), ('-y,+x,+z', 1), (' -y, +x, +z', 1),
                            ('x,x,z', 0), ('-y,+x', 0), ('-y,+x,+z,w', 0)):
            with self.subTest(spec=spec):
                self.run_padmap(spec, takes)

    def test_directional_flicks_with_real_motion(self):
        for rotation in range(4):
            for flick in (-3, 3):
                with self.subTest(rotation=rotation, flick=flick):
                    self.run_case(1, rotation, flick, raw='1')

    def test_default_gyro_shake_is_preserved(self):
        for rotation in range(4):
            for flick in (-3, 3):
                with self.subTest(rotation=rotation, flick=flick):
                    self.run_case(1, rotation, flick)

    def test_raw_motion_disabled_values_keep_shake(self):
        for raw in ('0', '-1', '', 'invalid'):
            with self.subTest(raw=raw):
                self.run_case(1, 0, 3, raw=raw)

    def test_below_threshold_gyro_motion_is_unchanged(self):
        for raw in (None, '1'):
            for rotation in range(4):
                with self.subTest(raw=raw, rotation=rotation):
                    self.run_case(1, rotation, 0.5, raw=raw)

    def test_opt_in_without_motion_keeps_shake(self):
        self.run_case(0, 0, 3, raw='1')
        self.run_case(1, 0, 3, motion='0', raw='1')

    def test_accelerometer_only_fallback(self):
        for flick in (-3, 3):
            with self.subTest(flick=flick):
                self.run_case(0, 0, flick)

    def test_motion_disabled_fallback(self):
        self.run_case(1, 0, 3, motion='0')

    def test_below_threshold_is_not_a_shake(self):
        self.run_case(0, 0, 0.5)

    def test_explicit_shake_overrides_real_motion(self):
        for raw in (None, '1'):
            with self.subTest(raw=raw):
                self.run_case(1, 0, 0.5, manual=True, raw=raw)


if __name__ == '__main__':
    unittest.main()
