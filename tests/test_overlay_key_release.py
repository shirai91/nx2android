"""Releasing a controller button must not take touch controls away again."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class OverlayKeyRelease(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            raise unittest.SkipTest('a C compiler is needed for the key-event check')
        front = (ROOT / 'nxnative/dm/android/front_android.c').read_text(encoding='utf-8')
        visible = front[front.index('int dm_overlay_visible(void)'):front.index('int dm_overlay_layout(void)')]
        mark = front[front.index('static void pad_mark_used('):front.index('static int handle_event(')]
        # Compile the complete production key-event arm; the motion arm is outside
        # this test. Android accessors and unrelated menu/keyboard calls are stubs.
        keys = front[front.index('static int handle_event('):
                     front.index('    if (atomic_load(&g_tx_on) && ((src & AINPUT_SOURCE_JOYSTICK)')]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>
enum { AINPUT_EVENT_TYPE_KEY = 1, AKEY_EVENT_ACTION_DOWN = 0,
       AKEY_EVENT_ACTION_UP = 1, AINPUT_SOURCE_GAMEPAD = 2,
       AKEYCODE_BACK = 1, AKEYCODE_BUTTON_MODE, AKEYCODE_MENU, TEST_A };
typedef struct { int source, type, key, action, device; } AInputEvent;
#define AInputEvent_getSource(e) ((e)->source)
#define AInputEvent_getType(e) ((e)->type)
#define AInputEvent_getDeviceId(e) ((e)->device)
#define AKeyEvent_getKeyCode(e) ((e)->key)
#define AKeyEvent_getAction(e) ((e)->action)
#define AKeyEvent_getRepeatCount(e) (0)
#define LOG(...) ((void)0)
static _Atomic int g_set_overlay = 1, g_overlay = 1, g_pad_in = -1, g_tx_on, g_menu;
static int g_pad_used, vibration = -1, publications;
static uint64_t g_btn[1];   // per player (PR #238); the device here is player 1's
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { int unused; } g_slots;
static uint32_t pad_slot_mask(const void *s) { (void)s; return 0; }
static int takes = -1;   // what the last pad_player call asked: 1 take a player, 0 not
static int pad_player(int id, int src, int take) { (void)id; (void)src; takes = take; return 0; }
static int dm_player_seen_as(int p) { return p; }   // hle_hid.c: the player the game sees (one player here)
static void dm_haptics_android_device(int d) { vibration = d; }
static int tx_key(AInputEvent *e) { (void)e; return 1; }
static void menu_open(int tab) { (void)tab; }
static void menu_close(void) {}
static void menu_key(uint64_t bit) { (void)bit; }
static uint64_t key_bit(int k) { return k == TEST_A ? 1 : 0; }
static void dm_pad_players(int n) { assert(n == 1); }
static void pad_publish(int p) { assert(p == 0); publications++; }
''' + visible + mark + keys + r'''
    return 0;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    int test = atoi(argv[1]);
    AInputEvent key = {AINPUT_SOURCE_GAMEPAD, AINPUT_EVENT_TYPE_KEY, TEST_A,
                       AKEY_EVENT_ACTION_DOWN, 17};
    if (test == 1) {
        // An idle release, including one from a virtual device, is no new input.
        key.action = AKEY_EVENT_ACTION_UP;
        assert(handle_event(&key) == 1);
        assert(dm_overlay_visible() == 1 && !g_pad_used);
        assert(g_pad_in == -1 && vibration == -1 && g_btn[0] == 0 && publications == 1);
        assert(takes == 0);  // and it takes no player
    } else {
        assert(handle_event(&key) == 1);
        assert(dm_overlay_visible() == 2 && g_pad_used && g_btn[0] == 1);
        assert(g_pad_in == 17 && vibration == 17 && publications == 1 && takes == 1);
        if (test == 2) {
            // The state after touch_event restores the overlay while A is held.
            atomic_store(&g_overlay, 1);
            assert(dm_overlay_visible() == 1);
        }
        key.action = AKEY_EVENT_ACTION_UP;
        assert(handle_event(&key) == 1);
        assert(g_btn[0] == 0 && publications == 2 && takes == 0);  // A still reaches the guest as released.
        assert(dm_overlay_visible() == (test == 2 ? 1 : 2));
        assert(g_pad_in == 17 && vibration == 17);
        key.action = AKEY_EVENT_ACTION_DOWN;
        assert(handle_event(&key) == 1);
        assert(dm_overlay_visible() == 2 && g_btn[0] == 1 && publications == 3);
    }
    return 0;
}
'''
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_key_release_')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        source, cls.exe = tmp / 'keys.c', tmp / 'keys'
        source.write_text(harness, encoding='utf-8')
        result = subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                 str(source), '-pthread', '-o', str(cls.exe)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.exe), str(case)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_idle_key_release_keeps_touch_controls_visible(self):
        self.run_case(1)

    def test_release_after_touch_keeps_controls_visible_and_releases_button(self):
        self.run_case(2)

    def test_release_without_touch_keeps_controller_mode(self):
        self.run_case(3)


if __name__ == '__main__':
    unittest.main()
