"""Run the pause menu's real hit testing, gestures and controller navigation."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from porter.upstream import android_profile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


class MenuScroll(unittest.TestCase):
    def test_external_menu_uses_native_geometry_and_revision(self):
        java = (DM / 'android/java/org/nx2a/ExternalActivity.java').read_text(encoding='utf-8')
        native = (DM / 'android/gfx_vk_present.c').read_text(encoding='utf-8')
        self.assertIn('externalMenuRect(getWidth(), getHeight(), menuState)', java)
        self.assertIn('rasterRevision == revision && rasterState == state', java)
        self.assertIn('externalMenuRevision()', java)
        self.assertIn('Java_org_nx2a_ExternalActivity_externalMenuRect', native)
        self.assertIn('rev != g_ov.panel_rev || dpi != g_ov.panel_dpi', native)
        self.assertIn('if (st & (OVT_ST | OVE_ST | OVS_ST)) ov_menu_rect(W, H, r);', native)

    def test_production_raster_keeps_the_designed_label_size(self):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('a C compiler is required')
        source = (DM / 'android/gfx_vk_present.c').read_text(encoding='utf-8')
        draw = csrc.between(source, 'typedef struct { uint8_t *px; uint32_t stride', 'static void ov_menu_icon(')
        menu = csrc.between(source, 'static const uint8_t C_BG', "// The layout editor's toolbar")
        code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overlay_layout.h"
#include "pixel_font.h"
static uint8_t *font;
static int chinese, position, option_size;
int dm_overlay_dpi(void) { return 320; }
int dm_menu_scroll_position(void) { return position; }
int dm_ui_zh(void) { return chinese; }
int dm_fps_opts(void) { return fpso_pack(7,0,0,0,option_size); }
const uint8_t *dm_overlay_font(void) { return font; }
float dm_overlay_scale(void) { return 1; }
int dm_overlay_custom(int l,int c,float*x,float*y,float*s) {(void)l;(void)c;(void)x;(void)y;*s=1;return 0;}
int dm_menu_audio_available(void) { return 0; }
int dm_overlay_stick_zone(void) { return 0; }   // (the Stick area row's segments)
int dm_save_note(char *s,int n) { if(n)s[0]=0;return OVX_IDLE; }
''' + draw + menu + r'''
int main(int argc,char **argv) {
    (void)C_HOT;
    assert(argc==2); FILE *f=fopen(argv[1],"rb");assert(f);
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);font=malloc(size);assert(font);
    assert(fread(font,1,size,f)==(size_t)size);fclose(f);
    const float screens[][2]={{915,412},{1216,760},{1920,1080},{3168,1440}};
    for(chinese=0;chinese<=1;chinese++) for(option_size=0;option_size<4;option_size++) for(unsigned k=0;k<sizeof screens/sizeof *screens;k++) {
        float W=screens[k][0],H=screens[k][1]; ov_pause l;ov_pause_layout(W,H,320,1,0,&l);
        unsigned w=(unsigned)(l.p[2]/2)+1,h=(unsigned)(l.p[3]/2)+1;
        uint8_t *pixels=calloc((size_t)w*h,4);assert(pixels);
        int state=(1<<8)|(((60-OV_OPACITY_MIN)/5)<<9)|(3<<13);
        ov_canvas cv={.px=pixels,.stride=w,.w=w,.h=h};position=0;ov_menu_panel(&cv,state,W,H);
        float sx=w/l.p[2],sy=h/l.p[3],rh=l.row*sy;
        float x=(l.view[0]-l.p[0])*sx+rh*.15f;
        float cy=(l.view[1]-l.p[1])*sy+rh*(l.stacked?.27f:.5f);
        uint8_t *expected=calloc((size_t)w*h,4);assert(expected);
        ov_canvas reference={.px=expected,.stride=w,.w=w,.h=h};
        int scale=(int)fmaxf(1,roundf((12+2*option_size)*l.dp*sy/PF_CAP));
        cv_text(&reference,x,cy,chinese?"显示帧率":"Show FPS",scale,C_DIM);
        unsigned ink=0;
        for(unsigned y=(unsigned)((l.view[1]-l.p[1])*sy);y<(l.view[1]-l.p[1])*sy+rh*.5f;y++) for(unsigned xx=(unsigned)x;xx<x+cv_text_w(chinese?"显示帧率":"Show FPS",scale);xx++) {
            size_t i=4*((size_t)y*w+xx);
            int actual=memcmp(pixels+i,C_DIM,4)==0,want=expected[i+3]!=0;
            assert(actual==want);ink+=want;
        }
        assert(ink>0);free(expected);
        uint8_t *fixed=malloc((size_t)w*h*4);assert(fixed);memcpy(fixed,pixels,(size_t)w*h*4);
        unsigned top=(unsigned)ceilf((l.view[1]-l.p[1])*sy),bottom=(unsigned)floorf((l.view[1]+l.view[3]-l.p[1])*sy);
        for(position=137;position<l.maxscroll*OVM_SCROLL_UNITS;position+=OVM_SCROLL_UNITS*13) {
            memset(pixels,0,(size_t)w*h*4);ov_menu_panel(&cv,state,W,H);
            assert(memcmp(fixed,pixels,(size_t)top*w*4)==0);
            assert(memcmp(fixed+(size_t)bottom*w*4,pixels+(size_t)bottom*w*4,(size_t)(h-bottom)*w*4)==0);
        }
        free(fixed);free(pixels);
    }
    free(font);return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_menu_font_') as tmp:
            tmp = Path(tmp)
            (tmp/'android_profile.h').write_text(android_profile.header(None))
            src, exe = tmp/'raster.c', tmp/'raster'
            src.write_text(code, encoding='utf-8')
            build = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-missing-field-initializers', '-I'+str(tmp), '-I'+str(DM/'android'), '-I'+str(DM/'host'), str(src), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            font = DM/'third_party/fusion-pixel-font/fusion-pixel-12px-proportional-zh_tw.nxpf'
            result = subprocess.run([str(exe), str(font)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_swipes_do_not_activate_rows_and_controller_reveals_selection(self):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('a C compiler is required')
        front = (DM / 'android/front_android.c').read_text(encoding='utf-8')
        state = csrc.between(front, 'static _Atomic int g_menu_scroll;', 'static _Atomic int g_tx_ver')
        code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include "overlay_layout.h"
float dm_overlay_scale(void) { return 1; }
int dm_overlay_layout(void) { return 0; }
int dm_overlay_custom(int l, int c, float *x, float *y, float *s) { (void)l; (void)c; (void)x; (void)y; *s=1; return 0; }
int dm_menu_audio_available(void) { return 0; }
enum { AMOTION_EVENT_ACTION_DOWN, AMOTION_EVENT_ACTION_UP, AMOTION_EVENT_ACTION_MOVE,
       AMOTION_EVENT_ACTION_CANCEL, AMOTION_EVENT_ACTION_POINTER_DOWN, AMOTION_EVENT_ACTION_POINTER_UP, AMOTION_EVENT_ACTION_SCROLL };
#define AMOTION_EVENT_AXIS_VSCROLL 9
#define AMotionEvent_getAxisValue(e,a,i) ((void)(e),(void)(a),(void)(i),-1.0f)
#define AMOTION_EVENT_ACTION_MASK 255
#define AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT 8
typedef struct { int action; float x, y; } AInputEvent;
#define AMotionEvent_getAction(e) ((e)->action)
#define AMotionEvent_getPointerId(e,i) ((void)(e),(void)(i),0)
#define AMotionEvent_getPointerCount(e) ((void)(e),1u)
#define AMotionEvent_getX(e,i) ((void)(i),(e)->x)
#define AMotionEvent_getY(e,i) ((void)(i),(e)->y)
static _Atomic uint32_t g_menu_revision;
static float screen_w,screen_h;
typedef int ANativeWindow;
static ANativeWindow window;
static ANativeWindow *phone_window(void) { return &window; }
#define ANativeWindow_getWidth(w) ((void)(w),screen_w)
#define ANativeWindow_getHeight(w) ((void)(w),screen_h)
int dm_overlay_dpi(void) { return 160; }
static _Atomic int g_edit, g_sp_on, g_set_sharp, g_set_opacity = 60;
static int actions, last_action = -1;
static void dm_overlay_redraw(void) {}
static void dm_haptics_android_tap(void) {}
static void settings_save(void) {}
static int ctl_at(float x,float y,float W,float H) { (void)x;(void)y;(void)W;(void)H;return -1; }
static void menu_do(int item) { actions++; last_action=item; }
static void menu_close(void) {}
static void ed_key(uint64_t bit) { (void)bit; }
static void sp_key(uint64_t bit) { (void)bit; }
''' + state + csrc.function(front, 'static void menu_sharp(') + csrc.function(front, 'static void menu_opacity(') + next(line for line in front.splitlines(True) if line.startswith('static void menu_tab(int t)')) + csrc.function(front, 'static void menu_scroll(') + csrc.function(front, 'static void menu_touch(') + csrc.function(front, 'static void menu_key(') + r'''
static void touch(int action, float x, float y, float W, float H) {
    AInputEvent e={action,x,y}; menu_touch(&e,W,H);
}
int main(void) {
    const float screens[][2]={{915,412},{1216,760},{3168,1440},{1080,2400},{360,640}};
    for(unsigned n=0;n<sizeof screens/sizeof *screens;n++) {
        float W=screen_w=screens[n][0],H=screen_h=screens[n][1];ov_pause l;ov_pause_layout(W,H,160,1,0,&l);
        assert(l.p[0]>=0&&l.p[1]>=0&&l.p[2]<=W&&l.p[3]<=H&&l.count==6);
        assert(OVM_TABS==4&&OVM_VIEW_ROWS==7&&OVM_ROWS==9);
        atomic_store(&g_menu_tab,1);atomic_store(&g_menu_scroll,0);g_menu_ptr=-1;actions=0;
        float r[4];ov_pause_rowrect(&l,1,r);float x=r[0]+r[2]*.2f,y=r[1]+r[3]/2;
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);touch(AMOTION_EVENT_ACTION_UP,x,y,W,H);
        assert(actions==1&&last_action==OVM_FPS);
        uint32_t revision=dm_menu_revision();
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);
        touch(AMOTION_EVENT_ACTION_MOVE,x,y-24.25f,W,H);
        int previous=atomic_load(&g_menu_scroll);
        touch(AMOTION_EVENT_ACTION_UP,x,y-24.25f,W,H);
        assert(actions==1&&atomic_load(&g_menu_scroll)==previous);
        if(l.maxscroll>0) {assert(previous>0&&previous<24.25f*OVM_SCROLL_UNITS);assert(dm_menu_revision()>revision);}
        touch(AMOTION_EVENT_ACTION_SCROLL,x,y,W,H);
        assert(atomic_load(&g_menu_scroll)<=l.maxscroll*OVM_SCROLL_UNITS+1);
        atomic_store(&g_menu_hi,-1);
        for(int i=0;i<OVM_ROWS*2;i++) {
            menu_key(DM_BTN_DOWN);int hi=atomic_load(&g_menu_hi);
            ov_pause_layout(W,H,160,1,ov_menu_scroll_rows(atomic_load(&g_menu_scroll)),&l);
            assert(ov_menu_item(1,hi)>=0);ov_pause_rowrect(&l,hi,r);
            if(hi>0&&hi<OVM_ROWS-1) {assert(r[1]>=l.view[1]-.01f);assert(r[1]+r[3]<=l.view[1]+l.view[3]+.01f);}
        }
        atomic_store(&g_menu_hi,6);menu_key(DM_BTN_A);assert(last_action==OVM_IMPORT);
        menu_scroll(&l,l.maxscroll);ov_pause_layout(W,H,160,1,ov_menu_scroll_rows(atomic_load(&g_menu_scroll)),&l);ov_pause_rowrect(&l,6,r);
        x=r[0]+r[2]*.2f;y=r[1]+r[3]/2;int before=actions;
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);touch(AMOTION_EVENT_ACTION_UP,x,y,W,H);assert(actions==before+1&&last_action==OVM_IMPORT);
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);touch(AMOTION_EVENT_ACTION_MOVE,x,y+10000,W,H);touch(AMOTION_EVENT_ACTION_UP,x,y+10000,W,H);
        assert(actions==before+1&&atomic_load(&g_menu_scroll)==0);
        menu_tab(0);assert(atomic_load(&g_menu_scroll)==0);
        ov_pause_layout(W,H,160,0,0,&l);float track[3];ov_pause_track(&l,2,track);x=track[0];y=track[1];
        atomic_store(&g_set_opacity,60);
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);assert(atomic_load(&g_set_opacity)==60);
        touch(AMOTION_EVENT_ACTION_MOVE,x+track[2],y,W,H);
        touch(AMOTION_EVENT_ACTION_UP,x+track[2],y,W,H);
        assert(atomic_load(&g_set_opacity)==80&&atomic_load(&g_menu_scroll)==0);
        // Vertical motion on a slider scrolls, never changes the slider.
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);touch(AMOTION_EVENT_ACTION_MOVE,x,y-30,W,H);touch(AMOTION_EVENT_ACTION_UP,x,y-30,W,H);
        assert(atomic_load(&g_set_opacity)==80);
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);touch(AMOTION_EVENT_ACTION_CANCEL,x,y,W,H);assert(g_menu_ptr==-1);
        atomic_store(&g_menu_scroll,0);ov_pause_layout(W,H,160,1,0,&l);ov_pause_track(&l,3,track);
        atomic_store(&g_menu_tab,1);atomic_store(&g_set_sharp,0);x=track[0];y=track[1];
        if(y<l.footer[1]) {touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);touch(AMOTION_EVENT_ACTION_MOVE,x+.74f*track[2],y,W,H);touch(AMOTION_EVENT_ACTION_UP,x+.74f*track[2],y,W,H);assert(atomic_load(&g_set_sharp)==75);}
        atomic_store(&g_set_sharp,75);atomic_store(&g_menu_hi,3);menu_key(DM_BTN_LEFT);assert(atomic_load(&g_set_sharp)==50);
    }
    for(int dpi=160;dpi<=640;dpi*=2) {
        ov_pause l;ov_pause_layout(1080,2400,dpi,0,100000,&l);assert(l.scroll==l.maxscroll&&l.row==(l.stacked?88:64)*dpi/160.0f);
        float footer[4],header[4];ov_pause_rowrect(&l,0,header);ov_pause_rowrect(&l,OVM_ROWS-1,footer);
        ov_pause top;ov_pause_layout(1080,2400,dpi,0,-1,&top);assert(top.scroll==0);
        assert(header[1]==top.header[1]&&footer[1]==top.footer[1]);
        assert(ov_pause_row_at(&l,header[0]+1,header[1]+1)==0);
        assert(ov_pause_row_at(&l,footer[0]+1,footer[1]+1)==OVM_ROWS-1);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_menu_scroll_') as tmp:
            tmp = Path(tmp)
            (tmp / 'android_profile.h').write_text(android_profile.header(None))
            src, exe = tmp / 'check.c', tmp / 'check'
            src.write_text(code, encoding='utf-8')
            build = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I'+str(tmp), '-I'+str(DM/'android'), '-I'+str(DM/'host'), str(src), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
