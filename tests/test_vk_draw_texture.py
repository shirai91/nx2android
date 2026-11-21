"""DrawTexture recorder and copied rectangle clipping; synthetic inputs only."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
DM=ROOT/'nxnative/dm'


def function(source,signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]+'\n'


@unittest.skipUnless(shutil.which('cc'),'requires host C compiler')
class DrawTexture(unittest.TestCase):
    def run_c(self,source):
        with tempfile.TemporaryDirectory(prefix='draw-texture-') as td:
            c=Path(td)/'test.c'; exe=Path(td)/'test'; c.write_text(source)
            args=['cc','-std=c11','-Wall','-Werror','-Wno-unused-function']
            args+=['-I'+str(DM/p) for p in ('gfx/nvn','gfx/nvn/include','android')]
            result=subprocess.run(args+[str(c),'-lm','-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_record_copies_regions_and_rebases_low_word_handle(self):
        source=(DM/'gfx/nvn/hle_nvn.c').read_text()
        signature='NVN(nvnCommandBufferDrawTexture, void,'
        self.assertTrue(signature in source,'public DrawTexture recorder missing')
        api=function(source,signature)
        self.run_c(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_obj.h"
_Static_assert(sizeof(NVNdrawTextureRegion)==16 && sizeof(p_draw_tex)==48,"DrawTexture ABI");
static uint64_t mtl_samp_seq(void) { return 1234; }
static p_draw_tex recorded;
static unsigned calls;
static void *record(NVNcommandBuffer *c,unsigned op,size_t n) {
    assert(c && op==OP_DRAW_TEX && n==sizeof recorded);calls++;return &recorded;
}
#define CB(c) ((cmd_t *)(c))
#define REC(c,op,T) ((T *)record(c,op,sizeof(T)))
#define NVN(name,ret,...) ret name(__VA_ARGS__)
''' + api + r'''
int main(void) {
    cmd_t state={.tex_base=512,.samp_base=128};
    NVNdrawTextureRegion dst={7.25f,5.25f,1.25f,1.25f},src={.75f,.5f,3.75f,2.5f};
    for(unsigned tex=0;tex<2;tex++) for(unsigned samp=0;samp<2;samp++) {
        uint64_t h=0xfefefefe00000000ull | (tex?5:0) | (uint64_t)(samp?3:0)<<20;
        nvnCommandBufferDrawTexture((NVNcommandBuffer *)&state,h,&dst,&src);
        assert(recorded.h==((tex?517:0) | (uint64_t)(samp?131:0)<<20));
        assert(!memcmp(&recorded.d,&dst,sizeof dst) && !memcmp(&recorded.s,&src,sizeof src));
    }
    assert(calls==4 && recorded.sseq==1234);memset(&dst,0,sizeof dst);memset(&src,0,sizeof src);
    assert(recorded.d.x0==7.25f && recorded.s.x1==3.75f);
    assert(OP_DRAW_TEX>OP_DRAW_TF);
}
''')

    def test_fractional_flipped_and_clipped_pixel_centers(self):
        path=DM/'android/gfx_vk_draw_texture.inc'
        self.assertTrue(path.exists(),'ported DrawTexture implementation missing')
        helper=function(path.read_text(),'static VkRect2D draw_texture_rect(')
        self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include "nvn_obj.h"
typedef struct {uint32_t width,height;} VkExtent2D;
typedef struct {struct {int32_t x,y;} offset;VkExtent2D extent;} VkRect2D;
static struct {int sc_set;p_scissor sc;} S;
''' + helper + r'''
static void check(NVNdrawTextureRegion d,int x,int y,unsigned w,unsigned h) {
    VkRect2D r=draw_texture_rect(&d,(VkExtent2D){8,6});
    assert(r.offset.x==x && r.offset.y==y && r.extent.width==w && r.extent.height==h);
}
int main(void) {
    check((NVNdrawTextureRegion){0,0,8,6},0,0,8,6);
    check((NVNdrawTextureRegion){7.25f,5.25f,1.25f,1.25f},1,1,6,4);
    S.sc_set=1; S.sc=(p_scissor){.x=2,.y=0,.w=4,.h=6};
    check((NVNdrawTextureRegion){7.25f,5.25f,1.25f,1.25f},2,1,4,4);
    check((NVNdrawTextureRegion){-20,-20,40,40},2,0,4,6);
    check((NVNdrawTextureRegion){-5,-4,-1,-1},0,0,0,0);
    check((NVNdrawTextureRegion){1,1,1,5},0,0,0,0);
    S.sc_set=0;check((NVNdrawTextureRegion){.51f,.51f,1.49f,1.49f},1,1,0,0);
}
''')


if __name__=='__main__': unittest.main()
