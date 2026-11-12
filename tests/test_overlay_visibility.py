"""Exercise the real Android input dispatch, visibility, menu action and hit testing."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.upstream import android_profile


ROOT = Path(__file__).resolve().parents[1]


class OverlayVisibility(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            raise unittest.SkipTest('a C compiler is needed for the overlay check')
        front = (ROOT / 'nxnative/dm/android/front_android.c').read_text(encoding='utf-8')
        pad = front[front.index('static int32_t axis('):front.index('// ---- the on-screen gamepad')]
        visible = front[front.index('int dm_overlay_visible(void)'):front.index('int dm_overlay_layout(void)')]
        hit = front[front.index('static int ctl_at('):front.index('// dragging the menu button:')]
        menu = front[front.index('static void menu_do('):front.index('static void menu_touch(')]
        events = front[front.index('static void touch_event(AInputEvent *e) {'):front.index('static int on_input(')]
        harness = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "front.h"
#include "overlay_layout.h"
static _Atomic int g_set_overlay, g_overlay, g_pads, g_edit;
static _Atomic int g_set_rumble, g_set_fps, g_set_labels, g_menu_tab, g_sv_st;
static int layout, saves;
int dm_overlay_layout(void) { return layout; }
float dm_overlay_scale(void) { return 1; }
int dm_overlay_custom(int l, int c, float *x, float *y, float *s) {
    (void)l; (void)c; (void)x; (void)y; *s = 1; return 0;
}
static void settings_save(void) { saves++; }
static void dm_haptics_android_set(int on) { (void)on; }
static void dm_haptics_android_tap(void) {}
static int dm_opmode(void) { return 0; }
static void dm_set_opmode(int mode) { (void)mode; }
static int pad_labels(void) { return 0; }
static void menu_tab(int tab) { (void)tab; }
static void ed_open(void) {}
static void sp_open(void) {}
static void save_export(void) {}
static void menu_close(void) {}
static void dm_overlay_redraw(void) {}
''' + visible + hit + menu + r'''
// Synthetic Android events. The production dispatch and touch handlers below consume them.
enum { AINPUT_SOURCE_TOUCHSCREEN = 1, AINPUT_SOURCE_GAMEPAD = 2, AINPUT_SOURCE_JOYSTICK = 4 };
enum { AINPUT_EVENT_TYPE_KEY = 1, AINPUT_EVENT_TYPE_MOTION };
enum { AKEYCODE_BACK = 1, AKEYCODE_BUTTON_MODE, AKEYCODE_MENU, TEST_PAD_KEY };
enum { AKEY_EVENT_ACTION_DOWN, AKEY_EVENT_ACTION_UP };
enum { AMOTION_EVENT_ACTION_DOWN, AMOTION_EVENT_ACTION_UP, AMOTION_EVENT_ACTION_CANCEL,
       AMOTION_EVENT_ACTION_POINTER_DOWN, AMOTION_EVENT_ACTION_POINTER_UP };
enum { AMOTION_EVENT_ACTION_MASK = 255, AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT = 8 };
enum { AMOTION_EVENT_AXIS_X, AMOTION_EVENT_AXIS_Y, AMOTION_EVENT_AXIS_Z, AMOTION_EVENT_AXIS_RZ,
       AMOTION_EVENT_AXIS_LTRIGGER, AMOTION_EVENT_AXIS_RTRIGGER, AMOTION_EVENT_AXIS_HAT_X,
       AMOTION_EVENT_AXIS_HAT_Y, AMOTION_EVENT_AXIS_BRAKE, AMOTION_EVENT_AXIS_GAS };
typedef struct { int source, type, action, key, device; float x, y, axis[10]; } AInputEvent;
typedef int ANativeWindow;
static int width, height, window, vibration = -1, touches, menu_opens, pad_publications, g_pad_used;
static uint64_t g_btn, g_touch_btn, touch_buttons;
static float g_ax[8];
static int32_t pad_sticks[4];
static uint64_t pad_buttons;
static int g_ptr_ctl[32];
static struct { int id, moved; } g_drag = {-1, 0};
static _Atomic int g_menu, g_sp_on, g_tx_on, g_pad_in, g_knob[2][2];
static _Atomic int64_t g_menu_t, g_press_t[16], g_stick_t[2];
static struct { int sel; } g_tx;
static int g_tx_mu;
#define pthread_mutex_lock(m) ((void)(m))
#define pthread_mutex_unlock(m) ((void)(m))
#define LOG(...) ((void)0)
#define AInputEvent_getSource(e) ((e)->source)
#define AInputEvent_getType(e) ((e)->type)
#define AInputEvent_getDeviceId(e) ((e)->device)
#define AKeyEvent_getKeyCode(e) ((e)->key)
#define AKeyEvent_getAction(e) ((e)->action)
#define AKeyEvent_getRepeatCount(e) (0)
#define AMotionEvent_getAction(e) ((e)->action)
#define AMotionEvent_getPointerCount(e) (1u)
#define AMotionEvent_getPointerId(e, i) ((void)(i), 0)
#define AMotionEvent_getX(e, i) ((void)(i), (e)->x)
#define AMotionEvent_getY(e, i) ((void)(i), (e)->y)
#define AMotionEvent_getAxisValue(e, a, i) ((void)(i), (e)->axis[a])
#define ANativeWindow_getWidth(w) ((void)(w), width)
#define ANativeWindow_getHeight(w) ((void)(w), height)
#define menu_grab(ptr, ctl, x, y, W, H) ((void)(ctl), (void)(x), (void)(y), (void)(W), (void)(H), g_drag.id = (ptr))
#define menu_drag(ctl, x, y, W, H) ((void)(ctl), (void)(x), (void)(y), (void)(W), (void)(H))
#define ed_touch(e, W, H) ((void)(e), (void)(W), (void)(H))
#define sp_touch(e, W, H) ((void)(e), (void)(W), (void)(H))
#define menu_touch(e, W, H) ((void)(e), (void)(W), (void)(H))
#define tx_touch(e, W, H) ((void)(e), (void)(W), (void)(H))
static ANativeWindow *phone_window(void) { return &window; }
static int64_t mono_ns(void) { return 1; }
static void dm_haptics_android_device(int id) { vibration = id; }
static void menu_open(int tab) { (void)tab; menu_opens++; }
static void menu_key(uint64_t bit) { (void)bit; }
static uint64_t key_bit(int code) { return code == TEST_PAD_KEY ? DM_BTN_A : 0; }
static int tx_key(AInputEvent *e) { (void)e; return 1; }
static void tx_changed(void) {}
void dm_pad_players(uint32_t n) { assert(n == 1); }
void dm_pad_set(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    pad_buttons = b;
    pad_sticks[0] = lx; pad_sticks[1] = ly; pad_sticks[2] = rx; pad_sticks[3] = ry;
    pad_publications++;
}
void dm_input_set(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    touch_buttons = b; (void)lx; (void)ly; (void)rx; (void)ry;
}
void dm_touch_set(const dm_touch *points, int n) { (void)points; touches = n; }
static void (*dm_picture_rect)(float r[4]);
''' + pad + events + r'''
static void controls(int shown);
static void input_handoff(int test) {
    width = layout ? 2400 : 1920; height = 1080;
    atomic_store(&g_set_overlay, test != 6);
    atomic_store(&g_overlay, 1);
    atomic_store(&g_pads, test == 5 ? 0 : 1);
    AInputEvent pad = {.source = AINPUT_SOURCE_GAMEPAD, .type = AINPUT_EVENT_TYPE_KEY,
                       .key = TEST_PAD_KEY, .action = AKEY_EVENT_ACTION_DOWN, .device = 42};
    int before = pad_publications;
    assert(handle_event(&pad) == 1);
    assert(g_btn & DM_BTN_A);
    assert(pad_publications == before + 1 && vibration == 42);
    assert(dm_overlay_visible() == 2); controls(0);
    AInputEvent tap = {.source = AINPUT_SOURCE_TOUCHSCREEN, .type = AINPUT_EVENT_TYPE_MOTION,
                       .action = AMOTION_EVENT_ACTION_DOWN, .x = width * 0.5f, .y = height * 0.5f};
    assert(handle_event(&tap) == 1);
    assert(touches == 1 && touch_buttons == 0);
    assert(vibration == (test == 5 ? -1 : 42));
    assert(dm_overlay_visible() == (test == 6 ? 2 : 1)); controls(test != 6);
    pad = (AInputEvent){.source = AINPUT_SOURCE_JOYSTICK, .type = AINPUT_EVENT_TYPE_MOTION, .device = 42};
    pad.axis[AMOTION_EVENT_AXIS_X] = 0.7f;
    assert(handle_event(&pad) == 1);
    assert(g_ax[0] == 0.7f && pad_publications == before + 2 && vibration == 42);
    assert(dm_overlay_visible() == 2); controls(0);
    for (int i = 0; i < OV_N; i++) if (OV_CTLS[i].kind == OV_MENU) {
        float b[4]; ov_box(&OV_CTLS[i], width, height, b);
        tap.x = b[0]; tap.y = b[1];
        before = menu_opens;
        assert(handle_event(&tap) == 1 && touches == 0);
        tap.action = AMOTION_EVENT_ACTION_UP;
        assert(handle_event(&tap) == 1 && menu_opens == before + 1);
    }
}

static void motion_handoff(void) {
    width = layout ? 2400 : 1920; height = 1080;
    atomic_store(&g_set_overlay, 1);
    atomic_store(&g_overlay, 1);
    atomic_store(&g_pads, 0);
    atomic_store(&g_pad_in, -1);
    g_pad_used = 0; g_btn = 0; vibration = -1;
    AInputEvent idle = {.source = AINPUT_SOURCE_JOYSTICK, .type = AINPUT_EVENT_TYPE_MOTION, .device = 43};
    assert(handle_event(&idle) == 1);
    assert(dm_overlay_visible() == 1 && !g_pad_used && atomic_load(&g_pad_in) == -1 && vibration == -1);
    const struct { int code; float active, idle; int stick; uint64_t button; } cases[] = {
        {AMOTION_EVENT_AXIS_X,         0.12f,  0.119f,  0, 0},
        {AMOTION_EVENT_AXIS_Y,        -0.12f, -0.119f,  1, 0},
        {AMOTION_EVENT_AXIS_Z,         0.12f,  0.119f,  2, 0},
        {AMOTION_EVENT_AXIS_RZ,       -0.12f, -0.119f,  3, 0},
        {AMOTION_EVENT_AXIS_LTRIGGER,  0.51f,  0.5f,   -1, DM_BTN_ZL},
        {AMOTION_EVENT_AXIS_RTRIGGER,  0.51f,  0.5f,   -1, DM_BTN_ZR},
        {AMOTION_EVENT_AXIS_HAT_X,    -0.51f, -0.5f,   -1, DM_BTN_LEFT},
        {AMOTION_EVENT_AXIS_HAT_Y,     0.51f,  0.5f,   -1, DM_BTN_DOWN},
        {AMOTION_EVENT_AXIS_BRAKE,     0.51f,  0.5f,   -1, DM_BTN_ZL},
        {AMOTION_EVENT_AXIS_GAS,       0.51f,  0.5f,   -1, DM_BTN_ZR},
    };
    float a[4] = {0};
    for (int i = 0; i < OV_N; i++) if (OV_CTLS[i].btn == DM_BTN_A) ov_box(&OV_CTLS[i], width, height, a);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int code = cases[i].code, stick = cases[i].stick;
        AInputEvent pad = {.source = AINPUT_SOURCE_JOYSTICK, .type = AINPUT_EVENT_TYPE_MOTION, .device = 42};
        pad.axis[code] = cases[i].active;
        assert(handle_event(&pad) == 1);
        assert(dm_overlay_visible() == 2 && g_pad_used && atomic_load(&g_pad_in) == 42 && vibration == 42);
        assert(pad_buttons == cases[i].button);
        if (stick >= 0) assert(pad_sticks[stick] == (int32_t)(0.12f * DM_STICK_MAX));

        atomic_store(&g_pads, 1);
        AInputEvent tap = {.source = AINPUT_SOURCE_TOUCHSCREEN, .type = AINPUT_EVENT_TYPE_MOTION,
                           .action = AMOTION_EVENT_ACTION_DOWN, .x = width * 0.5f, .y = height * 0.5f};
        assert(handle_event(&tap) == 1 && dm_overlay_visible() == 1);
        tap.action = AMOTION_EVENT_ACTION_UP;
        assert(handle_event(&tap) == 1);

        int before = pad_publications;
        pad.axis[code] = cases[i].idle;
        assert(handle_event(&pad) == 1);
        assert(handle_event(&idle) == 1);
        assert(pad_publications == before + 2 && !pad_buttons);
        for (int j = 0; j < 4; j++) assert(pad_sticks[j] == 0);
        assert(dm_overlay_visible() == 1 && atomic_load(&g_pad_in) == 42 && vibration == 42);

        tap.x = a[0]; tap.y = a[1]; tap.action = AMOTION_EVENT_ACTION_DOWN;
        assert(handle_event(&tap) == 1 && touch_buttons == DM_BTN_A && touches == 0);
        tap.action = AMOTION_EVENT_ACTION_UP;
        assert(handle_event(&tap) == 1 && !touch_buttons);
    }
}

static void controls(int shown) {
    float W = layout ? 2400 : 1920, H = 1080;
    int button = -1, menu = -1;
    for (int i = 0; i < OV_N; i++) {
        if (OV_CTLS[i].btn == DM_BTN_A) button = i;
        if (OV_CTLS[i].kind == OV_MENU) menu = i;
    }
    assert(button >= 0 && menu >= 0);
    float b[4], m[4];
    ov_box(&OV_CTLS[button], W, H, b); ov_box(&OV_CTLS[menu], W, H, m);
    assert(ctl_at(b[0], b[1], W, H) == (shown ? button : -1));
    assert(ctl_at(m[0], m[1], W, H) == menu);
    assert(ctl_at(W * 0.5f, H * 0.5f, W, H) == -1);
}
int main(int argc, char **argv) {
    assert(argc == 2); int test = atoi(argv[1]);
    if (test == 7) { for (layout = 0; layout < 2; layout++) motion_handoff(); return 0; }
    if (test >= 4) { for (layout = 0; layout < 2; layout++) input_handoff(test); return 0; }
    for (layout = 0; layout < 2; layout++) for (int pads = 0; pads < 3; pads++) for (int touch = 0; touch < 2; touch++) {
        atomic_store(&g_pads, pads); atomic_store(&g_overlay, touch);
        atomic_store(&g_edit, 0);
        if (test == 1) {
            // A connected or used pad does not override the current input mode.
            atomic_store(&g_set_overlay, 1);
            assert(dm_overlay_visible() == (touch ? 1 : 2)); controls(touch);
        } else if (test == 2) {
            // Off leaves a reachable menu and sends touches elsewhere to the game.
            atomic_store(&g_set_overlay, 0);
            assert(dm_overlay_visible() == 2); controls(0);
            atomic_store(&g_edit, 1); controls(1);
        } else {
            // The actual menu action enables controls even with a used pad present,
            // and persists both choices through the existing settings-save path.
            atomic_store(&g_set_overlay, 0); saves = 0;
            menu_do(OVM_OVERLAY);
            assert(saves == 1 && dm_overlay_visible() == 1); controls(1);
            menu_do(OVM_OVERLAY);
            assert(saves == 2 && dm_overlay_visible() == 2); controls(0);
        }
    }
    return 0;
}
'''
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_visibility_')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        (tmp / 'android_profile.h').write_text(android_profile.header(None), encoding='utf-8')
        c, cls.exe = tmp / 'visibility.c', tmp / 'visibility'
        c.write_text(harness, encoding='utf-8')
        result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                 f'-I{tmp}', f'-I{ROOT / "nxnative/dm/android"}',
                                 f'-I{ROOT / "nxnative/dm/host"}', str(c), '-lm', '-o', str(cls.exe)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.exe), str(case)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_visibility_follows_last_input_when_enabled(self):
        self.run_case(1)

    def test_hidden_controls_keep_menu_accessible(self):
        self.run_case(2)

    def test_menu_toggle_with_active_controller(self):
        self.run_case(3)

    def test_touch_restores_controls_with_controller_in_use(self):
        self.run_case(4)

    def test_touch_restores_phone_vibration_without_used_controller(self):
        self.run_case(5)

    def test_show_gamepad_off_keeps_menu_accessible_through_input_handoff(self):
        self.run_case(6)

    def test_idle_controller_motion_leaves_touch_controls_usable(self):
        self.run_case(7)


if __name__ == '__main__':
    unittest.main()
