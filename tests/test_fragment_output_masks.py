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
        at=source.index('static int att_int_kind(');helper=source[at:source.index('\n}\n',at)+3]
        code=r'''
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint32_t out_locs;} shader;
typedef struct {int w;} vkimg_t;
static vkimg_t g_s0_image;
static struct {vkimg_t *im;} P_att[9];
static struct {int ncol;VkFormat cfmt[8],dfmt;uint32_t samples;struct {int a2c;} ms;
    struct {unsigned logic_op,blend_en;} color;struct {unsigned mask;} chmask;
    shader *sh[3];int rt_has_col[8];
    struct {int src_rgb,dst_rgb,src_a,dst_a,eq_rgb,eq_a;} blend[8];} S;
static struct {unsigned debug_mode,ncol,dfmt,samples,a2c,logic_en,logic_op,cfmt[8],wmask[8],blend_en;
    struct {int src,dst,srca,dsta,eq,eqa;} bl[8];} k;
''' + helper + r'''
static void prepare(void){
''' + source[begin:end] + r'''
}
int main(int argc,char **argv){assert(argc==2);int mode=atoi(argv[1]);
    shader fs={.out_locs=5};S.ncol=4;S.sh[1]=mode==1?NULL:&fs;S.color.blend_en=15;S.chmask.mask=0xffffffff;
    for(int i=0;i<4;i++){S.cfmt[i]=VK_FORMAT_R8G8B8A8_UNORM;S.rt_has_col[i]=1;S.blend[i].src_rgb=1;}
    if(mode==2){fs.out_locs=10;S.chmask.mask=0x1234;S.cfmt[3]=VK_FORMAT_R32_UINT;}
    if(mode==3){fs.out_locs=10;k.debug_mode=1;}
    if(mode==4)P_att[0].im=&g_s0_image;
    // blending follows the format the attachment has (S.cfmt), whatever the texture's or the view's NVN format
    if(mode==5)S.cfmt[0]=VK_FORMAT_R16G16B16A16_UINT;   // an integer attachment: no blending
    if(mode==6)S.cfmt[2]=VK_FORMAT_A2B10G10R10_SINT_PACK32;
    prepare();
    unsigned masks[7][4]={{15,0,15,0},{0,0,0,0},{0,3,0,1},{15,15,15,15},{0,0,15,0},{15,0,15,0},{15,0,15,0}};
    unsigned blends[]={5,0,2,15,4,4,1};
    for(int i=0;i<4;i++)assert(k.wmask[i]==masks[mode][i]);
    assert(k.blend_en==blends[mode]);
}
'''
        cls.temp=tempfile.TemporaryDirectory(prefix='nx2a-output-mask-');cls.addClassCleanup(cls.temp.cleanup)
        source=Path(cls.temp.name)/'test.c';source.write_text(code, encoding='utf-8');cls.exe=source.with_suffix('')
        subprocess.run(['cc','-std=c11','-O1','-I',str(ROOT/'nxnative/dm/third_party/vulkan-headers/include'),str(source),'-o',str(cls.exe)],check=True,capture_output=True,text=True)

    def test_unwritten_attachments_disable_color_and_blending(self):
        for mode in range(7):
            with self.subTest(mode=mode):
                result=subprocess.run([str(self.exe),str(mode)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)

if __name__=='__main__':unittest.main()
