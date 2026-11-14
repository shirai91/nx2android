"""The Android frame rate counter's options: the menu table (4 tabs), the settings.txt keys and the packed state."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.upstream import android_profile


ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


class FpsCounterOptions(unittest.TestCase):
    def test_menu_table_settings_and_state(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the counter options check')
        front = (DM / 'android/front_android.c').read_text(encoding='utf-8')
        opts = front[front.index('// FPS_OPTS_BEGIN'):front.index('// FPS_OPTS_END')]
        harness = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overlay_layout.h"
static _Atomic int g_fps_opts = FPSO_DEFAULT, g_fps_ver;
int dm_menu_audio_available(void) { return 1; }
float dm_overlay_scale(void) { return 1; }
int dm_overlay_off(int l, int c) { (void)l; (void)c; return 0; }
int dm_overlay_custom(int l, int c, float *x, float *y, float *s) { (void)l; (void)c; (void)x; (void)y; *s = 1; return 0; }
''' + opts + r'''
static int parse(const char *line) { return fps_opt_parse(line); }
int main(void) {
    // four tabs, five rows each; the Counter tab: the five rows (FPS, SOC, BAT, driver, graph); the Style tab: the four choices then a blank
    assert(OVM_TABS == 4 && OVM_PER_TAB == 5 && OVM_ROWS == 7);
    assert(ov_menu_item(0, 0) == OVM_TAB && ov_menu_item(3, 6) == OVM_RESUME);
    assert(ov_menu_item(1, 1) == OVM_FPS && ov_menu_item(1, 5) == OVM_AUDIO);
    const int counter[5] = {OVM_ROW_FPS, OVM_ROW_SOC, OVM_ROW_BAT, OVM_ROW_DRV, OVM_ROW_GRAPH}, style[5] = {OVM_POS, OVM_COLOUR, OVM_OUTLINE, OVM_SIZE, -1};
    for (int r = 0; r < 5; r++) { assert(ov_menu_item(2, r + 1) == counter[r]); assert(ov_menu_item(3, r + 1) == style[r]); }
    assert(!strcmp(OVM_TAB_NAMES[2], "Counter") && !strcmp(OVM_TAB_NAMES[3], "Style"));
    float p[4] = {0, 0, 1000, 700};
    for (int t = 0; t < OVM_TABS; t++) { float r[4]; ov_menu_rowrect(p, 0, r); assert(ov_menu_tab_at(r[0] + (t + 0.5f) * r[2] / OVM_TABS, p) == t); }
    // the defaults: today's look, the driver row and the graph on
    int o = atomic_load(&g_fps_opts);
    assert(fpso_rows(o) == 31 && FPSO_GRAPH == 16 && fpso_pos(o) == FPSO_TL && fpso_colour(o) == FPSO_WHITE && fpso_outline(o) == FPSO_BOX && fpso_size(o) == FPSO_NORMAL);
    // the letters' scale for a base of 3 (a 1080 tall window's raster is 540 tall: base 2)
    assert(fpso_scale(3, FPSO_SMALL) == 2 && fpso_scale(3, FPSO_NORMAL) == 3 && fpso_scale(3, FPSO_LARGE) == 4 && fpso_scale(3, FPSO_XL) == 6 && fpso_scale(1, FPSO_SMALL) == 1);
    // keys: every value, out of range falls back, other lines are not the counter's
    assert(parse("fps_rows=9\n") && fpso_rows(atomic_load(&g_fps_opts)) == 9);
    assert(parse("fps_rows=0\n") && fpso_rows(atomic_load(&g_fps_opts)) == 0);
    assert(parse("fps_rows=16\n") && fpso_rows(atomic_load(&g_fps_opts)) == 16);   // the graph alone
    assert(parse("fps_rows=17\n") && fpso_rows(atomic_load(&g_fps_opts)) == 17);
    assert(parse("fps_rows=32\n") && fpso_rows(atomic_load(&g_fps_opts)) == 31 && parse("fps_rows=-1\n") && fpso_rows(atomic_load(&g_fps_opts)) == 31);
    assert(parse("fps_pos=3\n") && parse("fps_colour=4\n") && parse("fps_outline=4\n") && parse("fps_size=0\n"));
    o = atomic_load(&g_fps_opts);
    assert(fpso_pos(o) == 3 && fpso_colour(o) == 4 && fpso_outline(o) == 4 && fpso_size(o) == 0);
    assert(parse("fps_pos=4\n") && parse("fps_colour=-1\n") && parse("fps_outline=5\n") && parse("fps_size=9\n"));
    o = atomic_load(&g_fps_opts);
    assert(fpso_pos(o) == FPSO_TL && fpso_colour(o) == FPSO_WHITE && fpso_outline(o) == FPSO_BOX && fpso_size(o) == FPSO_NORMAL);
    assert(!parse("fps=1\n") && !parse("opacity=60\n"));
    // save, then load again
    atomic_store(&g_fps_opts, fpso_pack(5, 2, 3, 2, 3));
    FILE *f = tmpfile(); assert(f); fps_opt_write(f); long n = ftell(f); rewind(f);
    char *buf = calloc((size_t)n + 1, 1); assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n); fclose(f);
    assert(strstr(buf, "fps_rows=5\n") && strstr(buf, "fps_pos=2\n") && strstr(buf, "fps_colour=3\n") && strstr(buf, "fps_outline=2\n") && strstr(buf, "fps_size=3\n"));
    atomic_store(&g_fps_opts, FPSO_DEFAULT);
    for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) { char line[64]; snprintf(line, sizeof line, "%s\n", l); assert(parse(line)); }
    assert(atomic_load(&g_fps_opts) == fpso_pack(5, 2, 3, 2, 3));
    // the menu presses: a row's bit flips, the choices go round, each change counts
    atomic_store(&g_fps_opts, FPSO_DEFAULT);
    assert(fps_opt_press(OVM_ROW_SOC) && fpso_rows(atomic_load(&g_fps_opts)) == 29 && atomic_load(&g_fps_ver) == 1);
    assert(fps_opt_press(OVM_ROW_SOC) && fpso_rows(atomic_load(&g_fps_opts)) == 31);
    assert(fps_opt_press(OVM_ROW_GRAPH) && fpso_rows(atomic_load(&g_fps_opts)) == 15 && fpso_pos(atomic_load(&g_fps_opts)) == FPSO_TL);   // (the bit does not reach the corner)
    assert(fps_opt_press(OVM_ROW_GRAPH) && fpso_rows(atomic_load(&g_fps_opts)) == 31);
    for (int i = 0; i < FPSO_POS_N; i++) assert(fpso_pos(atomic_load(&g_fps_opts)) == i && fps_opt_press(OVM_POS));
    for (int i = 0; i < FPSO_COLOUR_N; i++) fps_opt_press(OVM_COLOUR);
    for (int i = 0; i < FPSO_OUTLINE_N; i++) fps_opt_press(OVM_OUTLINE);
    for (int i = 0; i < FPSO_SIZE_N; i++) fps_opt_press(OVM_SIZE);
    assert(atomic_load(&g_fps_opts) == FPSO_DEFAULT);   // all the way round
    assert(!fps_opt_press(OVM_FPS) && !fps_opt_press(OVM_MODE));   // the other items are not the counter's
    free(buf);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_fpsopts_') as tmp:
            tmp = Path(tmp)
            (tmp / 'android_profile.h').write_text(android_profile.header(None), encoding='utf-8')
            c, exe = tmp / 'check.c', tmp / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', f'-I{tmp}', f'-I{DM / "android"}', f'-I{DM / "host"}',
                                     str(c), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_driver_name_and_glyphs(self):
        core = (DM / 'android/gfx_vk_core.c').read_text(encoding='utf-8')
        present = (DM / 'android/gfx_vk_present.c').read_text(encoding='utf-8')
        # the version is decoded as the GPU log line does; every letter of the names the driver row can show is in the counter's character set
        self.assertIn('d >> 22, d >> 12 & 0x3ff, d & 0xfff', core)
        line = next(l for l in present.splitlines() if l.startswith('#define OV_FPS_CHARS'))
        chars = line.split('"')[1]
        for name in ('TURNIP', 'QCOM', 'ARM', 'IMG', 'NVIDIA', 'AMD', 'INTEL', 'ID 0123456789ABCDEF.'):
            for ch in name:
                self.assertIn(ch, chars, f'{ch!r} of {name} is not a counter character')

    def test_profile_settings(self):
        for k, v in (('fps_rows', '31'), ('fps_pos', '3'), ('fps_colour', '4'), ('fps_outline', '0'), ('fps_size', '2')):
            self.assertIn(v, android_profile.SETTINGS[k])
        self.assertIn('16', android_profile.SETTINGS['fps_rows'])
        self.assertNotIn('32', android_profile.SETTINGS['fps_rows'])
        self.assertNotIn('4', android_profile.SETTINGS['fps_pos'])


if __name__ == '__main__':
    unittest.main()
