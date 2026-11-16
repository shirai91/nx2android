"""Exercise production static-buffer initialization: disabled attributes must not change zero UBOs."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_shader.c'
PREFIX = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uintptr_t VkBuffer, VkDeviceMemory;
typedef struct { int type; void *next; unsigned flags; uint64_t size; unsigned usage, sharing, count; const unsigned *indices; } VkBufferCreateInfo;
typedef struct { uint64_t size, alignment; uint32_t memoryTypeBits; } VkMemoryRequirements;
typedef struct { int type; void *next; uint64_t size; unsigned memoryTypeIndex; } VkMemoryAllocateInfo;
enum { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO=1, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO=2,
       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT=1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT=2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT=4,
       VK_SHARING_MODE_EXCLUSIVE=0, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT=1, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT=2 };
#define VK_WHOLE_SIZE (~0ull)
#define VKCHK(x) assert((x)==0)
static struct { int dev; } V;
static unsigned buffers, allocations;
static void *memory[8], *bound[8];
static int vk_mem_type(unsigned bits, unsigned flags, int unused) { (void)bits; (void)flags; (void)unused; return 0; }
static int vkCreateBuffer(int dev, const VkBufferCreateInfo *ci, void *allocator, VkBuffer *out) { (void)dev; (void)allocator; assert(ci->size==65536); *out=++buffers; return 0; }
static void vkGetBufferMemoryRequirements(int dev, VkBuffer buf, VkMemoryRequirements *r) { (void)dev; (void)buf; *r=(VkMemoryRequirements){65536,16,1}; }
static int vkAllocateMemory(int dev, const VkMemoryAllocateInfo *ai, void *allocator, VkDeviceMemory *out) { (void)dev; (void)allocator; *out=++allocations; memory[*out]=malloc(ai->size); memset(memory[*out],0xcd,ai->size); return 0; }
static int vkBindBufferMemory(int dev, VkBuffer buf, VkDeviceMemory mem, uint64_t off) { (void)dev; assert(!off); bound[buf]=memory[mem]; return 0; }
static int vkMapMemory(int dev, VkDeviceMemory mem, uint64_t off, uint64_t size, unsigned flags, void **out) { (void)dev; (void)size; (void)flags; assert(!off); *out=memory[mem]; return 0; }
'''
SUFFIX = r'''
static void verify(void) {
    VkBuffer defaults=vk_default_attribute_buffer(), zero=vk_zero_buffer(), support=vk_support_buffer();
    assert(defaults!=zero && defaults!=support && zero!=support);
    const uint32_t *a=bound[defaults], *z=bound[zero];
    assert(a[0]==0 && a[1]==0 && a[2]==0 && a[3]==0x3f800000u);
    float w; memcpy(&w,a+3,4); assert(w==1.0f);
    for (int i=0;i<16384;i++) { assert(!z[i]); if (i!=3) assert(!a[i]); }
    const float *s=bound[support];
    for (int i=0;i<73;i++) assert(s[48+4*i]==1.0f);
    // The second vec4 remains zero for enabled inputs whose buffer/format is invalid.
    for (int i=4;i<8;i++) assert(!a[i]);
}
static void *reader(void *unused) { (void)unused; for (int i=0;i<20;i++) verify(); return NULL; }
int main(void) {
    pthread_t threads[8];
    for (int i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,reader,NULL));
    for (int i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    verify(); assert(buffers==3 && allocations==3);
    for (unsigned i=1;i<=allocations;i++) free(memory[i]);
    puts("Vulkan disabled attribute defaults: PASS"); return 0;
}
'''


def harness_source():
    source = SOURCE.read_text(encoding='utf-8')
    begin = source.index('static VkBuffer static_buf(int which)')
    end = source.index("// a shader's c1 constants", begin)
    return PREFIX + source[begin:end] + SUFFIX


@unittest.skipUnless(shutil.which('cc'), 'native C compiler unavailable')
class VertexDefaultBuffers(unittest.TestCase):
    def test_production_buffers_publish_separate_defaults_and_zero_storage(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path/'check.c').write_text(harness_source(), encoding='utf-8')
            subprocess.run(['cc', '-std=gnu11', '-O2', '-pthread', str(path/'check.c'), '-o', str(path/'check')], check=True, capture_output=True)
            result = subprocess.run([str(path/'check')], check=True, capture_output=True, text=True)
            self.assertIn('PASS', result.stdout)
