"""Exercise actual ROI parsing and unsigned-HDR decoding used by pixel history."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'nxnative/dm/android/gfx_vk_pixel_history.h'
SOURCE = ROOT / 'nxnative/dm/android/gfx_vk_exec.c'


def harness_source():
    return r'''
#include <assert.h>
#include <stdio.h>
''' + '#include "' + HEADER.as_posix() + '"\n' + r'''
int main(void) {
    uint32_t p[4] = {7,8,9,10};
    assert(vk_pixel_history_parse("2560x1440:923,1307",p));
    assert(p[0]==2560 && p[1]==1440 && p[2]==923 && p[3]==1307);
    assert(vk_pixel_history_parse("1x1:0,0",p));
    assert(vk_pixel_history_parse("16384x16384:16383,16383",p));
    const char *bad[] = {NULL,"","0","0x1:0,0","1x0:0,0","1x1:1,0","1x1:0,1",
        "1x1:-1,0","-1x1:0,0","1x1:0,0junk","1x1:0,0 ","1x1:0","1x1,0,0",
        "16385x1:0,0","999999999999999999999999999999999999999999x1:0,0"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        uint32_t old[4];memcpy(old,p,sizeof p);
        assert(!vk_pixel_history_parse(bad[i],p));assert(!memcmp(old,p,sizeof p));
    }
    // IEEE-like R11/G11 (6 mantissa bits) and B10 (5) include subnormals and non-finites.
    assert(vk_pixel_history_ufloat(0,6)==0);
    assert(vk_pixel_history_ufloat(15u<<6,6)==1);
    assert(vk_pixel_history_ufloat(16u<<6,6)==2);
    assert(vk_pixel_history_ufloat(14u<<5,5)==0.5f);
    assert(vk_pixel_history_ufloat(1,6)==0x1p-20f);
    assert(vk_pixel_history_ufloat(1,5)==0x1p-19f);
    assert(vk_pixel_history_ufloat((30u<<6)|63,6)==65024);
    assert(vk_pixel_history_ufloat((30u<<5)|31,5)==64512);
    assert(isinf(vk_pixel_history_ufloat(31u<<6,6)));
    assert(isinf(vk_pixel_history_ufloat(31u<<5,5)));
    assert(isnan(vk_pixel_history_ufloat((31u<<6)|1,6)));
    assert(isnan(vk_pixel_history_ufloat((31u<<5)|1,5)));
    puts("pixel history helpers: PASS");return 0;
}
'''


def lifecycle_harness_source():
    source = SOURCE.read_text(encoding='utf-8')
    block = source.split('#include "gfx_vk_pixel_history.h"', 1)[1].split('void vk_end_pass_land', 1)[0]
    prefix = r'''
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
'''+ '#include "' + HEADER.as_posix() + '"\n' + r'''
typedef struct { uint32_t fmt:8,offset:19,stream:5; } va_st;
typedef struct { uint32_t base_level,min_layer;int set_levels,set_layers; } view_t;
typedef struct { uint32_t w,h,levels,layers;VkSampleCountFlagBits samples;VkFormat fmt;VkImage img; } vkimg_t;
static struct { VkDevice dev; } V;
static struct {
 uint32_t pass_w,pass_h,rt_has_col[8],vattr[16];int pass_open,rt_has_cview[8],rt_has_dview;unsigned daspect;
 view_t rt_cview[8],rt_dview;struct { uint64_t key; } *sh[2];
 struct { uint64_t address; } vbuf[16];struct { unsigned stride; } vstream[16];struct { uint64_t addr; } ubo[6][16];
 struct { int dt,dw,df; } ds;struct { unsigned mask; } chmask;struct { unsigned blend_en; } color;
 int sc_set;struct { int x,y,w,h; } sc;struct { unsigned cull,front,mode,offset_en; } poly;float vp[16][4],dnear,dfar;
 struct { float factor,units,clamp; } pofs;
} S;
static struct { vkimg_t *im; } P_att[9];
static int g_pd_state,g_sub_pass=3,allocations,frees,destroys,invalidates,copies,ends,commits;
static uint64_t g_tex_frame=20;
static unsigned char *memory;
#define VKLOG(...) ((void)0)
#define VKCHK(x) assert((x)==VK_SUCCESS)
static int pass_commit(void) { commits++;S.pass_open=1;return 1; }
static void vk_end_pass(void) { ends++;S.pass_open=0; }
static VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)1; }
static void vk_img_barrier_all(VkCommandBuffer cb) { assert(cb); }
static uint64_t mtl_last_fence(void) { return 10; }
static uint32_t vk_mem_type(uint32_t bits,VkMemoryPropertyFlags want,VkMemoryPropertyFlags avoid) { assert(bits==1 && (want&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !avoid);return 0; }
static VkResult vkCreateBuffer(VkDevice d,const VkBufferCreateInfo *i,const VkAllocationCallbacks *a,VkBuffer *out) { (void)d;(void)a;assert(i->size==4096*16 && i->usage==VK_BUFFER_USAGE_TRANSFER_DST_BIT);*out=(VkBuffer)1;return VK_SUCCESS; }
static void vkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements *r) { (void)d;assert(b);*r=(VkMemoryRequirements){4096*16,16,1}; }
static VkResult vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo *i,const VkAllocationCallbacks *a,VkDeviceMemory *out) { (void)d;(void)a;allocations++;memory=calloc(1,i->allocationSize);assert(memory);*out=(VkDeviceMemory)1;return VK_SUCCESS; }
static VkResult vkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o) { (void)d;assert(b&&m&&!o);return VK_SUCCESS; }
static VkResult vkMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize n,VkMemoryMapFlags f,void **out) { (void)d;(void)n;(void)f;assert(m&&!o);*out=memory;return VK_SUCCESS; }
static VkResult vkInvalidateMappedMemoryRanges(VkDevice d,uint32_t n,const VkMappedMemoryRange *r) { (void)d;assert(n==1 && r->offset==0 && r->size==VK_WHOLE_SIZE);invalidates++;return VK_SUCCESS; }
static void vkDestroyBuffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks *a) { (void)d;(void)a;assert(b);destroys++; }
static void vkFreeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a) { (void)d;(void)a;assert(m);frees++;free(memory);memory=NULL; }
static void vkCmdCopyImageToBuffer(VkCommandBuffer cb,VkImage image,VkImageLayout layout,VkBuffer b,uint32_t n,const VkBufferImageCopy *r) {
 assert(cb&&b&&layout==VK_IMAGE_LAYOUT_GENERAL&&n==1);assert(r->imageOffset.x==1 && r->imageOffset.y==1);
 assert(r->imageExtent.width==1 && r->imageExtent.height==1 && r->imageExtent.depth==1);
 assert(r->bufferOffset+4<=4096*16);uint32_t v;
 if (image==(VkImage)11) { assert(r->imageSubresource.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT);v=(15u<<6)|((16u<<6)<<11)|((14u<<5)<<22); }
 else { assert(image==(VkImage)22 && r->imageSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT);v=0x3e800000u; }
 memcpy(memory+r->bufferOffset,&v,4);copies++;
}
static void vkCmdPipelineBarrier(VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t n,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *im) {
 (void)flags;(void)nb;(void)b;(void)ni;(void)im;assert(cb&&src==VK_PIPELINE_STAGE_TRANSFER_BIT&&dst==VK_PIPELINE_STAGE_HOST_BIT&&n==1);
 assert(m->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT&&m->dstAccessMask==VK_ACCESS_HOST_READ_BIT);
}
'''
    suffix = r'''
int main(int argc,char **argv) {
 assert(argc==2);setenv("DM_ROOT",argv[1],1);unsetenv("DM_VK_PIXEL_HISTORY");
 pixel_history_poll(0);pixel_history_record(4,7,1,3,1,0,0);assert(!allocations && !copies && !pixel_history_busy());
 setenv("DM_VK_PIXEL_HISTORY","1x1:1,0",1);pixel_history_poll(0);assert(!allocations);
 g_pd_state=1;setenv("DM_VK_PIXEL_HISTORY","2x2:1,1",1);pixel_history_poll(0);assert(!allocations);g_pd_state=0;
 setenv("DM_VK_PIXEL_HISTORY","2x2:1,1",1);pixel_history_poll(0);assert(allocations==1 && g_ph.state==1 && !*getenv("DM_VK_PIXEL_HISTORY"));
 vkimg_t color={2,2,1,1,VK_SAMPLE_COUNT_1_BIT,VK_FORMAT_B10G11R11_UFLOAT_PACK32,(VkImage)11};
 vkimg_t depth={2,2,1,1,VK_SAMPLE_COUNT_1_BIT,VK_FORMAT_D32_SFLOAT,(VkImage)22};
 P_att[0].im=&color;P_att[8].im=&depth;S.pass_w=S.pass_h=2;S.pass_open=1;S.rt_has_col[0]=1;S.daspect=VK_IMAGE_ASPECT_DEPTH_BIT;
 S.chmask.mask=0xffff;S.ds.dt=S.ds.dw=1;S.ds.df=4;S.vbuf[0].address=0x1234;S.vstream[0].stride=12;
 S.pass_w=3;pixel_history_record(4,7,1,3,1,0x4567,0);assert(!copies&&!ends);S.pass_w=2;
 color.samples=VK_SAMPLE_COUNT_4_BIT;pixel_history_record(4,7,1,3,1,0x4567,0);assert(!copies);color.samples=VK_SAMPLE_COUNT_1_BIT;
 S.rt_has_cview[0]=1;S.rt_cview[0].set_levels=1;S.rt_cview[0].base_level=1;pixel_history_record(4,7,1,3,1,0x4567,0);assert(!copies);S.rt_has_cview[0]=0;
 S.daspect=VK_IMAGE_ASPECT_STENCIL_BIT;pixel_history_record(1,-1,0,0,0,0,0);assert(copies==1&&g_ph.rows[0].dfmt==0);S.daspect=VK_IMAGE_ASPECT_DEPTH_BIT;
 S.pass_open=2;pixel_history_record(2,-1,0,0,0,0,0);assert(commits==1&&copies==3&&ends==2&&g_ph.n==2);
 S.pass_open=1;pixel_history_record(4,7,1,3,1,0x4567,0);assert(copies==5&&ends==3&&g_ph.n==3);
 g_ph.n=PH_MAX;S.pass_open=1;pixel_history_record(4,8,1,3,1,0,0);assert(g_ph.truncated&&copies==5);g_ph.n=3;
 pixel_history_poll(0);assert(g_ph.state==2&&g_ph.fence==11&&!frees&&!invalidates);
 pixel_history_poll(10);assert(g_ph.state==2&&!frees&&!invalidates);
 pixel_history_poll(11);assert(!pixel_history_busy()&&frees==1&&destroys==1&&invalidates==1);
 char path[2048];snprintf(path,sizeof path,"%s/pixel_history.tsv",argv[1]);FILE *f=fopen(path,"r");assert(f);
 char output[8192];size_t n=fread(output,1,sizeof(output)-1,f);output[n]=0;fclose(f);
 assert(strstr(output,"truncated=1")&&strstr(output,"hdr_word")&&strstr(output,"\t1\t2\t0.5\t0.25"));
 puts("pixel history lifecycle: PASS");return 0;
}
'''
    return prefix + block + suffix


class PixelHistoryTests(unittest.TestCase):
    def _run(self, text, vulkan=False):
        cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc:
            self.skipTest('Native C compiler unavailable; Android harness can run separately')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'check.c'
            exe = Path(directory) / 'check.exe'
            source.write_text(text, encoding='utf-8')
            include = ['-I' + str(ROOT/'nxnative/dm/third_party/vulkan-headers/include')] if vulkan else []
            subprocess.run([cc, '-std=c11', '-O2', *include, str(source), '-lm', '-o', str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe),directory], check=True, capture_output=True, text=True)
            self.assertIn('PASS', result.stdout)

    def test_native_roi_and_hdr_values(self):
        self._run(harness_source())

    def test_native_capture_and_fence_lifetime(self):
        self._run(lifecycle_harness_source(), vulkan=True)
