"""Actual C pacing/selection regression: host compiler, or opt-in NDK + adb.

NX2A_TEST_NDK=<ndk> NX2A_TEST_ADB_SERIAL=<serial> enables the Android runner.
It runs only a standalone check in /data/local/tmp, never a game or APK.
"""
import os, pathlib, re, shutil, subprocess, sys, tempfile, unittest
src = pathlib.Path(__file__).resolve().parents[1] / 'nxnative/dm'

def function(text,name):
    match=re.search(re.escape(name)+r'[^;{]*\{',text);start=text.rfind('\n',0,match.start())+1;brace=match.end()-1;depth=1;i=brace+1
    while depth:
        depth+=(text[i]=='{')-(text[i]=='}');i+=1
    return text[start:i]
nvn=(src/'gfx/nvn/hle_nvn.c').read_text(encoding='utf-8');perf=(src/'android/perf_android.c').read_text(encoding='utf-8');present=(src/'android/gfx_vk_present.c').read_text(encoding='utf-8')
front=(src/'android/front_android.c').read_text(encoding='utf-8')
code='''#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "overlay_layout.h"
#include "frame_limit.h"
float dm_overlay_scale(void) {return 1;}
static int selected;
int dm_user_present_hz(void) __attribute__((weak));
static const int64_t STEPS[]={16666667,25000000,33333333};
static int g_step,g_adapt=-1;static uint64_t g_game_period;
typedef struct {int interval;} win_t;
static int g_hz=60;static _Atomic int g_game_itv=1;static _Atomic uint64_t g_itv2_since;
static uint64_t now_ns(void) {return 10000000000ull;}
static void present_cfg(const win_t *o) {(void)o;}
static int present_itv(const win_t *o) {return o->interval;}
static uint64_t g_refresh_ns, g_pace_anchor, g_pace_target, g_pace_ideal, g_pace_period;
'''+function(perf,'uint64_t dm_present_period_adapt(')+'\n'+function(nvn,'static uint64_t present_period(')+'\n'+function(present,'static uint64_t pace_snap(')+'\n'+function(present,'static uint64_t pace_next_tick(')+'''
int dm_user_present_hz(void) {return selected;}
static char g_root[512];
static _Atomic int g_set_overlay=1,g_set_rumble=1,g_set_fps,g_set_opacity=60,g_set_grid,g_set_limit;
static _Atomic int g_cust[2][2][DM_AOV_N][3];
static const char *const LAYOUT_NAMES[2]={"tablet","phone"};
static int ctl_named(const char *name) {(void)name;return -1;}
int dm_opmode(void) {return 1;}
#define LOG(...) ((void)0)
'''+function(front,'static void settings_load(')+'\n'+function(front,'static void settings_save(')+'''
int main(int argc,char **argv) {
 assert(argc==2);snprintf(g_root,sizeof g_root,"%s",argv[1]);
 char path[600],line[256];snprintf(path,sizeof path,"%s/settings.txt",g_root);
 for(int i=0;i<DM_FRAME_LIMIT_COUNT;i++) {
   int rate=DM_FRAME_LIMITS[i];remove(path);atomic_store(&g_set_limit,rate);settings_save();
   atomic_store(&g_set_limit,0);settings_load();assert(atomic_load(&g_set_limit)==rate);
 }
 remove(path);atomic_store(&g_set_limit,0);settings_save();
 FILE *f=fopen(path,"r");assert(f);while(fgets(line,sizeof line,f))assert(!strstr(line,"fps_limit="));fclose(f);
 f=fopen(path,"w");assert(f);fprintf(f,"fps_limit=75\\n");fclose(f);settings_load();assert(atomic_load(&g_set_limit)==0);remove(path);
 win_t w={1};selected=0;assert(present_period(&w)==16666667);
 selected=120;assert(present_period(&w)==8333333);
 selected=60;assert(present_period(&w)==16666666);
 selected=120;assert(present_period(&w)==8333333);
 w.interval=2;assert(present_period(&w)==16666666);
 selected=0;assert(present_period(&w)==33333333);
 for(int i=1;i<DM_FRAME_LIMIT_COUNT;i++) {
   int rate=DM_FRAME_LIMITS[i];selected=rate;w.interval=1;
   assert(present_period(&w)==1000000000ull/rate);
   assert(dm_frame_limit_valid(rate));
   assert(dm_frame_limit_step(rate,1,1)==DM_FRAME_LIMITS[(i+1)%DM_FRAME_LIMIT_COUNT]);
   assert(dm_frame_limit_step(rate,-1,0)==DM_FRAME_LIMITS[i>0?i-1:0]);
   g_refresh_ns=8333333;g_pace_anchor=1000000000;g_pace_period=1000000000ull/rate;
   g_pace_ideal=g_pace_target=g_pace_anchor;
   uint64_t first=g_pace_target,last=first;
   for(int j=0;j<300;j++){uint64_t t=pace_next_tick(g_pace_period);assert(t>last);last=t;g_pace_target=t;}
   int64_t error=(int64_t)(last-first)-(int64_t)(300*g_pace_period);
   assert(llabs(error)<=(int64_t)g_refresh_ns);
 }
 assert(dm_frame_limit_valid(0) && !dm_frame_limit_valid(75));
 assert(dm_frame_limit_step(120,1,0)==120 && dm_frame_limit_step(0,-1,0)==0);
 assert(pace_next_tick(20000000)==0);
 selected=0;g_step=1;assert(present_period(&w)==25000000);g_step=0;
 assert(dm_frame_limit_step(120,1,1)==0);
 assert(ov_menu_item(1,2)==OVM_LIMIT);assert(ov_menu_item(0,5)==-1);
 assert(ov_menu_item(1,OVM_ROWS-1)==OVM_RESUME);
 const float screens[][2]={{1920,1080},{1080,1920},{915,412}};
 for(int s=0;s<3;s++){float r[4],row[4];ov_menu_rect(screens[s][0],screens[s][1],r);
 assert(r[0]>=0 && r[1]>=0 && r[0]+r[2]<=screens[s][0]+1 && r[1]+r[3]<=screens[s][1]+1);
 for(int i=0;i<OVM_ROWS;i++){ov_menu_rowrect(r,i,row);assert(ov_menu_row_at(row[0]+row[2]/2,row[1]+row[3]/2,screens[s][0],screens[s][1])==i);}}
 puts("PASS: all six live limits, fractional 50/90 cadence, adaptive bypass, bounds and menu geometry");return 0;
}
'''

class FrameLimitTest(unittest.TestCase):
    def test_actual_pacing_and_menu(self):
        ndk = os.environ.get('NX2A_TEST_NDK')
        serial = os.environ.get('NX2A_TEST_ADB_SERIAL')
        cc = shutil.which('cc') or shutil.which('clang')
        if ndk and serial:
            host = 'windows-x86_64' if os.name == 'nt' else 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
            cc = str(pathlib.Path(ndk) / 'toolchains/llvm/prebuilt' / host / 'bin' / ('clang.exe' if os.name == 'nt' else 'clang'))
        elif not cc:
            self.skipTest('needs a host C compiler, or NX2A_TEST_NDK and NX2A_TEST_ADB_SERIAL')
        with tempfile.TemporaryDirectory() as tmp:
            t = pathlib.Path(tmp); c = t / 'check.c'; c.write_text(code, encoding='utf-8')
            (t / 'android_profile.h').write_text('#define DM_AOV_N 1\n#define DM_AOV_TABLET {0}\n#define DM_AOV_PHONE {0}\n', encoding='utf-8')
            exe = t / ('check' if ndk and serial else 'check.exe' if os.name == 'nt' else 'check')
            args = [cc] + (['--target=aarch64-linux-android31'] if ndk and serial else [])
            subprocess.run(args + ['-std=gnu11', '-I'+str(t), '-I'+str(src/'android'), '-I'+str(src/'host'), str(c), '-lm', '-o', str(exe)], check=True, capture_output=True)
            if ndk and serial:
                adb = os.environ.get('NX2A_TEST_ADB') or shutil.which('adb')
                self.assertTrue(adb, 'adb must be on PATH or NX2A_TEST_ADB')
                remote = '/data/local/tmp/nx2a-frame-limit-check'
                try:
                    subprocess.run([adb, '-s', serial, 'push', str(exe), remote], check=True, capture_output=True)
                    result = subprocess.run([adb, '-s', serial, 'shell', 'mkdir -p '+remote+'.data && chmod 700 '+remote+' && '+remote+' '+remote+'.data'], capture_output=True, text=True)
                finally:
                    subprocess.run([adb, '-s', serial, 'shell', 'rm -f '+remote+' '+remote+'.data/settings.txt '+remote+'.data/settings.txt.new; rmdir '+remote+'.data'], capture_output=True)
            else:
                result = subprocess.run([str(exe),str(t)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS:', result.stdout)

if __name__ == '__main__': unittest.main()
