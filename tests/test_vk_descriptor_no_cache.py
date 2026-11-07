"""Disabled descriptor caching must not hash/store keys or alter descriptor writes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk


class DescriptorNoCache(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_cache_disabled_and_enabled_writes(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_exec.c').read_text()
        start = src.index('    uint64_t tb1 = tick(); g_ns_set_build += tb1 - tb0;')
        block = src[start:src.index('    // verify: the same content', start)]
        start = src.index('    int memo = st < 3')
        policy = src[start:src.index('\n', start)]
        harness = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define SETC 16
static struct {VkDevice dev;} V;
static struct {uint64_t h,epoch;VkDescriptorSetLayout l;VkDescriptorSet set;} g_setc[SETC];
static uint64_t g_set_epoch=1,g_ns_set_build,g_ns_set_hash,g_ns_set_new,g_set_hits,g_set_new;
static int g_tracing,g_x_nosetc,g_x_nosetmemo,g_var_now,g_x_nodrvc,g_ubolog_key,g_td_off=-1;
static int hashes,allocations,updates;
typedef struct {VkDescriptorSetLayout dsl;} shader;
static uint64_t tick(void) {static uint64_t n;return ++n;}
static int set_memo_ok(const shader *s) {(void)s;return 1;}
static uint64_t fnv(uint64_t h,const void *p,size_t n) {
    hashes++;const uint8_t *b=p;for(size_t i=0;i<n;i++)h=(h^b[i])*1099511628211ull;return h;
}
static VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    assert(layout==(VkDescriptorSetLayout)(uintptr_t)3);return (VkDescriptorSet)(uintptr_t)(100+(++allocations));
}
static void vkUpdateDescriptorSets(VkDevice dev,uint32_t n,const VkWriteDescriptorSet *w,uint32_t nc,const VkCopyDescriptorSet *c) {
    (void)dev;assert(n==3 && !nc && !c);updates++;
    for(unsigned i=0;i<n;i++)assert(w[i].dstSet==(VkDescriptorSet)(uintptr_t)(100+allocations) && w[i].dstBinding==i && w[i].descriptorCount==1);
    assert(w[0].pBufferInfo->offset==32 && w[0].pBufferInfo->range==64);
    assert(w[1].pImageInfo->imageLayout==VK_IMAGE_LAYOUT_GENERAL);
    assert(*w[2].pTexelBufferView==(VkBufferView)(uintptr_t)9);
}
static VkDescriptorSet collect(shader *s,VkWriteDescriptorSet *w) {
    int st=0;
POLICY
    assert(!g_x_nosetc || (!memo && !verify));
    (void)memo;(void)verify;uint64_t tb0=tick();uint32_t nw=3;VkDescriptorSet set=VK_NULL_HANDLE;
BLOCK
    return set;
}
int main(void) {
    shader s={.dsl=(VkDescriptorSetLayout)(uintptr_t)3};
    VkDescriptorBufferInfo buffer={(VkBuffer)(uintptr_t)4,32,64};
    VkDescriptorImageInfo image={.imageView=(VkImageView)(uintptr_t)5,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkBufferView view=(VkBufferView)(uintptr_t)9;
    VkWriteDescriptorSet w[3]={{.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer},
        {.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,.pImageInfo=&image},
        {.dstBinding=2,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,.pTexelBufferView=&view}};
    VkDescriptorSet first=collect(&s,w);assert(hashes>0 && allocations==1 && updates==1);
    assert(collect(&s,w)==first && allocations==1 && updates==1 && g_set_hits==1);
    unsigned char before[sizeof g_setc];memcpy(before,g_setc,sizeof before);
    g_x_nosetc=1;int saved=hashes;
    assert(collect(&s,w)!=first);assert(collect(&s,w)!=first);
    assert(hashes==saved && allocations==3 && updates==3 && !memcmp(before,g_setc,sizeof before));
    g_x_nosetc=0;assert(collect(&s,w)==first && allocations==3 && updates==3);
    image.imageView=(VkImageView)(uintptr_t)6;
    assert(collect(&s,w)!=first && allocations==4 && updates==4);
    assert(buffer.offset==32 && buffer.range==64 && view==(VkBufferView)(uintptr_t)9);
}
'''.replace('POLICY', policy).replace('BLOCK', block)
        with tempfile.TemporaryDirectory(prefix='nx2a-descriptor-nocache-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)
