"""Check the Android layout editor's geometry without a device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.upstream import android_profile


DM = Path(__file__).resolve().parents[1] / 'nxnative/dm'


class OverlayLayout(unittest.TestCase):
    def test_grid_and_toolbar(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the layout geometry check')
        harness = r'''
#include <assert.h>
#include "overlay_layout.h"
static int custom_faces, custom_a, custom_x;
float dm_overlay_scale(void) { return 1; }
int dm_overlay_custom(int layout, int ctl, float *x, float *y, float *s) {
    *s = 1;
    if (layout == 1 && custom_faces && (ctl == custom_a || ctl == custom_x)) {
        *s = custom_faces == 2 ? 0.5f : 1.2f;
        *x = ctl == custom_a ? 0.70f : (custom_faces == 3 ? 0.40f : custom_faces == 2 ? 0.675f : 0.65f);
        *y = 0.45f; return 1;
    }
    if (layout != 1 || ctl != 0) return 0;
    *x = 0.1125f; *y = 0.55f; *s = 0.705f; return 1;
}
int main(void) {
    assert(ov_layout_for_size(2400, 1080) == 1);
    assert(ov_layout_for_size(1920, 1080) == 0);
    assert(ov_layout_for_size(0, 0) == 0);
    const ov_ctl *phone_layout = OV_LAYOUTS[1];
    int ia = -1, ix = -1, ib = -1, iy = -1;
    for (int i = 0; i < OV_N; i++) {
        if (phone_layout[i].btn == DM_BTN_A) ia = i;
        if (phone_layout[i].btn == DM_BTN_X) ix = i;
        if (phone_layout[i].btn == DM_BTN_B) ib = i;
        if (phone_layout[i].btn == DM_BTN_Y) iy = i;
    }
    assert(ia >= 0 && ix >= 0 && ib >= 0 && iy >= 0);
    float a[4], x[4], b[4], y[4];
    ov_box(&phone_layout[ia], 2400, 1080, a); ov_box(&phone_layout[ix], 2400, 1080, x);
    ov_box(&phone_layout[ib], 2400, 1080, b); ov_box(&phone_layout[iy], 2400, 1080, y);
    assert(!ov_face_combo(phone_layout, a[0], a[1], 2400, 1080));
    assert(!ov_face_combo(phone_layout, x[0], x[1], 2400, 1080));
    assert(ov_face_combo(phone_layout, (a[0] + x[0]) / 2, (a[1] + x[1]) / 2, 2400, 1080) == (DM_BTN_A | DM_BTN_X));
    assert(ov_button_bits(phone_layout, &phone_layout[ix], x[0], x[1], 2400, 1080) == DM_BTN_X);
    assert(ov_button_bits(phone_layout, &phone_layout[ix], (a[0] + x[0]) / 2, (a[1] + x[1]) / 2, 2400, 1080) == (DM_BTN_A | DM_BTN_X));
    assert(ov_button_bits(phone_layout, &phone_layout[ib], (a[0] + x[0]) / 2, (a[1] + x[1]) / 2, 2400, 1080) == DM_BTN_B);
    assert(ov_face_combo(phone_layout, (a[0] + b[0]) / 2, (a[1] + b[1]) / 2, 2400, 1080) == (DM_BTN_A | DM_BTN_B));
    assert(!ov_face_combo(phone_layout, (a[0] + y[0]) / 2, (a[1] + y[1]) / 2, 2400, 1080));
    custom_a = ia; custom_x = ix;
    for (custom_faces = 1; custom_faces <= 3; custom_faces++) {
        ov_box(&phone_layout[ia], 2400, 1080, a); ov_box(&phone_layout[ix], 2400, 1080, x);
        uint64_t pair = ov_face_combo(phone_layout, (a[0] + x[0]) / 2, (a[1] + x[1]) / 2, 2400, 1080);
        assert(pair == (custom_faces == 3 ? 0 : (DM_BTN_A | DM_BTN_X)));
        assert(!ov_face_combo(phone_layout, a[0], a[1], 2400, 1080));
        assert(!ov_face_combo(phone_layout, x[0], x[1], 2400, 1080));
    }
    custom_faces = 0;
    // The saved phone layout must be evaluated against the phone's dimensions,
    // even if a 16:9 external screen is attached.
    float phone[4], external[4];
    ov_box(&OV_LAYOUTS[1][0], 2400, 1080, phone);
    ov_box(&OV_LAYOUTS[1][0], 1920, 1080, external);
    assert(fabsf(phone[0] - 270) < 0.01f && fabsf(phone[1] - 594) < 0.01f);
    assert(fabsf(external[0] - 216) < 0.01f);
    assert(fabsf(phone[2] - OV_LAYOUTS[1][0].w * 1080 * 0.705f / 2) < 0.01f);
    assert(ov_drag_pos(126, 30, 1000, 50) == 150);
    assert(ov_drag_pos(124, 30, 1000, 50) == 100);
    assert(ov_drag_pos(-100, 65, 1000, 50) == 100);
    assert(ov_drag_pos(1100, 65, 1000, 50) == 900);
    assert(ov_drag_pos(126, 30, 1000, 0) == 126);
    assert(ov_drag_pos(1100, 65, 1000, 0) == 935);
    assert(ov_drag_pos(1100, 600, 1013, 50) == 506.5f);
    assert(ov_drag_pos(1100, 505, 1013, 50) == 508);

    const float screens[][2] = {{915, 412}, {1216, 760}, {2400, 1080}, {1080, 2400}};
    for (unsigned s = 0; s < sizeof screens / sizeof screens[0]; s++) {
        float W = screens[s][0], H = screens[s][1], step = H / OVE_GRID_ROWS, p[4];
        ov_menu_rect(W, H, p);
        for (int row = 0; row < OVM_ROWS; row++) {
            float r[4]; ov_menu_rowrect(p, row, r);
            assert(ov_menu_row_at(r[0] + r[2] / 2, r[1] + r[3] / 2, W, H) == row);
        }
        for (int axis = 0; axis < 2; axis++) for (int i = -10; i <= 110; i++) {
            float extent = screens[s][axis], half = H * 0.12f;
            float pos = ov_drag_pos(extent * i / 100, half, extent, step);
            assert(pos >= half - 0.001f && pos <= extent - half + 0.001f);
            assert(fabsf(pos / step - roundf(pos / step)) < 0.001f);
        }
        ov_ed_rect(W, H, p);
        for (int i = 0; i < OVE_ALL; i++) {
            if (i == OVE_DISCARD) ov_ed_prompt_rect(W, H, p);
            float b[4]; ov_ed_button(i, p, b);
            assert(b[0] >= p[0] && b[0] + b[2] <= p[0] + p[2]);
            assert(b[1] >= p[1] && b[1] + b[3] <= p[1] + p[3]);
            int first = i < OVE_N ? 0 : OVE_DISCARD, n = i < OVE_N ? OVE_N : 2;
            assert(ov_ed_button_at(b[0] + b[2] / 2, b[1] + b[3] / 2, p, first, n) == i);
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_overlay_') as tmp:
            tmp = Path(tmp)
            (tmp / 'android_profile.h').write_text(android_profile.header(None), encoding='utf-8')
            c, exe = tmp / 'check.c', tmp / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', f'-I{tmp}',
                                     f'-I{DM / "android"}', f'-I{DM / "host"}', str(c), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
