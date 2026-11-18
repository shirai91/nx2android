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
        source = (DM / 'android/gfx_vk_exec.c').read_text(encoding='utf-8')
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
#include <stdarg.h>
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
static unsigned ew=5,eh=3,ed=2,span=48;
static unsigned logs,log_readback,watches;
static unsigned long long watch_address;static int watch_width,watch_height;
static VkBufferImageCopy last;
static jmp_buf failure;
static void die(const char *s,...) {(void)s;assert(expect_error);longjmp(failure,1);}
#define VKRED_ONCE(...) ((void)0)
static void test_log(const char *fmt,...) {
    char text[256];va_list a;va_start(a,fmt);vsnprintf(text,sizeof text,fmt,a);va_end(a);
    assert(strstr(text,"BC CPU buffer upload: NVN format 0x49, 5x3x2, GPU readback "));
    logs++;log_readback=strstr(text,"readback yes")!=NULL;
}
#define VKLOG(...) test_log(__VA_ARGS__)
static void vk_end_pass_land(void) {ended++;phase=0;}
static vkimg_t *vk_tex(const tex_t *t) {(void)t;return &image;}
static const vkfmt_t *vk_fmt(int f) {assert(f==0x49);return &format;}
static int vk_buf_at(uint64_t a,size_t n,VkBuffer *b,VkDeviceSize *o) {
    if(a<(uintptr_t)cpu || n>span_limit)return 0;*b=(VkBuffer)8;*o=a-(uintptr_t)cpu;return 1;
}
static int vk_buffer_snapshot(VkBuffer b,VkDeviceSize off,size_t n,void *dst) {
    assert(b==(VkBuffer)8 && off==4 && n==span && !phase);
    if(fail_snapshot)return 0;memcpy(dst,gpu+off,n);snapshots++;phase=1;return 1;
}
static void *vk_scratch(size_t n,size_t align,VkBuffer *b,VkDeviceSize *o) {
    assert(n==ew*eh*ed && align==16);*b=(VkBuffer)9;*o=16;phase=1;return scratch+16;
}
static VkCommandBuffer vk_cmd(void) {assert(phase==1);return (VkCommandBuffer)2;}
static void vk_tex_prepare(VkCommandBuffer cb,vkimg_t *im) {assert(cb==(VkCommandBuffer)2 && im==&image && phase==1);}
static void vk_img_barrier_all(VkCommandBuffer cb) {assert(cb==(VkCommandBuffer)2);}
static void vk_watch(vkimg_t *im,const char *s) {
    unsigned long long address;int w,h;
    assert(im==&image && sscanf(s,"buffer copy destination (buffer %llx, %dx%d)",&address,&w,&h)==3);
    assert(address>=(uintptr_t)cpu && address<(uintptr_t)cpu+sizeof cpu && w>0 && h>0);
    watches++;watch_address=address;watch_width=w;watch_height=h;
}
static vkpool_t *vk_pool_of(uint64_t a,uint64_t n) {return a>=(uintptr_t)cpu && a+n<=pool.end?&pool:NULL;}
static void vk_readback_unknown(void) {abort();}
void vk_gpu_wrote(uint64_t a,uint64_t n) {(void)a;(void)n;abort();}
void vkCmdCopyImageToBuffer(VkCommandBuffer c,VkImage i,VkImageLayout l,VkBuffer b,uint32_t n,const VkBufferImageCopy *r) {
    (void)c;(void)i;(void)l;(void)b;(void)n;(void)r;abort();
}
void vkCmdCopyBufferToImage(VkCommandBuffer cb,VkBuffer b,VkImage im,VkImageLayout l,uint32_t n,const VkBufferImageCopy *r) {
    assert(cb==(VkCommandBuffer)2 && b==(VkBuffer)9 && im==image.img && l==VK_IMAGE_LAYOUT_GENERAL && n==1);
    assert(r->bufferOffset==16 && !r->bufferRowLength && !r->bufferImageHeight);
    for(unsigned z=0;z<ed;z++)for(unsigned y=0;y<eh;y++)for(unsigned x=0;x<ew;x++)
        assert(scratch[16+z*ew*eh+y*ew+x]==wanted+z*30+(x==4));
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
    c.r.width=24;copy_b2t(&c,1);c.r.width=5;
    assert(!uploads && !snapshots && !logs && !watches);
    int fallback_first=argc>2;if(fallback_first)span_limit=47;
    wanted=enabled&&!fallback_first?20:50;copy_b2t(&c,1);
    assert(logs==1 && log_readback==(unsigned)(enabled&&!fallback_first) && !watches);
    g_watch_off=0;
    if(fallback_first) {
        assert(!snapshots && uploads==1);copy_b2t(&c,1);
        assert(!snapshots && uploads==2 && watches==1 && logs==1 && !log_readback);
        return 0;
    }
    if(!enabled) {
        assert(snapshots==0 && uploads==1);
        setenv("DM_VK_X_BC_GPU_READBACK","1",1);copy_b2t(&c,1);
        assert(snapshots==0 && uploads==2); // cached default stays off
        goto small_mips;
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
    c.r.width=24;copy_b2t(&c,1);c.r.width=5;
    fail_snapshot=1;copy_b2t(&c,1); // refused submission skips, never decodes undefined bytes
    copy_b2t(&c,0);
    assert(snapshots==2 && uploads==4);
small_mips:;
    // Block-rounded requests for small mips, edge blocks, array layers and 3D.
    const unsigned cases[][9] = {
        {19,13,3,0,0,4,4,1,1}, // 2x1 mip: one BC block
        {19,13,4,0,0,4,4,1,1}, // 1x1 mip
        {8,16,3,0,0,4,4,1,1},  // 1x2 mip
        {8,8,2,0,0,4,4,1,1},   // 2x2 mip
        {32,32,3,0,0,4,4,1,1}, // exact 4x4
        {5,3,0,0,0,8,4,1,1},   // non-block-sized edge, two source blocks
        {9,7,0,8,4,4,4,1,1},   // offset edge leaves 1x3 texels
        {19,13,3,0,0,4,4,3,2}, // array: two layers, base layer one
        {2,1,0,0,0,4,4,1,2},   // two 3D slices
    };
    fail_snapshot=0;span_limit=48;pool.end=(uintptr_t)cpu+64;
    wanted=enabled?20:50;
    memset(cpu,0,sizeof cpu);memset(gpu,0,sizeof gpu);
    for(unsigned z=0;z<2;z++)for(unsigned x=0;x<2;x++) {gpu[4+z*32+x*8]=20+z*30+x;cpu[4+z*32+x*8]=50+z*30+x;}
    for(unsigned k=0;k<sizeof cases/sizeof cases[0];k++) {
        const unsigned *v=cases[k];image.w=v[0];image.h=v[1];image.layers=v[7];image.d=8;
        c.view=(view_t){1,v[2],1,v[7]>1?1:0};c.has_view=1;
        c.r=(region_t){v[3],v[4],0,v[5],v[6],v[8]};c.src=(uintptr_t)cpu+4;
        ew=(image.w>>v[2]?image.w>>v[2]:1)-v[3];if(ew>v[5])ew=v[5];
        eh=(image.h>>v[2]?image.h>>v[2]:1)-v[4];if(eh>v[6])eh=v[6];ed=v[8];
        span=(ed-1)*32+((eh+3)/4-1)*24+(ew+3)/4*8;
        unsigned before=uploads,reads=snapshots;copy_b2t(&c,1);
        assert(uploads==before+1 && snapshots==reads+(enabled?1:0));
        assert(last.imageExtent.width==ew && last.imageExtent.height==eh);
        assert(last.imageExtent.depth==(image.layers>1?1:ed));
        assert(last.imageSubresource.layerCount==(image.layers>1?ed:1));
        assert(last.imageSubresource.baseArrayLayer==(image.layers>1?1:0));
        pool.end=c.src+span-1;copy_b2t(&c,1);pool.end=(uintptr_t)cpu+64;
        c.r.width+=4;copy_b2t(&c,1);c.r.width-=4;
        if(v[5]-1>ew) {c.r.width=v[5]-1;copy_b2t(&c,1);c.r.width=v[5];}
        if(v[6]-1>eh) {c.r.height=v[6]-1;copy_b2t(&c,1);c.r.height=v[6];}
        c.r.xoffset=(image.w>>v[2]?image.w>>v[2]:1);copy_b2t(&c,1);
        assert(uploads==before+1 && snapshots==reads+(enabled?1:0));
        assert(logs==1 && watches==uploads-1);
        assert(watch_address==(uintptr_t)cpu+4 && watch_width==(int)v[5] && watch_height==(int)v[6]);
    }
}

'''
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / 'upload.c'
            exe = Path(folder) / 'upload'
            src.write_text(code, encoding='utf-8')
            subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-O1', '-fno-strict-aliasing',
                            '-I', str(DM / 'third_party/vulkan-headers/include'),
                            '-I', str(DM / 'gfx/tex'), str(src), str(DM / 'gfx/tex/nvn_bc.c'),
                            '-o', str(exe)], check=True)
            for mode in ([], ['gpu'], ['gpu', 'fallback-first']):
                result = subprocess.run([str(exe), *mode], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
