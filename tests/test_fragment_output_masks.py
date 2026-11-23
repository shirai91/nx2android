"""Existing donor MRT output eligibility using the target's attachment loop."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class FragmentOutputMasks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source=csrc.unit((ROOT/'nxnative/dm/android/gfx_vk_exec.c'))
        begin=source.index('    // attachments and blending\n')
        end=source.index('    // Dual-source blending:',begin)
        at=source.index('static const vkfmt_t *rt_view_fmt');helper=source[at:source.index('\n}\n',at)+3]
        code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct {int is_int;} vkfmt_t;
static const vkfmt_t formats[]={{0},{0},{1}};
static const vkfmt_t *vk_fmt(unsigned f){return &formats[f];}
typedef struct {uint32_t out_locs;} shader;
typedef struct {int w;} vkimg_t;
static vkimg_t g_s0_image;
static struct {vkimg_t *im;} P_att[9];
static struct {int ncol;uint32_t cfmt[8],dfmt,samples;struct {int a2c;} ms;
    struct {unsigned logic_op,blend_en;} color;struct {unsigned mask;} chmask;
    shader *sh[3];int rt_has_col[8];struct {struct {unsigned format;} d;} rt_col[8];
    int rt_has_cview[8];struct {int set_format;unsigned format;} rt_cview[8];
    struct {int src_rgb,dst_rgb,src_a,dst_a,eq_rgb,eq_a;} blend[8];} S;
static struct {unsigned debug_mode,ncol,dfmt,samples,a2c,logic_en,logic_op,cfmt[8],wmask[8],blend_en;
    struct {int src,dst,srca,dsta,eq,eqa;} bl[8];} k;
''' + helper + r'''
static void prepare(void){
''' + source[begin:end] + r'''
}
int main(int argc,char **argv){assert(argc==2);int mode=atoi(argv[1]);
    shader fs={.out_locs=5};S.ncol=4;S.sh[1]=mode==1?NULL:&fs;S.color.blend_en=15;S.chmask.mask=0xffffffff;
    for(int i=0;i<4;i++){S.cfmt[i]=1;S.rt_has_col[i]=1;S.rt_col[i].d.format=1;S.blend[i].src_rgb=1;}
    if(mode==2){fs.out_locs=10;S.chmask.mask=0x1234;S.rt_col[3].d.format=2;}
    if(mode==3){fs.out_locs=10;k.debug_mode=1;}
    if(mode==4)P_att[0].im=&g_s0_image;
    if(mode==5){S.rt_has_cview[0]=1;S.rt_cview[0].set_format=1;S.rt_cview[0].format=2;}  // float texture, integer view: no blending
    if(mode==6){S.rt_col[2].d.format=2;S.rt_has_cview[2]=1;S.rt_cview[2].set_format=1;S.rt_cview[2].format=1;}  // integer texture, float view: blends
    prepare();
    unsigned masks[7][4]={{15,0,15,0},{0,0,0,0},{0,3,0,1},{15,15,15,15},{0,0,15,0},{15,0,15,0},{15,0,15,0}};
    unsigned blends[]={5,0,2,15,4,4,5};
    for(int i=0;i<4;i++)assert(k.wmask[i]==masks[mode][i]);
    assert(k.blend_en==blends[mode]);
}
'''
        cls.temp=tempfile.TemporaryDirectory(prefix='nx2a-output-mask-');cls.addClassCleanup(cls.temp.cleanup)
        source=Path(cls.temp.name)/'test.c';source.write_text(code, encoding='utf-8');cls.exe=source.with_suffix('')
        subprocess.run(['cc','-std=c11','-O1',str(source),'-o',str(cls.exe)],check=True,capture_output=True,text=True)

    def test_unwritten_attachments_disable_color_and_blending(self):
        for mode in range(7):
            with self.subTest(mode=mode):
                result=subprocess.run([str(self.exe),str(mode)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)

    def test_clear_value_follows_the_view(self):   # 1.0 into a UINT view of a float texture is integer 1, not the bits of 1.0
        source=csrc.unit((ROOT/'nxnative/dm/android/gfx_vk_exec.c'))
        fn=source.index('static void clear_color(');begin=source.index('    VkClearAttachment a =',fn);end=source.index('\n',source.index('if (f && f->is_int) for',begin))
        at=source.index('static const vkfmt_t *rt_view_fmt');helper=source[at:source.index('\n}\n',at)+3]
        code=r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {int is_int,is_sint;} vkfmt_t;
static const vkfmt_t formats[]={{0,0},{1,0}};
static const vkfmt_t *vk_fmt(unsigned f){return &formats[f];}
static struct {struct {struct {unsigned format;} d;} rt_col[8];int rt_has_cview[8];struct {int set_format;unsigned format;} rt_cview[8];} S;
enum {VK_IMAGE_ASPECT_COLOR_BIT=1};
typedef struct {int aspect;uint32_t index;struct {union {float float32[4];int32_t int32[4];uint32_t uint32[4];} color;} clearValue;} VkClearAttachment;
typedef struct {int index;float rgba[4];} p_clear_color;
'''+helper+r'''
int main(void){p_clear_color cc={0,{1,1,1,1}},*c=&cc;S.rt_has_cview[0]=1;S.rt_cview[0].set_format=1;S.rt_cview[0].format=1;
'''+source[begin:end]+r'''
    assert(a.clearValue.color.uint32[0]==1);return 0;}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a-clear-view-') as t:
            c=Path(t)/'c.c';c.write_text(code,encoding='utf-8');exe=c.with_suffix('')
            made=subprocess.run(['cc','-std=c11','-O1',str(c),'-o',str(exe)],capture_output=True,text=True);self.assertEqual(made.returncode,0,made.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stderr)

if __name__=='__main__':unittest.main()
