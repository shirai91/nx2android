"""Fault-inject the actual zero-copy path without allocating GPU memory."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk
import sys; sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_cc   # noqa: E402


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

    @unittest.skipUnless(host_cc.posix(), 'a POSIX host C compiler is required (<sys/mman.h>, pthread, setenv)')
    def test_failure_stages_and_cleanup(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        helpers = src[src.index('// DM_VK_MEM_DIAG'):src.index('\nuint32_t vk_mem_type')]
        start = src.index('typedef struct { int fd; uint64_t off; uint8_t *map; int prot; } zcfill_t;')   # zc_fill, zc_place, pool_zero_copy
        function = src[start:src.index('\n}\n', src.index('static int pool_zero_copy(')) + 3]
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
// Only the socketpair mock sees this Linux-only flag on macOS.
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC (1 << 24)
#endif
typedef struct AHardwareBuffer { int dummy; } AHardwareBuffer;
typedef struct { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; } AHardwareBuffer_Desc;
#define AHARDWAREBUFFER_FORMAT_BLOB 33
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN 3
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN 48
#define AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER 16777216
#define POOL_USAGE VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
typedef struct { VkBuffer buf; VkDeviceMemory mem; AHardwareBuffer *ahb; int zero_copy; uint64_t boff; void *slab; uint32_t slot; uint64_t left; } vkpool_t;
static struct { VkDevice dev; } V;
static int failure, released, closed, destroyed, freed, messages, swaps, anon, moved, fixed_prot = PROT_READ | PROT_WRITE;
typedef struct slab_t { int fd; uint8_t *map; VkBuffer buf; uint64_t slot_bytes; } slab_t;   // no slab and no kept buffer here: a hardware buffer of its own
typedef struct { AHardwareBuffer *ahb; VkBuffer buf; VkDeviceMemory mem; } zcfree_t;
static int slab_take(uint64_t size, slab_t **out, uint32_t *slot) { (void)size; (void)out; (void)slot; return 0; }
static void slab_give(slab_t *s, uint32_t i, uint64_t fence) { (void)s; (void)i; (void)fence; assert(0); }
static int zcf_take(uint64_t size, zcfree_t *out) { (void)size; (void)out; return 0; }
static void zcf_free_now(const zcfree_t *e) { (void)e; assert(0); }
static int swap_rc[4], nq; static size_t swap_done[4];   // answers for the next swaps (a queue), then fill as asked
static int nvn_dirty_swap(void *mem, size_t len, int (*fill)(void *at, size_t n, size_t off, void *ctx), void *ctx, size_t *done) {
    swaps++;
    if (nq) { int rc = swap_rc[0]; size_t d = swap_done[0]; nq--; memmove(swap_rc, swap_rc + 1, sizeof swap_rc - sizeof *swap_rc); memmove(swap_done, swap_done + 1, sizeof swap_done - sizeof *swap_done); if (done) *done = d; return rc; }
    int rc = fill(mem, len, 0, ctx); if (done) *done = rc ? 0 : len; return rc;
}
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
    assert(family == AF_UNIX && type == (SOCK_SEQPACKET | SOCK_CLOEXEC) && protocol == 0);
    if (failure == 2) { errno = EPERM; return -1; } sv[0] = 10; sv[1] = 11; return 0;
}
#define socketpair mock_socketpair
static int AHardwareBuffer_sendHandleToUnixSocket(AHardwareBuffer *a, int fd) {
    assert(a && fd == 10); return failure == 3 ? -EINVAL : 0;
}
static int recv_fds(int fd, int *fds, int cap) { assert(fd == 11 && cap == 16); fds[0] = 12; return failure == 4 ? -1 : 1; }
static int mock_close(int fd) { assert(fd >= 10 && fd <= 12); closed++; return 0; }
#define close mock_close
static uint8_t anon_pages[64];
static void *mock_mmap(void *addr, size_t n, int prot, int flags, int fd, off_t off) {
    if (flags & MAP_ANONYMOUS) {   // copy_back (fixed, over the pool) or zc_anon_fill (new pages)
        assert(fd == -1 && n <= 64 && prot == (PROT_READ | PROT_WRITE) && (flags & MAP_PRIVATE)); anon++; return addr ? addr : anon_pages; }
    assert(n == 64 && prot == (addr ? fixed_prot : PROT_READ | PROT_WRITE) && fd == 12 && off == 0);
    if (failure == (addr ? 6 : 5)) { errno = EACCES; return MAP_FAILED; }
    assert(flags == (addr ? MAP_SHARED | MAP_FIXED : MAP_SHARED)); return addr ? addr : temporary;
}
#define mmap mock_mmap
static int mock_munmap(void *p, size_t n) { assert(p == temporary && n == 64); return 0; }
#define munmap mock_munmap
static int mock_mprotect(void *p, size_t n, int prot) { assert(p == anon_pages && n <= 64 && prot == PROT_READ); return 0; }
#define mprotect mock_mprotect
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1   // (macOS has no mremap)
#define MREMAP_FIXED 2
#endif
static void *mock_mremap(void *old, size_t n, size_t n2, int flags, void *to) { assert(old == anon_pages && n == n2 && flags == (MREMAP_MAYMOVE | MREMAP_FIXED)); memcpy(to, old, n); moved++; return to; }
#define mremap mock_mremap
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
static void vkFreeMemory(VkDevice d, VkDeviceMemory m, const void *alloc) { (void)d; (void)alloc; assert(m); freed++; }
HELPERS
FUNCTION
int main(void) {
    int enabled = getenv("DM_VK_MEM_DIAG") && atoi(getenv("DM_VK_MEM_DIAG"));
    static const char *const names[] = {"size", "AHardwareBuffer_allocate", "socketpair", "AHardwareBuffer_sendHandleToUnixSocket", "recv_fds", "mmap-temporary", "mmap-fixed", "vkGetAndroidHardwareBufferPropertiesANDROID", "vkCreateBuffer", "vkAllocateMemory", "vkBindBufferMemory"};
    uint8_t memory[64]; memset(memory, 23, sizeof memory);
    for (failure = 0; failure <= 10; failure++) {
        for (int repeat = 0; repeat < 2; repeat++) {
            vkpool_t p = {0}; released = closed = destroyed = freed = anon = 0;
            assert(pool_zero_copy(&p, memory, failure == 0 ? 0x100000000ull : 64, 0) == (failure < 7 ? -1 : -2));
            assert(!p.zero_copy && !p.left);
            assert(released == (failure >= 2));
            assert(closed == (failure < 3 ? 0 : failure < 5 ? 2 : 3));
            assert(destroyed == (failure >= 9));
            assert(freed == (failure == 10));   // bound memory that failed to bind is freed, not leaked
            assert(anon == (failure >= 7));     // an import that failed after the copy: the pages go back to anonymous memory before the buffer goes
            assert(messages == (enabled ? failure + 1 : 0));
            if (enabled) assert(strstr(last, names[failure]));
        }
    }
    failure = -1; released = closed = destroyed = 0; vkpool_t p = {0};
    assert(pool_zero_copy(&p, memory, 64, 0) == 0 && p.zero_copy && p.ahb);
    assert(closed == 3 && released == 0 && destroyed == 0 && memcmp(memory, temporary, 64) == 0 && !swaps);
    // a lazy pool backed while the game runs: the pages replaced under the tracker and mapped read-only for it; pages it does not hold are left alone
    fixed_prot = PROT_READ; released = closed = 0; memset(memory, 41, sizeof memory); vkpool_t l = {0};
    assert(pool_zero_copy(&l, memory, 64, 1) == 0 && l.zero_copy && swaps == 1 && closed == 3 && !released && memcmp(memory, temporary, 64) == 0);
    swap_rc[0] = 1; nq = 1; released = closed = 0; vkpool_t u = {0};
    assert(pool_zero_copy(&u, memory, 64, 1) == 1 && !u.zero_copy && !u.buf && swaps == 2 && closed == 3 && released == 1);
    swap_rc[0] = -1; nq = 1; released = 0; vkpool_t b = {0};
    assert(pool_zero_copy(&b, memory, 64, 1) == -1 && !b.zero_copy && released == 1 && swaps == 3);
    // written by the host lately: nothing swapped, try later (2), nothing to put back
    swap_rc[0] = 2; nq = 1; released = anon = 0; vkpool_t w = {0};
    assert(pool_zero_copy(&w, memory, 64, 1) == 2 && !w.zero_copy && !w.left && released == 1 && swaps == 4 && !anon);
    // a swap failing after its first 32 bytes: those go back to anonymous pages under the tracker (a second swap with zc_anon_fill), then the buffer is dropped
    memset(memory, 57, sizeof memory); swap_rc[0] = -6; swap_done[0] = 32; nq = 1; released = anon = moved = 0; vkpool_t h = {0};
    assert(pool_zero_copy(&h, memory, 64, 1) == -1 && !h.zero_copy && !h.left && swaps == 6 && anon == 1 && moved == 1 && released == 1);
    for (int i = 0; i < 64; i++) assert(memory[i] == 57);
    // and when that cannot be done now (the host wrote into the pool meanwhile): left says how much stays on the dropped buffer's pages
    swap_rc[0] = -6; swap_done[0] = 32; swap_rc[1] = 2; swap_done[1] = 0; nq = 2; released = anon = moved = 0; vkpool_t k = {0};
    assert(pool_zero_copy(&k, memory, 64, 1) == -1 && k.left == 32 && swaps == 8 && !moved && released == 1);
    // an import failing after a lazy pool's swap: the whole pool goes back the same way
    failure = 10; released = anon = moved = freed = 0; vkpool_t f = {0};
    assert(pool_zero_copy(&f, memory, 64, 1) == -2 && !f.left && freed == 1 && moved == 1 && swaps == 10 && released == 1);
    failure = -1;
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
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-Wno-misleading-indentation',
                            '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            for enabled in ('0', '1'):
                subprocess.run([str(exe)], env=dict(os.environ, DM_VK_MEM_DIAG=enabled), check=True)
