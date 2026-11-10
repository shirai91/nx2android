"""Run the real alias selector, mask-transition and clear routing code on the host."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'),'host C compiler required')
class AttachmentAlias(unittest.TestCase):
    def test_inactive_aliases_mask_transitions_and_clears(self):
        source=(ROOT/'nxnative/dm/android/gfx_vk_exec.c').read_text()
        start=source.index('static void color_alias_map(')
        helpers=source[start:source.index('// Render passes.',start)]
        start=source.index('static void clear_color(const p_clear_color *c)')
        clear=source[start:source.index('static void clear_ds(',start)]
        code=r'''
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef struct {int32_t index,mask;float rgba[4];} p_clear_color;
typedef struct {int is_int,is_sint;} vkfmt_t;
static struct {int pass_open,ncol;struct {uint32_t mask;} chmask;VkFormat cfmt[8];
 int rt_has_cview[8];struct {int set_format,format;} rt_cview[8];struct {struct {int format;} d;} rt_col[8];} S;
static VkImageView P_alias_views[8];static uint8_t P_alias[8];
static VkRenderingAttachmentInfo P_ca[8];static VkRenderingInfo P_ri;static int P_cleared,ended,clears;static uint32_t cleared_slot;
static struct {unsigned folded;} g_vkst;
static void vk_end_pass(void) {ended++;S.pass_open=0;}
static int begin_pass(void) {return 1;}
static int pass_commit(void) {return 1;}
static int clear_masked(const p_clear_color *c,VkRect2D r) {(void)c;(void)r;return 0;}
static VkRect2D scissor_rect(void) {return (VkRect2D){{0,0},{100,100}};}
static int pass_whole(VkRect2D r) {return r.extent.width==100;}
static const vkfmt_t *vk_fmt(int format) {(void)format;return NULL;}
static VkCommandBuffer vk_cmd(void) {return VK_NULL_HANDLE;}
static void fake_clear(VkCommandBuffer cb,uint32_t na,const VkClearAttachment *a,uint32_t nr,const VkClearRect *r) {
 (void)cb;(void)nr;(void)r;assert(na==1);clears++;cleared_slot=a->colorAttachment;
}
#define vkCmdClearAttachments fake_clear
#define VKRED_ONCE(...) ((void)0)
''' + helpers + clear + r'''
int main(void) {
 VkImageView views[8]={0};uint8_t map[8];views[0]=(VkImageView)(uintptr_t)1;
 views[3]=views[4]=(VkImageView)(uintptr_t)2;views[7]=(VkImageView)(uintptr_t)3;
 color_alias_map(8,views,0x0000f000u,map);assert(map[3]==3 && map[4]==3 && map[0]==0 && map[7]==7);
 color_alias_map(8,views,0x000f0000u,map);assert(map[3]==4 && map[4]==4);
 color_alias_map(8,views,0,map);assert(map[3]==3 && map[4]==3); // no writer: one load/store
 color_alias_map(8,views,0xffffffffu,map);assert(map[3]==3 && map[4]==4); // never discard a writer
 views[4]=(VkImageView)(uintptr_t)4; // another view, e.g. a different layer of the image
 color_alias_map(8,views,0x0000f000u,map);assert(map[3]==3 && map[4]==4);
 views[4]=views[3];views[6]=views[3];
 color_alias_map(8,views,0x0000f000u,map);assert(map[3]==3 && map[4]==3 && map[6]==3);
 color_alias_map(0,views,0,map);for(int i=0;i<8;i++)assert(map[i]==i);
 memcpy(P_alias_views,views,sizeof views);S.ncol=8;S.pass_open=1;S.chmask.mask=0x0000f000u;
 color_alias_map(8,views,S.chmask.mask,P_alias);
 bind_chmask(0x0000a000u);assert(ended==0); // channel subset, same writer
 bind_chmask(0x000f0000u);assert(ended==1 && S.pass_open==0 && S.chmask.mask==0x000f0000u);
 bind_chmask(0);assert(ended==1); // no open pass
 // Restore the slot-3 writer. Clearing logical alias 4 must clear physical 3,
 // both when folded into the load op and after rendering has begun.
 S.ncol=5;S.chmask.mask=0x0000f000u;color_alias_map(5,views,S.chmask.mask,P_alias);
 S.cfmt[3]=VK_FORMAT_A2B10G10R10_UNORM_PACK32;S.cfmt[4]=VK_FORMAT_UNDEFINED;S.pass_open=2;
 p_clear_color c={4,15,{.25f,.5f,.75f,1}};clear_color(&c);
 assert(P_ca[3].loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR && P_ca[3].clearValue.color.float32[1]==.5f);
 assert(P_cleared && g_vkst.folded==1 && clears==0 && c.index==4);
 S.pass_open=1;clear_color(&c);assert(clears==1 && cleared_slot==3);
 c.index=-1;clear_color(&c);c.index=8;clear_color(&c);assert(clears==1);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src=Path(folder)/'test.c';src.write_text(code);exe=Path(folder)/'test'
            result=subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'nxnative/dm/third_party/vulkan-headers/include'),str(src),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
