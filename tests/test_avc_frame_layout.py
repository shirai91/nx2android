"""Synthetic oracles adapted from nxnative t_android_avc_metadata.c (1f01e28c)."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'nxnative/dm/android/movie_avc.c'

def function(text, signature):
    at=text.index(signature)
    return text[at:text.index('\n}',at)+2]+'\n'

@unittest.skipUnless(shutil.which('cc'),'host C compiler required')
class AVCFrameLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source=SOURCE.read_text(encoding='utf-8')
        # Include production declarations and SPS helpers when color metadata lands.
        state=source[source.index('#define QMAX'):source.index('mavc *mavc_new(')]
        code=r'''
#define __USE_MINGW_ANSI_STDIO 1
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#define LOG(...) (fprintf(stderr,__VA_ARGS__),fputc(10,stderr))
typedef struct AMediaCodec AMediaCodec;
typedef struct mavc mavc;
static void die(const char *fmt,...) {
    va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);fputc(10,stderr);exit(77);
}
static int fail_picture_allocation;
static void *picture_malloc(size_t n) {return fail_picture_allocation?NULL:malloc(n);}
''' + state + r'''
#define AMEDIAFORMAT_KEY_WIDTH "width"
#define AMEDIAFORMAT_KEY_HEIGHT "height"
#define AMEDIAFORMAT_KEY_STRIDE "stride"
#define AMEDIAFORMAT_KEY_SLICE_HEIGHT "slice-height"
#define AMEDIAFORMAT_KEY_COLOR_FORMAT "color-format"
#define AMEDIAFORMAT_KEY_DISPLAY_CROP "crop"
typedef struct {int width,height,stride,slice,color,has_stride,has_slice,has_crop,l,t,r,b;} AMediaFormat;
static AMediaFormat metadata;
static AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *codec) {(void)codec;return &metadata;}
static bool AMediaFormat_getInt32(AMediaFormat *f,const char *key,int32_t *value) {
    if(!strcmp(key,"width"))*value=f->width;else if(!strcmp(key,"height"))*value=f->height;
    else if(!strcmp(key,"stride")){if(!f->has_stride)return false;*value=f->stride;}
    else if(!strcmp(key,"slice-height")){if(!f->has_slice)return false;*value=f->slice;}
    else if(!strcmp(key,"color-format"))*value=f->color;
    else return false;
    return true;
}
static bool AMediaFormat_getRect(AMediaFormat *f,const char *key,int32_t *l,int32_t *t,int32_t *r,int32_t *b) {
    (void)key;if(!f->has_crop)return false;*l=f->l;*t=f->t;*r=f->r;*b=f->b;return true;
}
static void AMediaFormat_delete(AMediaFormat *f) {(void)f;}
''' + function(source,'static void read_format(') + '#define malloc picture_malloc\n' + function(source,'static void keep_picture(') + '#undef malloc\n' + function(source,'static void nv12_rgba(') + r'''
static void finish(mavc *a) {(void)a;}
static int drain(mavc *a,int64_t wait) {(void)a;(void)wait;return 0;}
''' + function(source,'int mavc_take(') + r'''
static mavc format(int layout) {
    return (mavc){.fw=6,.fh=6,.fstride=8,.fslice=8,.fcolor=layout,.cl=2,.ct=2,.cr=4,.cb=4};
}
static void cropped(int layout,int rgba) {
    uint8_t src[96],out[97];memset(src,0xee,sizeof src);memset(out,0xbb,sizeof out);
    const uint8_t yplane[]={46,75,100,82,110,135,155,135,112};
    const uint8_t up[]={88,160,85,140},vp[]={96,148,165,115};
    for(int y=0;y<3;y++)memcpy(src+(2+y)*8+2,yplane+3*y,3);
    int planar=layout==19,cs=planar?4:8,step=planar?1:2;
    int u=64+cs+step,v=u+(planar?16:1);
    for(int y=0;y<2;y++)for(int x=0;x<2;x++){src[u+y*cs+x*step]=up[y*2+x];src[v+y*cs+x*step]=vp[y*2+x];}
    mavc a=format(layout);a.rgba=rgba;
    keep_picture(&a,src,v+cs+step,42);assert(!a.nq); // missing exactly the final V byte
    keep_picture(&a,src,v+cs+step,42);assert(!a.nq); // repeated drop logs only once
    keep_picture(&a,src,v+cs+step+1,42);assert(a.nq==1 && a.q[0].w==3 && a.q[0].h==3);
    int64_t pts=0;assert(mavc_take(&a,0,out,6,4,&pts) && pts==42 && !a.nq);
    assert(out[rgba?96:36]==0xbb);
    for(int y=0;y<4;y++)for(int x=0;x<6;x++){
        int inside=x<3 && y<3,yy=inside?yplane[y*3+x]:16;
        if(!rgba){assert(out[y*6+x]==yy);continue;}
        int i=2*(y/2)+x/2,cb=inside?up[i]-128:0,cr=inside?vp[i]-128:0;
        int expect[3]={298*(yy-16)+409*cr,298*(yy-16)-100*cb-208*cr,298*(yy-16)+516*cb};
        for(int c=0;c<3;c++){int b=(expect[c]+128)>>8;b=b<0?0:b>255?255:b;assert(out[4*(y*6+x)+c]==b);}
        assert(out[4*(y*6+x)+3]==255);
    }
    if(!rgba)for(int y=0;y<2;y++)for(int x=0;x<6;x+=2){
        int i=y*2+x/2,j=24+y*6+x;assert(out[j]==(x<4?up[i]:128) && out[j+1]==(x<4?vp[i]:128));
    }
}
static void invalid(int which) {
    uint8_t src[2048]={0};mavc a=format(19);
    switch(which){
    case 0:a.cl=-2;break;case 1:a.ct=-2;break;case 2:a.cl=1;break;case 3:a.ct=1;break;
    case 4:a.cr=6;break;case 5:a.cb=6;break;case 6:a.fstride=4;break;case 7:a.fslice=4;break;
    case 8:a.fstride=32769;break;case 9:a.fslice=32769;break;case 10:a.fw=INT32_MAX;break;
    case 11:a.fw=0;break;case 12:a.fh=0;break;case 13:a.cr=1;break;case 14:a.cb=1;break;
    }
    keep_picture(&a,src,sizeof src,0);assert(!a.nq);
    keep_picture(&a,src,sizeof src,0);assert(!a.nq);
}
static void odd_planar_stride(void) {
    // 5x5 I420 has 3-byte chroma rows and 3-row chroma planes.
    uint8_t src[43];for(unsigned i=0;i<sizeof src;i++)src[i]=(uint8_t)i;
    mavc a={.fw=5,.fh=5,.fstride=5,.fslice=5,.fcolor=19,.cr=4,.cb=4};
    keep_picture(&a,src,sizeof src,11);assert(a.nq==1 && a.q[0].w==5 && a.q[0].h==5);
    for(int i=0;i<25;i++)assert(a.q[0].nv12[i]==i);
    for(int i=0;i<9;i++){assert(a.q[0].nv12[25+2*i]==25+i);assert(a.q[0].nv12[26+2*i]==34+i);}
    free(a.q[0].nv12);
}
static void metadata_case(int mode) {
    metadata=(AMediaFormat){.width=6,.height=6,.color=19};
    switch(mode){
    case 31:metadata.has_slice=1;break;
    case 32:metadata.has_crop=1;metadata.l=1;metadata.r=4;metadata.b=4;break;
    case 33:metadata.has_crop=1;metadata.l=4;metadata.r=2;metadata.b=4;break;
    case 34:metadata.has_stride=1;break;
    case 35:metadata.width=INT32_MIN;break;
    case 36:metadata.has_slice=1;metadata.slice=-1;break;
    case 37:metadata.has_crop=1;metadata.r=6;metadata.b=5;break;
    case 38:metadata.has_crop=1;metadata.l=-2;metadata.r=5;metadata.b=5;break;
    case 39:metadata.has_stride=metadata.has_slice=1;metadata.stride=metadata.slice=4;break;
    }
    mavc a={0};uint8_t bytes[54];for(int i=0;i<54;i++)bytes[i]=(uint8_t)i;
    read_format(&a);keep_picture(&a,bytes,sizeof bytes,0);
    assert(a.nq==(mode!=32 && mode!=35));
    if(a.nq){
        assert(a.fstride==6 && a.fslice==6 && a.q[0].w==6 && a.q[0].h==6);
        for(int i=0;i<36;i++)assert(a.q[0].nv12[i]==i);
        for(int i=0;i<9;i++){assert(a.q[0].nv12[36+2*i]==36+i);assert(a.q[0].nv12[37+2*i]==45+i);}
        free(a.q[0].nv12);
    }
}
int main(int argc,char **argv){assert(argc==2);int mode=atoi(argv[1]);
    if(mode==50){uint8_t bytes[96]={0};mavc a=format(19);fail_picture_allocation=1;keep_picture(&a,bytes,sizeof bytes,0);return 0;}
    if(mode>=40){cropped(mode==40?0x7f420888:mode==41?0x1234:0,0);return 0;}
    if(mode>=30){metadata_case(mode);return 0;}
    if(mode<6)cropped(mode/2==0?19:mode/2==1?21:0x7fa30c04,mode%2);
    else if(mode==6)odd_planar_stride();else invalid(mode-7);
}
'''
        cls.temp=tempfile.TemporaryDirectory(prefix='nx2a-avc-layout-')
        cls.addClassCleanup(cls.temp.cleanup)
        c=Path(cls.temp.name)/'test.c';cls.binary=c.with_suffix('.exe' if os.name=='nt' else '');c.write_text(code,encoding='utf-8')
        flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.getenv('NX2A_TEST_SANITIZE') else []
        subprocess.run(['cc','-std=c11','-O1','-Wall','-Werror','-Wno-unused-function',*flags,str(c),'-o',str(cls.binary)],check=True,capture_output=True,text=True)

    def run_case(self,case):
        r=subprocess.run([str(self.binary),str(case)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stderr)
        return r.stderr

    def test_cropped_i420_nv12_and_venus_last_byte_and_odd_visible_extent(self):
        for mode in range(6):
            with self.subTest(mode=mode):
                log=self.run_case(mode)
                self.assertEqual(log.count('[RED]'),1,log)
                for field in ('bytes','stride 8','slice height 8','crop 2,2-4,4'):
                    self.assertIn(field,log)

    def test_unknown_formats_keep_nv12_fallback_and_log_once_beside_drop(self):
        for mode in (40,41,42):
            with self.subTest(mode=mode):
                log=self.run_case(mode)
                self.assertEqual(log.count('[RED]'),2,log)
                self.assertEqual(log.count('NV12'),1,log)
                self.assertIn('crop 2,2-4,4',log)

    def test_picture_allocation_failure_stops_with_diagnostic(self):
        r=subprocess.run([str(self.binary),'50'],capture_output=True,text=True)
        self.assertEqual(r.returncode,77,'failed picture allocation did not call die(): '+r.stderr)
        self.assertIn('movie: AVC picture allocation failed',r.stderr)

    def test_odd_planar_stride_and_plane_height(self):self.run_case(6)

    def test_format_defaults_and_crop_stride_slice_fallbacks(self):
        for mode in range(30,40):
            with self.subTest(mode=mode):self.run_case(mode)

    def test_rejects_invalid_geometry_with_one_diagnostic(self):
        for mode in range(7,22):
            with self.subTest(mode=mode):
                log=self.run_case(mode)
                self.assertEqual(log.count('[RED]'),1,log)
                for field in ('bytes','stride','slice height','crop'):
                    self.assertIn(field,log)

if __name__=='__main__':unittest.main()
