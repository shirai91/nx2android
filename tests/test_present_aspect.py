"""Run production presentation geometry and Fit/Fill settings without Vulkan."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc

DM = Path(__file__).resolve().parents[1] / 'nxnative/dm/android'


class PresentAspect(unittest.TestCase):
    def test_fit_fill_and_source_crop(self):
        source = (DM / 'gfx_vk_present.c').read_text()
        geometry = csrc.between(source, '    static double aspect;', '    if (im->layout')
        front = (DM / 'front_android.c').read_text()
        code = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
static struct { int width, height; } g_sext;
static int band, bottom_up, saved, taps, redraws;
static _Atomic int g_set_fill, g_fps_ver;
static void settings_save(void) { saved++; }
static void dm_haptics_android_tap(void) { taps++; }
static void dm_overlay_redraw(void) { redraws++; }
''' + next(line + '\n' for line in front.splitlines() if line.startswith('int dm_overlay_fill(')) + csrc.function(front, 'static void menu_fill(') + r'''
static int dm_overlay_visible(void) { return band; }
static int ov_band(float w, float h, float *y, float *bh) { (void)w; *y=h/2; *bh=h/2; return band; }
static int vk_window_bottom_up(void) { return bottom_up; }
static void vk_pic_set(float x,float y,float w,float h) { (void)x;(void)y;assert(w>0 && h>0); }
#define VKLOG(...) ((void)0)
static int rect[8];
static void geometry(int cx,int cy,int cw,int ch) {
''' + geometry + r'''
    rect[0]=dx;rect[1]=dy;rect[2]=dw;rect[3]=dh;
    rect[4]=cx;rect[5]=cy;rect[6]=cw;rect[7]=ch;
}
int main(int argc, char **argv) {
    assert(argc==2);
    int wide=atoi(argv[1]);
    g_sext.width=1920;g_sext.height=1080;
    geometry(0,0,1920,1080);
    assert(rect[0]==0 && rect[2]==1920);
    if(wide) { assert(rect[1]==138 && rect[3]==803); }
    else { assert(rect[1]==0 && rect[3]==1080 && rect[6]==1920); }
    menu_fill(1);assert(saved==1 && taps==1 && redraws==1 && g_fps_ver==1);
    menu_fill(1);assert(saved==1 && g_fps_ver==1);
    geometry(100,50,1920,1080);
    assert(rect[0]==0 && rect[1]==0 && rect[2]==1920 && rect[3]==1080);
    if(wide) { assert(rect[4]>100 && rect[6]<1920 && rect[5]==50 && rect[7]==1080); }
    menu_fill(0);assert(saved==2 && g_fps_ver==2);
    // Portrait display with the gamepad band; Fill clips source, not the destination.
    g_sext.width=800;g_sext.height=1200;band=1;
    geometry(100,50,1920,1080);
    assert(rect[0]>=0 && rect[1]>=0 && rect[2]<=800 && rect[3]<=600);
    menu_fill(1);geometry(100,50,1920,1080);
    assert(rect[0]==0 && rect[1]==0 && rect[2]==800 && rect[3]==600);
    assert(rect[4]>=100 && rect[5]>=50 && rect[4]+rect[6]<=2020 && rect[5]+rect[7]<=1130);
    int top=rect[5], height=rect[7];bottom_up=1;
    geometry(100,50,1920,1080);
    assert(rect[5]==50+1080-(top-50+height));
    return 0;
}
'''
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('C compiler required')
        with tempfile.TemporaryDirectory(prefix='nx2a_aspect_geometry_') as tmp:
            src, exe = Path(tmp) / 'geometry.c', Path(tmp) / 'geometry'
            src.write_text(code)
            build = subprocess.run([cc, '-std=c11', str(src), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            import os
            for aspect in ('', '43:18', 'invalid', '0:9', 'nan:9', '43:18extra'):
                with self.subTest(aspect=aspect):
                    result = subprocess.run([str(exe), str(int(aspect == '43:18'))], env={**os.environ, 'DM_PRESENT_ASPECT': aspect}, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
