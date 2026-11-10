"""Exercise the real visibility decision, menu action and control hit testing."""
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
        front = (ROOT / 'nxnative/dm/android/front_android.c').read_text()
        visible = front[front.index('int dm_overlay_visible(void)'):front.index('int dm_overlay_layout(void)')]
        hit = front[front.index('static int ctl_at('):front.index('// dragging the menu button:')]
        menu = front[front.index('static void menu_do('):front.index('static void menu_touch(')]
        harness = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
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
    for (layout = 0; layout < 2; layout++) for (int pads = 0; pads < 3; pads++) for (int touch = 0; touch < 2; touch++) {
        atomic_store(&g_pads, pads); atomic_store(&g_overlay, touch);
        atomic_store(&g_edit, 0);
        if (test == 1) {
            // Saved Show gamepad=on survives controller use/detection, including
            // a phantom Android gamepad. Drawing and touch hit testing agree.
            atomic_store(&g_set_overlay, 1);
            assert(dm_overlay_visible() == 1); controls(1);
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

    def test_show_overrides_controller_detection(self):
        self.run_case(1)

    def test_hidden_controls_keep_menu_accessible(self):
        self.run_case(2)

    def test_menu_toggle_with_active_controller(self):
        self.run_case(3)


if __name__ == '__main__':
    unittest.main()
