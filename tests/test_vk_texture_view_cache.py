"""Cached storage views must retain the uncached path's GPU ownership."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk


class TextureViewCache(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_sampled_to_storage_hit_marks_gpu_owned(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        source = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_exec.c').read_text()
        start = source.index('static int tex_desc_for(')
        function = source[start:source.index('\n}\n', start) + 3]
        harness = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#define VKLOG(...) do {} while(0)
typedef struct {int dim,flags;} dmsv_bind;
typedef struct {int unused;} view_t;
typedef struct {struct {int format;} d;} tex_t;
typedef struct {int is_int,is_sint;} vkfmt_t;
typedef struct {uint64_t last_use;int gpu_w;} vkimg_t;
enum {TVC=4};
static struct {uint64_t epoch,gen;int tid,dim,arr,num;VkImageView v;vkimg_t *im;} g_tvc[TVC];
static uint64_t g_set_epoch=1,g_tex_gen=1;
static int g_var_now,g_tracing,resolves,no_image;
static uint64_t g_x_logfs;
static struct {struct {uint64_t key;} *sh[3];} S;
static vkimg_t image;static tex_t texture;
static VkSampler samp_for(int st,const dmsv_bind *b,uint64_t h,int nearest) {
    (void)st;(void)b;(void)h;(void)nearest;return (VkSampler)(uintptr_t)1;
}
static uint64_t mtl_last_fence(void) {return 10;}
static void vk_texdump_note(vkimg_t *im) {(void)im;}
static int vk_tex_by_id(int tid,const view_t **v,const tex_t **t) {
    assert(tid==3);*v=NULL;*t=&texture;return 1;
}
static VkImageView vk_tex_view(const tex_t *t,const view_t *v,int dim,int arr,int arg,vkimg_t **im) {
    (void)t;(void)v;(void)dim;(void)arr;(void)arg;
    resolves++;*im=no_image ? NULL : &image;return (VkImageView)(uintptr_t)2;
}
static int view_num(const tex_t *t,const view_t *v) {(void)t;(void)v;return 0;}
static const vkfmt_t *vk_fmt(int f) {(void)f;return NULL;}
static VkImageView vk_dummy_view(int d,int a,int b,int c,int e) {
    (void)d;(void)a;(void)b;(void)c;(void)e;return (VkImageView)(uintptr_t)4;
}
FUNCTION
int main(void) {
    dmsv_bind b={.dim=1};VkDescriptorImageInfo ii;
    tex_desc_for(1,&b,3,&ii,0);
    assert(resolves==1 && !image.gpu_w && image.last_use==11);
    tex_desc_for(1,&b,3,&ii,0);
    assert(resolves==1 && !image.gpu_w && ii.sampler);
    tex_desc_for(1,&b,3,&ii,1);
    assert(resolves==1 && image.gpu_w && !ii.sampler);
    tex_desc_for(1,&b,3,&ii,0);
    assert(resolves==1 && image.gpu_w && ii.sampler);
    // A cache miss must agree, and buffer-backed views may have no image.
    g_set_epoch++;image.gpu_w=0;tex_desc_for(1,&b,3,&ii,1);
    assert(resolves==2 && image.gpu_w);
    g_set_epoch++;no_image=1;tex_desc_for(1,&b,3,&ii,0);
    tex_desc_for(1,&b,3,&ii,1);assert(resolves==3 && !ii.sampler);
}
'''.replace('FUNCTION', function)
        with tempfile.TemporaryDirectory(prefix='nx2a-texture-view-cache-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)
