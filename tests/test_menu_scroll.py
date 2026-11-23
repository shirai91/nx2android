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


def overflow_fixture(tmp):
    # Exercise a longer Game tab with existing actions, including the optional audio row.
    header = (DM / 'android/overlay_layout.h').read_text(encoding='utf-8')
    header = header.replace('#define OVM_PER_TAB 5', '#define OVM_PER_TAB 9')
    header = header.replace('OVM_RUMBLE, OVM_PAD}', 'OVM_RUMBLE, OVM_PAD, -1, -1, -1, -1}')
    header = header.replace('OVM_IMPORT, OVM_AUDIO}', 'OVM_IMPORT, OVM_AUDIO, OVM_POS, OVM_COLOUR, OVM_OUTLINE, OVM_SIZE}')
    (tmp / 'overlay_layout.h').write_text(header, encoding='utf-8')


class MenuScroll(unittest.TestCase):
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
static int chinese;
int dm_ui_zh(void) { return chinese; }
int dm_fps_opts(void) { return FPSO_DEFAULT; }
const uint8_t *dm_overlay_font(void) { return font; }
float dm_overlay_scale(void) { return 1; }
int dm_overlay_custom(int l,int c,float*x,float*y,float*s) {(void)l;(void)c;(void)x;(void)y;*s=1;return 0;}
int dm_menu_audio_available(void) { return 0; }
int dm_save_note(char *s,int n) { if(n)s[0]=0;return OVX_IDLE; }
''' + draw + menu + r'''
int main(int argc,char **argv) {
    (void)C_HOT;
    assert(argc==2); FILE *f=fopen(argv[1],"rb");assert(f);
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);font=malloc(size);assert(font);
    assert(fread(font,1,size,f)==(size_t)size);fclose(f);
    const float screens[][2]={{915,412},{1216,760},{1920,1080},{3168,1440}};
    for(chinese=0;chinese<=1;chinese++) for(unsigned k=0;k<sizeof screens/sizeof *screens;k++) {
        float p[4];ov_menu_rect(screens[k][0],screens[k][1],p);
        unsigned w=(unsigned)(p[2]/2)+1,h=(unsigned)(p[3]/2)+1;
        uint8_t *pixels=calloc((size_t)w*h,4);assert(pixels);
        int state=(1<<8)|(60<<9);
        ov_canvas cv={pixels,w,0,0,w,h};ov_menu_panel(&cv,state,0);
        float r[4];ov_menu_rowrect((float[4]){0,0,w,h},1,r);
        // Compare the actual body label to a reference drawn at the intended scale in this language.
        uint8_t *expected=calloc((size_t)w*h,4);assert(expected);
        ov_canvas reference={expected,w,0,0,w,h};
        int scale=(int)fmaxf(1,roundf(r[3]*.30f/PF_CAP));
        cv_text(&reference,r[0]+r[3]*.15f,r[1]+r[3]/2,chinese?"显示帧率":"Show FPS",scale,C_DIM);
        unsigned ink=0;
        for(unsigned y=(unsigned)r[1];y<r[1]+r[3];y++) for(unsigned x=(unsigned)r[0];x<r[0]+r[2]*.36f;x++) {
            size_t i=4*((size_t)y*w+x);
            int actual=memcmp(pixels+i,C_DIM,4)==0, want=expected[i+3]!=0;
            assert(actual==want); ink+=want;
        }
        assert(ink>0);free(expected);
        uint8_t *fixed=malloc((size_t)w*h*4);assert(fixed);memcpy(fixed,pixels,(size_t)w*h*4);
        float first[4],last[4];ov_menu_rowrect((float[4]){0,0,w,h},1,first);ov_menu_rowrect((float[4]){0,0,w,h},OVM_VIEW_ROWS-1,last);
        unsigned top=(unsigned)ceilf(first[1]),bottom=(unsigned)floorf(last[1]);
        for(int position=1;position<=2*OVM_SCROLL_UNITS;position+=137) {
            memset(pixels,0,(size_t)w*h*4);ov_menu_panel(&cv,state,position);
            assert(ov_menu_raster_key(state,position)!=ov_menu_raster_key(state,position-1));
            assert(memcmp(fixed,pixels,(size_t)top*w*4)==0); // partial rows and scrollbar never cover the tabs
            assert(memcmp(fixed+(size_t)bottom*w*4,pixels+(size_t)bottom*w*4,(size_t)(h-bottom)*w*4)==0); // Resume stays fixed
        }
        free(fixed);free(pixels);
    }
    free(font);return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_menu_font_') as tmp:
            tmp = Path(tmp)
            overflow_fixture(tmp)
            (tmp/'android_profile.h').write_text(android_profile.header(None))
            src, exe = tmp/'raster.c', tmp/'raster'
            src.write_text(code, encoding='utf-8')
            build = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-I'+str(tmp), '-I'+str(DM/'android'), '-I'+str(DM/'host'), str(src), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            font = DM/'third_party/fusion-pixel-font/fusion-pixel-12px-proportional-zh_tw.nxpf'
            result = subprocess.run([str(exe), str(font)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_swipes_do_not_activate_rows_and_controller_reveals_selection(self):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('a C compiler is required')
        front = (DM / 'android/front_android.c').read_text(encoding='utf-8')
        state = csrc.between(front, 'static _Atomic int g_menu_hi', 'static _Atomic int g_tx_ver')
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
       AMOTION_EVENT_ACTION_CANCEL, AMOTION_EVENT_ACTION_POINTER_DOWN, AMOTION_EVENT_ACTION_POINTER_UP };
#define AMOTION_EVENT_ACTION_MASK 255
#define AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT 8
typedef struct { int action; float x, y; } AInputEvent;
#define AMotionEvent_getAction(e) ((e)->action)
#define AMotionEvent_getPointerId(e,i) ((void)(e),(void)(i),0)
#define AMotionEvent_getPointerCount(e) ((void)(e),1u)
#define AMotionEvent_getX(e,i) ((void)(i),(e)->x)
#define AMotionEvent_getY(e,i) ((void)(i),(e)->y)
static _Atomic int g_edit, g_sp_on, g_set_opacity = 60;
static int actions, last_action = -1;
static void dm_overlay_redraw(void) {}
static void dm_haptics_android_tap(void) {}
static void settings_save(void) {}
static int ctl_at(float x,float y,float W,float H) { (void)x;(void)y;(void)W;(void)H;return -1; }
static void menu_do(int item) { actions++; last_action=item; }
static void menu_close(void) {}
static void ed_key(uint64_t bit) { (void)bit; }
static void sp_key(uint64_t bit) { (void)bit; }
''' + state + csrc.function(front, 'static void menu_opacity(') + next(line for line in front.splitlines(True) if line.startswith('static void menu_tab(int t)')) + csrc.function(front, 'static void menu_touch(') + csrc.function(front, 'static void menu_key(') + r'''
static void touch(int action, float x, float y, float W, float H) {
    AInputEvent e={action,x,y}; menu_touch(&e,W,H);
}
int main(void) {
    const float screens[][2]={{915,412},{1216,760},{3168,1440},{1080,2400}};
    for (unsigned n=0;n<sizeof screens/sizeof *screens;n++) {
        float W=screens[n][0], H=screens[n][1], p[4], r[4]; ov_menu_rect(W,H,p);
        assert(p[0]>=0 && p[1]>=0 && p[2]<=W && p[3]<=H);
        atomic_store(&g_menu_tab,1); atomic_store(&g_menu_scroll,0); g_menu_ptr=-1; actions=0;
        assert(ov_menu_body_count(1)==8 && ov_menu_scroll_limit(1)==3);
        assert(ov_menu_scroll_limit(0)==0);
        for (int offset=0;offset<=3;offset++) for (int slot=0;slot<OVM_VIEW_ROWS;slot++) {
            ov_menu_rowrect(p,slot,r);
            int row=slot==0?0:slot==OVM_VIEW_ROWS-1?OVM_ROWS-1:ov_menu_body_row(1,offset+slot-1);
            assert(ov_menu_scroll_row_at(r[0]+r[2]/2,r[1]+r[3]/2,W,H,1,offset)==row);
            assert(ov_menu_scroll_slot(1,offset,row)==slot);
            assert(ov_menu_item(1,row)>=0);  // the unavailable audio action consumes no space
        }
        ov_menu_rowrect(p,3,r); float x=r[0]+r[2]*.2f, y=r[1]+r[3]/2, rh=r[3];
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);
        int previous=0;
        for(int step=0;step<3;step++) {
            touch(AMOTION_EVENT_ACTION_MOVE,x,y-(.28f+step*.1f)*rh,W,H);
            int position=atomic_load(&g_menu_scroll);
            assert(position>previous && position<OVM_SCROLL_UNITS/2); // continuous movement below a single row
            previous=position;
        }
        touch(AMOTION_EVENT_ACTION_UP,x,y-.48f*rh,W,H);
        assert(atomic_load(&g_menu_scroll)==previous && actions==0); // release does not snap
        float fraction=ov_menu_scroll_rows(previous);
        assert(ov_menu_scroll_rowrect(p,1,fraction,3,r));
        x=r[0]+r[2]*.2f;y=r[1]+r[3]/2;
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);touch(AMOTION_EVENT_ACTION_UP,x,y,W,H);
        assert(last_action==OVM_EXPORT && actions==1); // hit test follows a fractionally shifted row
        atomic_store(&g_menu_scroll,0);actions=0;
        ov_menu_rowrect(p,3,r);x=r[0]+r[2]*.2f;y=r[1]+r[3]/2;
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);
        touch(AMOTION_EVENT_ACTION_MOVE,x,y-3.3f*rh,W,H);
        touch(AMOTION_EVENT_ACTION_UP,x,y-3.3f*rh,W,H);
        assert(atomic_load(&g_menu_scroll)==3*OVM_SCROLL_UNITS && actions==0); // no accidental export on swipe
        ov_menu_rowrect(p,5,r); x=r[0]+r[2]*.2f; y=r[1]+r[3]/2;
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H); touch(AMOTION_EVENT_ACTION_UP,x,y,W,H);
        assert(last_action==OVM_SIZE && actions==1);
        ov_menu_rowrect(p,4,r); x=r[0]+r[2]*.2f; y=r[1]+r[3]/2;
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);
        touch(AMOTION_EVENT_ACTION_MOVE,x,y+20*rh,W,H);
        touch(AMOTION_EVENT_ACTION_UP,x,y+20*rh,W,H);
        assert(atomic_load(&g_menu_scroll)==0 && actions==1); // clamps and does not close outside
        atomic_store(&g_menu_hi,-1);
        for (int i=0;i<OVM_ROWS*2;i++) {
            menu_key(DM_BTN_DOWN);
            int hi=atomic_load(&g_menu_hi);float offset=ov_menu_scroll_rows(atomic_load(&g_menu_scroll));
            assert(ov_menu_item(1,hi)>=0 && ov_menu_scroll_rowrect(p,1,offset,hi,r));
            if(hi>0 && hi<OVM_ROWS-1) {float slot=ov_menu_scroll_slot(1,offset,hi);assert(slot>=1 && slot+1<=OVM_VIEW_ROWS-1);}
        }
        atomic_store(&g_menu_scroll,3*OVM_SCROLL_UNITS); atomic_store(&g_menu_hi,OVM_ROWS-2); menu_key(DM_BTN_A);
        assert(last_action==OVM_SIZE);
        menu_tab(0); assert(atomic_load(&g_menu_scroll)==0);
        ov_menu_rowrect(p,2,r); float track[3]; ov_menu_track(p,2,track);
        x=track[0]; y=track[1];
        touch(AMOTION_EVENT_ACTION_DOWN,x,y,W,H);
        touch(AMOTION_EVENT_ACTION_MOVE,x+track[2],y-rh,W,H);
        touch(AMOTION_EVENT_ACTION_UP,x+track[2],y-rh,W,H);
        assert(atomic_load(&g_set_opacity)==80 && atomic_load(&g_menu_scroll)==0);
        assert(ov_menu_scroll_slot(1,0,0)==0 && ov_menu_scroll_slot(1,2,0)==0);
        assert(ov_menu_scroll_slot(1,0,OVM_ROWS-1)==OVM_VIEW_ROWS-1);
        assert(ov_menu_scroll_slot(1,2,OVM_ROWS-1)==OVM_VIEW_ROWS-1);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_menu_scroll_') as tmp:
            tmp = Path(tmp)
            overflow_fixture(tmp)
            (tmp / 'android_profile.h').write_text(android_profile.header(None))
            src, exe = tmp / 'check.c', tmp / 'check'
            src.write_text(code, encoding='utf-8')
            build = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I'+str(tmp), '-I'+str(DM/'android'), '-I'+str(DM/'host'), str(src), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
