"""Exercise real copied-pool allocation and opt-in fallback with Vulkan mocks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk


class CachedPools(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_cache_ranges(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = src.index('static void pool_cache_sync(')
        function = src[start:src.index('\n}\n', start)+3]
        harness = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
static inline int mem_diag_on(void) { return 0; }
static inline uint64_t mem_diag_ns(void) { return 0; }
static inline void mem_diag_note(unsigned phase, uint64_t t, uint64_t n) { (void)phase; (void)t; (void)n; }
static struct { VkDevice dev; VkPhysicalDeviceProperties props; } V;
typedef struct { uint64_t lo,hi; VkDeviceMemory mem; int noncoherent; } vkpool_t;
static VkMappedMemoryRange seen; static int calls;
static VkResult cache(VkDevice dev, uint32_t n, const VkMappedMemoryRange *r) {
    (void)dev; assert(n==1 && r->sType==VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE && !r->pNext);
    assert(r->memory==(VkDeviceMemory)(uintptr_t)2); seen=*r; calls++; return VK_SUCCESS;
}
#define vkFlushMappedMemoryRanges cache
#define vkInvalidateMappedMemoryRanges cache
#define VKCHK(x) assert((x)==VK_SUCCESS)
FUNCTION
static void check(uint64_t size, uint64_t lo, uint64_t hi, uint64_t atom) {
    assert(lo<hi && hi<=size); V.props.limits.nonCoherentAtomSize=atom;
    vkpool_t p={.hi=size,.mem=(VkDeviceMemory)(uintptr_t)2,.noncoherent=1};
    int before=calls; pool_cache_sync(&p,0,lo,hi); assert(calls==before+1);
    uint64_t start=lo/atom*atom;
    __uint128_t end=((__uint128_t)hi+atom-1)/atom*atom;
    assert(seen.offset==start && seen.offset<=lo && seen.offset%atom==0);
    assert(seen.size==(end>=size ? VK_WHOLE_SIZE : (uint64_t)end-start));
    if (seen.size!=VK_WHOLE_SIZE) assert(seen.size%atom==0 && seen.size>=hi-start && seen.size<size-start);
    VkMappedMemoryRange flushed=seen;
    pool_cache_sync(&p,1,lo,hi); assert(seen.offset==flushed.offset && seen.size==flushed.size);
    p.noncoherent=0; before=calls; pool_cache_sync(&p,0,lo,hi); pool_cache_sync(&p,1,lo,hi); assert(calls==before);
}
int main(void) {
    check(65,0,65,64); check(1000,65,70,64); check(1000,990,1000,64);
    check(1024,900,960,64); check(1024,900,1024,64); check(1,0,1,64);
    check(UINT64_MAX-15,UINT64_MAX-100,UINT64_MAX-20,64);
    uint64_t seed=12345, atoms[]={1,64,256,65536,131072};
    for (unsigned a=0;a<sizeof atoms/sizeof *atoms;a++) for (int i=0;i<1000;i++) {
        seed=seed*6364136223846793005ull+1; uint64_t size=1+seed%300000;
        seed=seed*6364136223846793005ull+1; uint64_t lo=seed%size;
        seed=seed*6364136223846793005ull+1; uint64_t hi=lo+1+seed%(size-lo);
        check(size,lo,hi,atoms[a]);
    }
}
'''.replace('FUNCTION', function)
        with tempfile.TemporaryDirectory(prefix='nx2a-flush-ranges-') as tmp:
            c, exe = Path(tmp)/'check.c', Path(tmp)/'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)

    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_failed_wait_stops_before_readback(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = src.index('static void wait_fence(uint64_t f) {')
        wait = src[start:src.index('\n}\n', start)+3]
        completion = next(line for line in src.splitlines() if 'g_fring[c->fence % NFRING]' in line)
        harness = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
static struct { VkDevice dev; int has_timeline; } V;
static VkSemaphore g_timeline;
enum { NFRING=4 }; static VkFence g_fring[NFRING];
static VkResult result; static int reads, waits;
#define die(...) do { assert(!reads); _exit(91); } while (0)
static int mtl_wait(uint64_t f, uint64_t t) { (void)f; (void)t; assert(0); return 0; }
static VkResult vkWaitSemaphores(VkDevice dev, const VkSemaphoreWaitInfo *wi, uint64_t timeout) {
    (void)dev; assert(wi->semaphoreCount==1 && *wi->pValues==1 && timeout==UINT64_MAX); waits++; return result;
}
static VkResult vkWaitForFences(VkDevice dev, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    (void)dev; assert(n==1 && f==g_fring+1 && all && timeout==UINT64_MAX); waits++; return result;
}
WAIT
int main(int argc, char **argv) {
    assert(argc==3); V.has_timeline=atoi(argv[1]); result=(VkResult)atoi(argv[2]);
    struct { uint64_t fence; } item={1}, *c=&item;
COMPLETION
    reads++; assert(waits==1 && result==VK_SUCCESS); return 0;
}
'''.replace('WAIT', wait).replace('COMPLETION', completion)
        with tempfile.TemporaryDirectory(prefix='nx2a-gpu-wait-') as tmp:
            c, exe = Path(tmp)/'check.c', Path(tmp)/'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            for timeline in ('0', '1'):
                for result in ('0', '-4', '2'):  # success, device lost, timeout
                    run = subprocess.run([str(exe), timeline, result], capture_output=True, text=True, timeout=15)
                    self.assertEqual(run.returncode, 0 if result=='0' else 91, run.stderr)

    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_allocation_and_fallback(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        def function(name):
            start = src.index(name)
            return src[start:src.index('\n}\n', start)+3]
        usage = src[src.index('static const VkBufferUsageFlags POOL_USAGE0'):src.index('static VkResult pool_copied_alloc(')]
        start = src.index('        const char *e = getenv("DM_VK_POOL_CACHED")')
        select = src[start:src.index('\n    }\n    pthread_rwlock_wrlock(&g_pool_mu);', start)]
        harness = r'''
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
static struct { VkDevice dev; VkPhysicalDeviceMemoryProperties mem; VkPhysicalDeviceProperties props; int has_xfb; } V;
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *shadow; int noncoherent; uint64_t lo, hi; } vkpool_t;
static int failure, buffers, memories, mapped, creates, allocs, types[8];
static uint32_t allowed=3;
static VkResult failure_result=VK_ERROR_OUT_OF_DEVICE_MEMORY;
static uint8_t storage[128];
static int mem_diag_on(void) { return 0; }
static inline uint64_t mem_diag_ns(void) { return 0; }
static inline void mem_diag_note(unsigned phase, uint64_t t, uint64_t n) { (void)phase; (void)t; (void)n; }

#define VKLOG(...) ((void)0)
#define VKCHK(x) do { if ((x)<0) _exit(90); } while (0)
static void mem_diag_pool(uint64_t size, uint32_t bits, uint32_t type) { assert(size==65 && bits==allowed && type<2); }
static int fail(int stage) { if (failure==stage) { failure=0; return 1; } return 0; }
static VkResult vkCreateBuffer(VkDevice dev, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    (void)dev; (void)a; assert(ci->size==65 && ci->usage); creates++;
    if (fail(1)) return failure_result;
    assert(!buffers); buffers++; *out=(VkBuffer)(uintptr_t)1; return VK_SUCCESS;
}
static void vkDestroyBuffer(VkDevice dev, VkBuffer b, const VkAllocationCallbacks *a) {
    (void)dev; (void)a; assert(b && buffers==1); buffers--;
}
static void vkGetBufferMemoryRequirements(VkDevice dev, VkBuffer b, VkMemoryRequirements *r) {
    (void)dev; assert(b); r->size=128; r->alignment=64; r->memoryTypeBits=allowed;
}
static VkResult vkAllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    (void)dev; (void)a; assert(ai->allocationSize==128); types[allocs++]=(int)ai->memoryTypeIndex;
    if (fail(2)) return failure_result;
    assert(!memories); memories++; *out=(VkDeviceMemory)(uintptr_t)2; return VK_SUCCESS;
}
static void vkFreeMemory(VkDevice dev, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    (void)dev; (void)a; assert(m && memories==1 && !mapped && !buffers); memories--;
}
static VkResult vkMapMemory(VkDevice dev, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize n, VkMemoryMapFlags flags, void **out) {
    (void)dev; assert(m && !off && n==VK_WHOLE_SIZE && !flags);
    if (fail(3)) return failure_result;
    assert(!mapped); mapped++; *out=storage; return VK_SUCCESS;
}
static void unmap(VkDevice dev, VkDeviceMemory m) { (void)dev; assert(m && mapped==1); mapped--; }
static PFN_vkUnmapMemory vkUnmapMemory=unmap;
static VkResult vkBindBufferMemory(VkDevice dev, VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
    (void)dev; assert(b && m && !off && !mapped); return fail(4) ? failure_result : VK_SUCCESS;
}
static int cache_calls;
static VkResult cache(VkDevice dev, uint32_t n, const VkMappedMemoryRange *r) {
    (void)dev; assert(n==1 && r->sType==VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
    assert(r->memory==(VkDeviceMemory)(uintptr_t)2 && r->offset==0 && r->size==VK_WHOLE_SIZE);
    cache_calls++; return VK_SUCCESS;
}
static PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges=cache;
static PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges=cache;
MEM_TYPE
USAGE
ALLOCATE
CACHE
static void create(vkpool_t *p, uint64_t size) {
SELECT
}
static void drop(vkpool_t *p) {
    assert(p->buf && p->mem && p->shadow==storage && buffers==1 && memories==1 && mapped==1);
    vkUnmapMemory(V.dev,p->mem); vkDestroyBuffer(V.dev,p->buf,NULL); vkFreeMemory(V.dev,p->mem,NULL);
    assert(!buffers && !memories && !mapped);
}
int main(int argc, char **argv) {
    (void)argv; V.mem.memoryTypeCount=3; V.props.limits.nonCoherentAtomSize=64;
    V.mem.memoryTypes[0].propertyFlags=7;
    V.mem.memoryTypes[1].propertyFlags=11;
    V.mem.memoryTypes[2].propertyFlags=17;
    vkpool_t p={0};
    setenv("DM_VK_POOL_CACHED","1",1);
    if (argc>1) { failure=2; failure_result=VK_ERROR_DEVICE_LOST; create(&p,65); return 0; }
    for (int i=1;i<=4;i++) {
        failure=i; assert(pool_copied_alloc(&p,65,1)<0);
        assert(!p.buf && !p.mem && !p.shadow && !buffers && !memories && !mapped);
        allocs=creates=0;
    }
    create(&p,65); assert(types[0]==1 && p.noncoherent);
    p.hi=65; pool_cache_sync(&p,0,0,65); pool_cache_sync(&p,1,0,65); assert(cache_calls==2);
    drop(&p); allocs=creates=0;
    for (int i=1;i<=4;i++) {
        p=(vkpool_t){0}; failure=i; create(&p,65);
        assert(!p.noncoherent && types[allocs-1]==0 && creates==2);
        drop(&p); allocs=creates=0;
    }
    allowed=1; p=(vkpool_t){0}; create(&p,65);
    assert(!p.noncoherent && allocs==1 && types[0]==0 && creates==2); drop(&p); allocs=creates=0; allowed=3;
    V.mem.memoryTypes[1].propertyFlags=15; p=(vkpool_t){0}; create(&p,65);
    assert(types[0]==1 && !p.noncoherent);
    pool_cache_sync(&p,0,0,65); pool_cache_sync(&p,1,0,65); assert(cache_calls==2);
    drop(&p); allocs=creates=0; V.mem.memoryTypes[1].propertyFlags=11;
    vkFlushMappedMemoryRanges=NULL; p=(vkpool_t){0}; create(&p,65);
    assert(types[0]==0 && creates==1 && !p.noncoherent); drop(&p); allocs=creates=0; vkFlushMappedMemoryRanges=cache;
    unsetenv("DM_VK_POOL_CACHED"); p=(vkpool_t){0}; create(&p,65);
    assert(types[0]==0 && creates==1 && !p.noncoherent); drop(&p);
}
'''.replace('MEM_TYPE', function('uint32_t vk_mem_type(')).replace('USAGE', usage).replace('ALLOCATE', function('static VkResult pool_copied_alloc(')).replace('\nCACHE\n', '\n' + function('static void pool_cache_sync(') + '\n').replace('SELECT', select)
        with tempfile.TemporaryDirectory(prefix='nx2a-cached-pool-') as tmp:
            c, exe = Path(tmp)/'check.c', Path(tmp)/'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                            '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)
            result = subprocess.run([str(exe), 'device-lost'], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 90, result.stderr)
