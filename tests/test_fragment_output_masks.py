"""Existing donor MRT output eligibility using the target's attachment loop."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class FragmentOutputMasks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source=(ROOT/'nxnative/dm/android/gfx_vk_exec.c').read_text(encoding='utf-8')
        begin=source.index('    // attachments and blending\n')
        end=source.index('    // Dual-source blending:',begin)
        code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned VkFormat;
typedef struct {int is_int;} vkfmt_t;
static const vkfmt_t formats[]={{0},{0},{1}};
static const vkfmt_t *vk_fmt(unsigned f){return &formats[f];}
typedef struct {uint32_t out_locs;} shader;
static struct {int ncol;uint32_t cfmt[8],dfmt,samples;struct {int a2c;} ms;
    struct {unsigned logic_op,blend_en;} color;struct {unsigned mask;} chmask;
    shader *sh[3];int rt_has_col[8];struct {struct {unsigned format;} d;} rt_col[8];
    struct {int src_rgb,dst_rgb,src_a,dst_a,eq_rgb,eq_a;} blend[8];} S;
static struct {unsigned debug_mode,alpha,raster_discard,ncol,dfmt,samples,a2c,logic_en,logic_op,cfmt[8],wmask[8],blend_en;
    struct {int src,dst,srca,dsta,eq,eqa;} bl[8];} k;
static int alpha_enabled(void *fs,unsigned format,int discard){(void)fs;(void)format;(void)discard;return 0;}
static void color_logic(void *key){(void)key;}
static void prepare(void){
''' + source[begin:end] + r'''
}
int main(int argc,char **argv){assert(argc==2);int mode=atoi(argv[1]);
    shader fs={.out_locs=5};S.ncol=4;S.sh[1]=mode==1?NULL:&fs;S.color.blend_en=15;S.chmask.mask=0xffffffff;
    for(int i=0;i<4;i++){S.cfmt[i]=1;S.rt_has_col[i]=1;S.rt_col[i].d.format=1;S.blend[i].src_rgb=1;}
    if(mode==2){fs.out_locs=10;S.chmask.mask=0x1234;S.rt_col[3].d.format=2;}
    if(mode==3){fs.out_locs=10;k.debug_mode=1;}
    prepare();
    unsigned masks[4][4]={{15,0,15,0},{0,0,0,0},{0,3,0,1},{15,15,15,15}};
    unsigned blends[]={5,0,2,15};
    for(int i=0;i<4;i++)assert(k.wmask[i]==masks[mode][i]);
    assert(k.blend_en==blends[mode]);
}
'''
        cls.temp=tempfile.TemporaryDirectory(prefix='nx2a-output-mask-');cls.addClassCleanup(cls.temp.cleanup)
        source=Path(cls.temp.name)/'test.c';source.write_text(code, encoding='utf-8');cls.exe=source.with_suffix('')
        subprocess.run(['cc','-std=c11','-O1',str(source),'-o',str(cls.exe)],check=True,capture_output=True,text=True)

    def test_unwritten_attachments_disable_color_and_blending(self):
        for mode in range(4):
            with self.subTest(mode=mode):
                result=subprocess.run([str(self.exe),str(mode)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)

if __name__=='__main__':unittest.main()
