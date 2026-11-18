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
static int custom_faces, custom_a, custom_x, hidden_ctl = -1, audio_available;
int dm_menu_audio_available(void) { return audio_available; }
float dm_overlay_scale(void) { return 1; }
int dm_overlay_off(int l, int c) { return l == 1 && c == hidden_ctl; }
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
    // a hidden face button drops out of the two-button combo
    hidden_ctl = ix;
    assert(!ov_face_combo(phone_layout, (a[0] + x[0]) / 2, (a[1] + x[1]) / 2, 2400, 1080));
    assert(ov_face_combo(phone_layout, (a[0] + b[0]) / 2, (a[1] + b[1]) / 2, 2400, 1080) == (DM_BTN_A | DM_BTN_B));
    hidden_ctl = -1;
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

    assert(ov_menu_item(0, OVM_ROWS - 2) == OVM_PAD && ov_menu_item(1, OVM_ROWS - 2) == -1);
    assert(ov_menu_item(1, OVM_ROWS - 1) == OVM_RESUME);
    audio_available = 1;
    assert(ov_menu_item(1, OVM_ROWS - 2) == OVM_AUDIO);
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


    def test_grown_controls_do_not_overlap(self):
        """On a small screen the controls grow so a face button stays 10 mm wide (front_android.c dm_overlay_scale). Each side grows out from its bottom
        corner and no further than fits (overlay_layout.h ov_box, ov_fit_scale): no two controls overlap and none leaves the window. Grown around their
        own centres, the Odin 3's 1920x1080 at x1.45 made eleven pairs overlap and pushed ZL, ZR and the menu button off the screen. The top strip (the
        menu button and the frame rate counter's FPS row; its temperature rows are not kept clear: overlay_layout.h ov_strip) stays clear"""
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the layout geometry check')
        harness = r"""
#include <assert.h>
#include <stdio.h>
#include "overlay_layout.h"
static float g_s = 1;
float dm_overlay_scale(void) { return g_s; }
int dm_overlay_custom(int layout, int ctl, float *x, float *y, float *s) { (void)layout; (void)ctl; (void)x; (void)y; *s = 1; return 0; }
static float gap(const ov_ctl *a, const float p[4], const ov_ctl *b, const float q[4]) {   // < 0: they overlap (round ones as circles, the rest as boxes)
    float dx = fabsf(p[0] - q[0]), dy = fabsf(p[1] - q[1]);
    if (a->w == a->h && b->w == b->h) return hypotf(dx, dy) - p[2] - q[2];
    return fmaxf(dx - p[2] - q[2], dy - p[3] - q[3]);
}
int main(void) {
    const float screens[][3] = {{1920, 1080, 1}, {1920, 1080, 1.45f}, {2400, 1080, 1}, {2400, 1080, 1.45f}, {1280, 800, 1.45f}, {2560, 1600, 1.2f}, {2340, 1080, 1.45f}, {2340, 1080, 1.064f}, {2160, 1080, 1.45f}};
    for (unsigned k = 0; k < sizeof screens / sizeof screens[0]; k++) {
        float W = screens[k][0], H = screens[k][1]; int l = ov_layout_for_size((int)W, (int)H); const ov_ctl *L = OV_LAYOUTS[l];
        g_s = ov_fit_scale(L, W, H, screens[k][2]);
        assert(g_s >= 1 && g_s <= screens[k][2]);
        float b[OV_N][4];
        for (int i = 0; i < OV_N; i++) {
            ov_box(&L[i], W, H, b[i]);
            if (!(b[i][0] - b[i][2] >= -0.5f && b[i][0] + b[i][2] <= W + 0.5f && b[i][1] - b[i][3] >= -0.5f && b[i][1] + b[i][3] <= H + 0.5f)) {
                fprintf(stderr, "%gx%g x%.2f: %s leaves the window\n", W, H, g_s, L[i].name); return 1; }
        }
        // the top strip: the menu button (the frame rate counter sits beside it) and the controls beside it (the phone's - and +), which grow down from
        // the top edge, away from the counter's corner; every other control stays below it
        float strip = 0; for (int i = 0; i < OV_N; i++) if (L[i].kind == OV_MENU || ov_beside(L, &L[i])) strip = fmaxf(strip, b[i][1] + b[i][3]);
        for (int i = 0; i < OV_N; i++) if (L[i].kind != OV_MENU && !ov_beside(L, &L[i]) && b[i][1] - b[i][3] < strip - 0.5f) {
            fprintf(stderr, "%gx%g x%.2f: %s reaches into the top strip\n", W, H, g_s, L[i].name); return 1; }
        for (int i = 0; i < OV_N; i++) if (ov_beside(L, &L[i])) {
            float edge0 = fabsf(L[i].x) * H - L[i].w * H / 2, edge = L[i].ax == 0 ? b[i][0] - b[i][2] : W - b[i][0] - b[i][2];
            if (edge < edge0 - 0.5f || b[i][1] - b[i][3] < L[i].y * H - L[i].h * H / 2 - 0.5f) {
                fprintf(stderr, "%gx%g x%.2f: %s moves towards its top corner\n", W, H, g_s, L[i].name); return 1; }
        }
        for (int i = 0; i < OV_N; i++) for (int j = i + 1; j < OV_N; j++) if (gap(&L[i], b[i], &L[j], b[j]) < 0) {
            fprintf(stderr, "%gx%g x%.2f: %s and %s overlap by %.0f px\n", W, H, g_s, L[i].name, L[j].name, -gap(&L[i], b[i], &L[j], b[j])); return 1; }
    }
    g_s = ov_fit_scale(OV_LAYOUTS[0], 1920, 1080, 1.45f);   // the Odin 3 still gets bigger controls, only not as far as x1.45
    assert(g_s > 1.15f && g_s < 1.45f);
    // a phone grows too: - and + beside the menu button do not hold it at x1
    assert(ov_fit_scale(OV_LAYOUTS[1], 2400, 1080, 1.2f) > 1);
    // 2340x1080 at 425 dpi: A is 9.4 mm, so front_android.c dm_overlay_scale asks for x1.06 (as on main), and all of it fits
    float face = 0; for (int i = 0; i < OV_N; i++) if (OV_LAYOUTS[1][i].btn == DM_BTN_A) face = OV_LAYOUTS[1][i].w;
    float want = OV_MIN_MM / (face * 1080 / 425.0f * 25.4f);
    assert(want > 1.05f && want < 1.07f && ov_fit_scale(OV_LAYOUTS[1], 2340, 1080, want) == want);
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix='nx2a_overlay_') as tmp:
            tmp = Path(tmp)
            (tmp / 'android_profile.h').write_text(android_profile.header(None), encoding='utf-8')
            c, exe = tmp / 'check.c', tmp / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', f'-I{tmp}',
                                     f'-I{DM / "android"}', f'-I{DM / "host"}', str(c), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
