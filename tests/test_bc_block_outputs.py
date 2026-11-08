"""BC output variants must not affect ordinary or mixed-format passes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class BCBlockOutputs(unittest.TestCase):
    def test_module_cache_keeps_output_variant_separate(self):
        source = (ROOT / 'nxnative/dm/android/gfx_vk_shader.c').read_text()
        start = source.index('static VkShaderModule vk_shader_module_(')
        function = source[start:source.index('// static buffers:', start)]
        code = r'''
#include <vulkan/vulkan_core.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#define VK_SHADER_UINT_OUTPUT 4
#define VKLOG(...) ((void)0)
#define die(...) abort()
typedef struct {
 int stage,depth_repl,nvar,nbind;uint32_t *spv,nspv;uint64_t key;VkShaderModule mod;
 struct {uint32_t binding;} b[64];
 struct {uint64_t um,im;uint32_t xo;int fd;VkShaderModule mod;} var[8];
} vkshader_t;
static struct {VkDevice dev;} V;
static pthread_mutex_t g_sh_mu=PTHREAD_MUTEX_INITIALIZER;
int g_prof_on;uint64_t g_prof_ns[8];
static unsigned created,rewritten;
static VkShaderModule alt_module(vkshader_t *s) {(void)s;abort();}
static uint32_t *copy(const uint32_t *w,uint32_t n,uint32_t *out) {*out=n;uint32_t *p=malloc(n*4);memcpy(p,w,n*4);return p;}
static uint32_t *spv_uint_outputs(const uint32_t *w,uint32_t n,uint32_t *out) {rewritten++;return copy(w,n,out);}
static uint32_t *spv_frag_depth(const uint32_t *w,uint32_t n,uint32_t *out) {return copy(w,n,out);}
static uint32_t *spv_dual(const uint32_t *w,uint32_t n,uint32_t *out) {return copy(w,n,out);}
static uint32_t *spv_add_outputs(const uint32_t *w,uint32_t n,uint32_t x,uint32_t *out) {(void)x;return copy(w,n,out);}
static uint32_t *spv_vs_init(const uint32_t *w,uint32_t n,uint32_t *out) {return copy(w,n,out);}
static uint32_t *spv_int_images(const uint32_t *w,uint32_t n,const uint16_t *b,const uint8_t *k,int nb,uint32_t *out) {
 (void)b;(void)k;(void)nb;return copy(w,n,out);
}
VkResult vkCreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo *c,const VkAllocationCallbacks *a,VkShaderModule *m) {
 (void)d;(void)a;assert(c->codeSize==20);*m=(VkShaderModule)(uintptr_t)(100+ ++created);return VK_SUCCESS;
}
''' + function + r'''
int main(void) {
 uint32_t words[5]={0};vkshader_t s={.stage=1,.spv=words,.nspv=5,.mod=(VkShaderModule)42};
 assert(vk_shader_module(&s,0,0,0,0)==s.mod);
 VkShaderModule raw=vk_shader_module(&s,0,0,0,4);
 assert(raw!=s.mod && rewritten==1 && created==1 && s.var[0].fd==4);
 assert(vk_shader_module(&s,0,0,0,4)==raw && rewritten==1);
 assert(vk_shader_module(&s,0,0,0,5)!=raw && rewritten==2 && s.var[1].fd==5);
 assert(vk_shader_module(&s,0,0,0,6)!=raw && rewritten==3 && s.var[2].fd==6);
 s.depth_repl=1;assert(vk_shader_module(&s,0,0,0,5)==raw && rewritten==3);
 s.stage=0;assert(vk_shader_module(&s,0,0,0,4)==s.mod && rewritten==3);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / 'cache.c'
            exe = Path(folder) / 'cache'
            src.write_text(code)
            subprocess.run(['cc', '-std=gnu11', '-pthread',
                            '-I', str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'),
                            str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_native_decoded_and_unrelated_targets(self):
        source = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text()
        start = source.index('static int bc_block_outputs(')
        function = source[start:source.index('\n}\n', start)+3]
        code = r'''
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stddef.h>
typedef struct {int set_format,format;} view_t;
typedef struct {VkFormat vk;int compressed,dec,bw,bh,bpb,is_int,is_sint;} vkfmt_t;
static struct {int ncol,cfmt[8],rt_has_cview[8];view_t rt_cview[8];struct {struct {int format;} d;} rt_col[8];} S;
static const vkfmt_t formats[]={
 {VK_FORMAT_BC3_UNORM_BLOCK,1,0,4,4,16,0,0},
 {VK_FORMAT_R32G32B32A32_UINT,0,0,1,1,16,1,0},
 {VK_FORMAT_R32G32_UINT,0,0,1,1,8,1,0},
 {VK_FORMAT_R8G8B8A8_UNORM,0,4,4,4,16,0,0},
 {VK_FORMAT_R8G8B8A8_UNORM,0,0,1,1,4,0,0},
 {VK_FORMAT_R32G32B32A32_SINT,0,0,1,1,16,1,1}
};
static const vkfmt_t *vk_fmt(int f) {return f>=0 && f<6?&formats[f]:NULL;}
''' + function + r'''
int main(void) {
 assert(!bc_block_outputs());
 S.ncol=1;S.cfmt[0]=1;S.rt_has_cview[0]=1;S.rt_cview[0]=(view_t){1,1};
 assert(bc_block_outputs()); // native BC, raw 16-byte uint view
 S.rt_col[0].d.format=3;assert(bc_block_outputs()); // CPU-decoded backing
 S.rt_cview[0].format=2;assert(!bc_block_outputs()); // block/view byte mismatch
 S.rt_cview[0].format=5;assert(!bc_block_outputs()); // signed target
 S.rt_cview[0].format=4;assert(!bc_block_outputs()); // ordinary float target
 S.rt_has_cview[0]=0;S.rt_col[0].d.format=1;assert(!bc_block_outputs()); // ordinary uint target
 S.rt_has_cview[0]=1;S.rt_col[0].d.format=0;S.rt_cview[0].format=1;
 S.ncol=2;S.cfmt[1]=1;S.rt_col[1].d.format=4;assert(!bc_block_outputs()); // mixed float pass
 S.cfmt[1]=0;assert(bc_block_outputs()); // omitted inactive attachment
 S.rt_col[0].d.format=99;assert(!bc_block_outputs());
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / 'selection.c'
            exe = Path(folder) / 'selection'
            src.write_text(code)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'),
                            str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
