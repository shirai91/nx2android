"""Fault-inject the actual zero-copy path without allocating GPU memory."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk


class MemoryDiagnostics(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_buffer_memory_compatibility(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = src.index('uint32_t vk_mem_type(')
        end = src.index('\n}\n', src.index('static void mem_diag_pool(')) + 3
        harness = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static struct { VkPhysicalDeviceMemoryProperties mem; } V;
static int enabled, messages;
static char last[1024];
static int mem_diag_on(void) { return enabled; }
static void log_message(const char *fmt, ...) {
    va_list a; va_start(a,fmt); vsnprintf(last,sizeof last,fmt,a); va_end(a); messages++;
}
#define VKLOG log_message
FUNCTIONS
int main(void) {
    V.mem.memoryTypeCount=3;
    V.mem.memoryTypes[0].propertyFlags=7;  // Mali coherent, uncached
    V.mem.memoryTypes[1].propertyFlags=11; // Mali cached, non-coherent
    V.mem.memoryTypes[2].propertyFlags=17; // not host-visible
    VkPhysicalDeviceMemoryProperties before=V.mem;
    const VkMemoryPropertyFlags current=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    assert(vk_mem_type(3,current,0)==0);
    mem_diag_pool(1048576,3,0); assert(!messages);
    enabled=1; mem_diag_pool(1048576,3,0);
    assert(messages==1 && strstr(last,"allowed-types 0x3, selected type 0 flags 0x7"));
    assert(strstr(last,"host-cached no") && strstr(last,"cached candidate 1"));
    mem_diag_pool(1048576,1,0); assert(strstr(last,"cached candidate -1"));
    mem_diag_pool(1048576,5,0); assert(strstr(last,"cached candidate -1"));
    assert(!memcmp(&before,&V.mem,sizeof before) && vk_mem_type(3,current,0)==0);
}
'''.replace('FUNCTIONS', src[start:end])
        with tempfile.TemporaryDirectory(prefix='nx2a-pool-types-') as tmp:
            c, exe = Path(tmp)/'check.c', Path(tmp)/'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)

    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_failure_stages_and_cleanup(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        helpers = src[src.index('// DM_VK_MEM_DIAG'):src.index('\nuint32_t vk_mem_type')]
        start = src.index('static int pool_zero_copy(')
        function = src[start:src.index('\n}\n', start) + 3]
        harness = r'''
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
struct ANativeWindow;
#include <vulkan/vulkan_android.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef struct AHardwareBuffer { int dummy; } AHardwareBuffer;
typedef struct { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; } AHardwareBuffer_Desc;
#define AHARDWAREBUFFER_FORMAT_BLOB 33
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN 3
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN 48
#define AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER 16777216
#define POOL_USAGE VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
typedef struct { VkBuffer buf; VkDeviceMemory mem; AHardwareBuffer *ahb; int zero_copy; } vkpool_t;
static struct { VkDevice dev; } V;
static int failure, released, closed, destroyed, messages;
static char last[1024];
static uint8_t temporary[64];
static uint64_t clock_ns;
static int mock_clock(clockid_t id, struct timespec *t) {
    (void)id; clock_ns += 1000000; t->tv_sec = clock_ns / 1000000000; t->tv_nsec = clock_ns % 1000000000; return 0;
}
#define clock_gettime mock_clock
static void log_message(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vsnprintf(last, sizeof last, fmt, a); va_end(a); messages++;
}
#define VKLOG(...) log_message(__VA_ARGS__)
static void nvn_dirty_stats(uint64_t *f, uint64_t *p) { *f = 7; *p = 3; }
static int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *d, AHardwareBuffer **out) {
    assert(d->format == AHARDWAREBUFFER_FORMAT_BLOB && d->width == 64);
    if (failure == 1) return -ENOMEM;
    static AHardwareBuffer a; *out = &a; return 0;
}
static void AHardwareBuffer_release(AHardwareBuffer *a) { assert(a); released++; }
static int mock_socketpair(int family, int type, int protocol, int *sv) {
    (void)type; assert(family == AF_UNIX && protocol == 0);
    if (failure == 2) { errno = EPERM; return -1; } sv[0] = 10; sv[1] = 11; return 0;
}
#define socketpair mock_socketpair
static int AHardwareBuffer_sendHandleToUnixSocket(AHardwareBuffer *a, int fd) {
    assert(a && fd == 10); return failure == 3 ? -EINVAL : 0;
}
static int recv_fds(int fd, int *fds, int cap) { assert(fd == 11 && cap == 16); fds[0] = 12; return failure == 4 ? -1 : 1; }
static int mock_close(int fd) { assert(fd >= 10 && fd <= 12); closed++; return 0; }
#define close mock_close
static void *mock_mmap(void *addr, size_t n, int prot, int flags, int fd, off_t off) {
    assert(n == 64 && prot == (PROT_READ | PROT_WRITE) && fd == 12 && off == 0);
    if (failure == (addr ? 6 : 5)) { errno = EACCES; return MAP_FAILED; }
    assert(flags == (addr ? MAP_SHARED | MAP_FIXED : MAP_SHARED)); return addr ? addr : temporary;
}
#define mmap mock_mmap
static int mock_munmap(void *p, size_t n) { assert(p == temporary && n == 64); return 0; }
#define munmap mock_munmap
static uint32_t vk_mem_type(uint32_t bits, uint32_t want, uint32_t avoid) { (void)bits; (void)want; (void)avoid; return 0; }
static VkResult vkGetAndroidHardwareBufferPropertiesANDROID(VkDevice d, AHardwareBuffer *a, VkAndroidHardwareBufferPropertiesANDROID *out) {
    (void)d; assert(a); out->allocationSize = 64; out->memoryTypeBits = 1; return failure == 7 ? VK_ERROR_UNKNOWN : VK_SUCCESS;
}
static VkResult vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *ci, const void *alloc, VkBuffer *out) {
    (void)d; (void)alloc; assert(ci->size == 64); *out = (VkBuffer)(uintptr_t)1; return failure == 8 ? VK_ERROR_UNKNOWN : VK_SUCCESS;
}
static VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const void *alloc, VkDeviceMemory *out) {
    (void)d; (void)alloc; assert(ai->allocationSize == 64); *out = (VkDeviceMemory)(uintptr_t)2; return failure == 9 ? VK_ERROR_UNKNOWN : VK_SUCCESS;
}
static VkResult vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
    (void)d; assert(b && m && off == 0); return failure == 10 ? VK_ERROR_UNKNOWN : VK_SUCCESS;
}
static void vkDestroyBuffer(VkDevice d, VkBuffer b, const void *alloc) { (void)d; (void)alloc; assert(b); destroyed++; }
HELPERS
FUNCTION
int main(void) {
    int enabled = getenv("DM_VK_MEM_DIAG") && atoi(getenv("DM_VK_MEM_DIAG"));
    static const char *const names[] = {"size", "AHardwareBuffer_allocate", "socketpair", "AHardwareBuffer_sendHandleToUnixSocket", "recv_fds", "mmap-temporary", "mmap-fixed", "vkGetAndroidHardwareBufferPropertiesANDROID", "vkCreateBuffer", "vkAllocateMemory", "vkBindBufferMemory"};
    uint8_t memory[64]; memset(memory, 23, sizeof memory);
    for (failure = 0; failure <= 10; failure++) {
        for (int repeat = 0; repeat < 2; repeat++) {
            vkpool_t p = {0}; released = closed = destroyed = 0;
            assert(pool_zero_copy(&p, memory, failure == 0 ? 0x100000000ull : 64) == (failure < 7 ? -1 : -2));
            assert(!p.zero_copy);
            assert(released == (failure >= 2));
            assert(closed == (failure < 3 ? 0 : failure < 5 ? 2 : 3));
            assert(destroyed == (failure >= 9));
            assert(messages == (enabled ? failure + 1 : 0));
            if (enabled) assert(strstr(last, names[failure]));
        }
    }
    failure = -1; released = closed = destroyed = 0; vkpool_t p = {0};
    assert(pool_zero_copy(&p, memory, 64) == 0 && p.zero_copy && p.ahb);
    assert(closed == 3 && released == 0 && destroyed == 0 && memcmp(memory, temporary, 64) == 0);
    uint64_t t = mem_diag_ns(); mem_diag_note(1, t, 1048576);
    assert(strstr(last, "cpu-to-gpu: last 1.00 ms / 1.00 MiB, mean 1.00 ms / 1.00 MiB (1 calls)"));
    assert(strstr(last, "fault-released-pages=7 prep-pages=3"));
    const char *phases[]={"pool-flush", "pool-invalidate", "merge-scan", "sync-scan", "sync-protect", "sync-upload", "sync-baseline"};
    for (unsigned i=0;i<7;i++) { t=mem_diag_ns(); mem_diag_note(i+3,t,2097152); assert(strstr(last,phases[i]) && strstr(last,"1.00 ms / 2.00 MiB")); }
    uint64_t counts[4]={1,2,3,4}, sizes[4]={1048576,2097152,3145728,4194304};
    mem_diag_baselines(counts,sizes); assert(strstr(last,"baseline-reason incomplete-other: last 4 copies / 4.00 MiB; total 4 copies / 4.00 MiB (1 syncs)"));
    mem_diag_baselines(counts,sizes); assert(strstr(last,"total 8 copies / 8.00 MiB (2 syncs)"));
    mem_diag_baselines(counts,sizes); int saved=messages; mem_diag_baselines(counts,sizes); assert(messages==saved);
    clock_ns+=1000000000ull; mem_diag_baselines(counts,sizes); assert(strstr(last,"total 20 copies / 20.00 MiB (5 syncs)"));
    mem_diag_copyback(20000000,12000000,3,4194304);
    assert(strstr(last,"copyback-breakdown: last total 20.00 ms, invalidate 12.00 ms (60.0%, 3 calls / 4.00 MiB), other 8.00 ms"));
    mem_diag_copyback(10000000,0,0,0);
    assert(strstr(last,"invalidate 0.00 ms (0.0%, 0 calls / 0.00 MiB), other 10.00 ms"));
    mem_diag_copyback(0,0,0,0); assert(strstr(last,"total 0.00 ms") && !strstr(last,"nan"));
    saved=messages; mem_diag_copyback(20000000,10000000,2,2097152); assert(messages==saved);
    clock_ns+=1000000000ull; mem_diag_copyback(20000000,10000000,2,2097152);
    assert(strstr(last,"mean total 14.00 ms, invalidate 6.40 ms (45.7%, 1.60 MiB), 5 submissions"));
    return 0;
}
'''.replace('HELPERS', helpers).replace('FUNCTION', function)
        with tempfile.TemporaryDirectory(prefix='nx2a-memdiag-test-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                            '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            for enabled in ('0', '1'):
                subprocess.run([str(exe)], env=dict(os.environ, DM_VK_MEM_DIAG=enabled), check=True)
