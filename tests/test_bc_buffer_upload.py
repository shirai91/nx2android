"""Run the actual BC upload with separate GPU storage and stubbed Vulkan calls."""
from pathlib import Path
import subprocess
import tempfile
import unittest

import host_cc

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


@unittest.skipUnless(host_cc.posix(), 'POSIX host C compiler required')
class BCBufferUpload(unittest.TestCase):
    def test_gpu_snapshot_strides_layers_and_validation(self):
        source = (DM / 'android/gfx_vk_exec.c').read_text()
        functions = source[source.index('static VkImageSubresourceLayers sub_of('):
                           source.index('// Scaled copies of depth and stencil:')]
        code = r'''
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>
#include "nvn_bc.h"
typedef struct {int format;} desc_t;
typedef struct {desc_t d;} tex_t;
typedef struct {int set_levels,base_level,set_layers,min_layer;} view_t;
typedef struct {int xoffset,yoffset,zoffset,width,height,depth;} region_t;
typedef struct {tex_t *tex;view_t view;int has_view;region_t r;uint64_t src;} p_copy_b2t;
typedef struct {unsigned layers,levels,w,h,d,gpu_w;VkImageAspectFlags aspect;VkImage img;} vkimg_t;
typedef struct {unsigned dec,bw,bh,bpb,dbpp;} vkfmt_t;
typedef struct {uint64_t hi,end;} vkpool_t;
static vkimg_t image={.layers=3,.levels=5,.w=19,.h=13,.d=8,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.img=(VkImage)7};
static vkfmt_t format={.dec=5,.bw=4,.bh=4,.bpb=8,.dbpp=1};
static struct {int64_t row_stride,image_stride;} S={24,32};
static int64_t g_watch_off=-1;
static uint8_t gpu[64],cpu[64],scratch[128];
static vkpool_t pool;static unsigned wanted=50,fail_snapshot;
static unsigned snapshots,uploads,ended,phase,expect_error,span_limit=48;
static VkBufferImageCopy last;
static jmp_buf failure;
static void die(const char *s,...) {(void)s;assert(expect_error);longjmp(failure,1);}
#define VKRED_ONCE(...) ((void)0)
static void vk_end_pass_land(void) {ended++;phase=0;}
static vkimg_t *vk_tex(const tex_t *t) {(void)t;return &image;}
static const vkfmt_t *vk_fmt(int f) {assert(f==0x49);return &format;}
static int vk_buf_at(uint64_t a,size_t n,VkBuffer *b,VkDeviceSize *o) {
    if(a<(uintptr_t)cpu || n>span_limit)return 0;*b=(VkBuffer)8;*o=a-(uintptr_t)cpu;return 1;
}
static int vk_buffer_snapshot(VkBuffer b,VkDeviceSize off,size_t n,void *dst) {
    assert(b==(VkBuffer)8 && off==4 && n==48 && !phase);
    if(fail_snapshot)return 0;memcpy(dst,gpu+off,n);snapshots++;phase=1;return 1;
}
static void *vk_scratch(size_t n,size_t align,VkBuffer *b,VkDeviceSize *o) {
    assert(n==30 && align==16);*b=(VkBuffer)9;*o=16;phase=1;return scratch+16;
}
static VkCommandBuffer vk_cmd(void) {assert(phase==1);return (VkCommandBuffer)2;}
static void vk_tex_prepare(VkCommandBuffer cb,vkimg_t *im) {assert(cb==(VkCommandBuffer)2 && im==&image && phase==1);}
static void vk_img_barrier_all(VkCommandBuffer cb) {assert(cb==(VkCommandBuffer)2);}
static void vk_watch(vkimg_t *im,const char *s) {(void)im;(void)s;}
static vkpool_t *vk_pool_of(uint64_t a,uint64_t n) {return a>=(uintptr_t)cpu && a+n<=pool.end?&pool:NULL;}
static void vk_readback_unknown(void) {abort();}
void vk_gpu_wrote(uint64_t a,uint64_t n) {(void)a;(void)n;abort();}
void vkCmdCopyImageToBuffer(VkCommandBuffer c,VkImage i,VkImageLayout l,VkBuffer b,uint32_t n,const VkBufferImageCopy *r) {
    (void)c;(void)i;(void)l;(void)b;(void)n;(void)r;abort();
}
void vkCmdCopyBufferToImage(VkCommandBuffer cb,VkBuffer b,VkImage im,VkImageLayout l,uint32_t n,const VkBufferImageCopy *r) {
    assert(cb==(VkCommandBuffer)2 && b==(VkBuffer)9 && im==image.img && l==VK_IMAGE_LAYOUT_GENERAL && n==1);
    assert(r->bufferOffset==16 && !r->bufferRowLength && !r->bufferImageHeight);
    for(unsigned z=0;z<2;z++)for(unsigned y=0;y<3;y++)for(unsigned x=0;x<5;x++)
        assert(scratch[16+z*15+y*5+x]==wanted+z*30+(x==4));
    last=*r;uploads++;
}
''' + functions + r'''
int main(int argc,char **argv) {
    (void)argv;int enabled=argc>1;
    tex_t t={.d={0x49}};
    pool=(vkpool_t){(uintptr_t)cpu+32,(uintptr_t)cpu+64};
    p_copy_b2t c={.tex=&t,.view={1,1,1,1},.has_view=1,.r={4,0,0,5,3,2},.src=(uintptr_t)cpu+4};
    for(unsigned z=0;z<2;z++)for(unsigned x=0;x<2;x++) {gpu[4+z*32+x*8]=20+z*30+x;cpu[4+z*32+x*8]=50+z*30+x;}
    if(enabled)setenv("DM_VK_X_BC_GPU_READBACK","1",1);else unsetenv("DM_VK_X_BC_GPU_READBACK");
    wanted=enabled?20:50;copy_b2t(&c,1);
    if(!enabled) {
        assert(snapshots==0 && uploads==1);
        setenv("DM_VK_X_BC_GPU_READBACK","1",1);copy_b2t(&c,1);
        assert(snapshots==0 && uploads==2); // cached default stays off
        return 0;
    }
    setenv("DM_VK_X_BC_GPU_READBACK","0",1); // cached opt-in stays on
    assert(snapshots==1 && uploads==1 && image.gpu_w && last.imageSubresource.mipLevel==1);
    assert(last.imageSubresource.baseArrayLayer==1 && last.imageSubresource.layerCount==2 && last.imageExtent.depth==1);
    image.layers=1;c.view=(view_t){0};c.r.xoffset=0;c.r.zoffset=2;copy_b2t(&c,1);
    assert(last.imageSubresource.layerCount==1 && last.imageExtent.depth==2 && last.imageOffset.z==2);
    span_limit=47;wanted=50;copy_b2t(&c,1); // crosses chunk: valid CPU span, no abort
    assert(snapshots==2 && uploads==3);span_limit=48;
    memset(cpu,0,sizeof cpu);c.src=(uintptr_t)cpu+2;
    for(unsigned z=0;z<2;z++)for(unsigned x=0;x<2;x++)cpu[2+z*32+x*8]=50+z*30+x;
    copy_b2t(&c,1);assert(snapshots==2 && uploads==4); // misaligned: CPU fallback
    c.src=(uintptr_t)cpu+4;
    S.row_stride=8;copy_b2t(&c,1);S.row_stride=24;
    pool.end=(uintptr_t)cpu+51;copy_b2t(&c,1);pool.end=(uintptr_t)cpu+64;
    c.r.width=20;copy_b2t(&c,1);c.r.width=5;
    fail_snapshot=1;copy_b2t(&c,1); // refused submission skips, never decodes undefined bytes
    copy_b2t(&c,0);
    assert(snapshots==2 && uploads==4);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / 'upload.c'
            exe = Path(folder) / 'upload'
            src.write_text(code)
            subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-O1', '-fno-strict-aliasing',
                            '-I', str(DM / 'third_party/vulkan-headers/include'),
                            '-I', str(DM / 'gfx/tex'), str(src), str(DM / 'gfx/tex/nvn_bc.c'),
                            '-o', str(exe)], check=True)
            for mode in ([], ['gpu']):
                result = subprocess.run([str(exe), *mode], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
