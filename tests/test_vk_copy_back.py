"""Copy-back must prepare protected guest pages and retain dirty tracking."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk


class CopyBack(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_readback_dependency(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        core = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = core.index('static void pools_readback_barrier(VkCommandBuffer cb) {')
        function = core[start:core.index('\n}\n', start)+3]
        harness = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stddef.h>
static int g_shadow_pools, calls;
static void vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
    uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    assert(cb && !flags && nm==1 && !nb && !b && !ni && !i);
    assert(src & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    assert(dst & VK_PIPELINE_STAGE_HOST_BIT);
    assert(m->srcAccessMask & VK_ACCESS_MEMORY_WRITE_BIT);
    assert(m->dstAccessMask & VK_ACCESS_HOST_READ_BIT);
    calls++;
}
FUNCTION
int main(void) {
    VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)1;
    pools_readback_barrier(cb); assert(calls==0); // zero-copy: no new synchronization
    g_shadow_pools=1; pools_readback_barrier(cb); pools_readback_barrier(cb); assert(calls==2);
    g_shadow_pools=0; pools_readback_barrier(cb); assert(calls==2);
}
'''.replace('FUNCTION', function)
        with tempfile.TemporaryDirectory(prefix='nx2a-readback-') as tmp:
            c, exe = Path(tmp)/'check.c', Path(tmp)/'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)

    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_protected_pools(self):
        dm = Path(__file__).resolve().parents[1] / 'nxnative/dm'
        dirty = (dm / 'gfx/nvn/nvn_dirty.c').read_text()
        dirty = '\n'.join(s for s in dirty.splitlines() if not s.startswith('#include'))
        core = (dm / 'android/gfx_vk_core.c').read_text()
        start = core.index('static void pools_copy_back(const struct vksub *s) {')
        copy_back = core[start:core.index('\n}\n', start) + 3]
        if 'static uint64_t pool_merge_back(' in core:
            copy_back = core[core.index('static uint64_t pool_merge_back('):start] + copy_back
        start = core.index('static void vk_pools_sync_(VkCommandBuffer cb) {')
        sync = core[start:core.index('\n}\n', start) + 3]
        start = core.index('static void pool_cache_sync(')
        cache_sync = core[start:core.index('\n}\n', start) + 3]
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static void mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = t->denom = 1; }
static uint64_t mach_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VKLOG LOG
#define die(...) do { LOG(__VA_ARGS__); abort(); } while (0)
static int (*fault_hook)(void *, int, void *);
static void (*write_hook)(void *, size_t);
static void dm_fault_hook_register(int (*fn)(void *, int, void *)) { fault_hook = fn; }
static void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { write_hook = fn; }
void dm_guest_write_prepare(void *a, size_t n) { if (write_hook) write_hook(a, n); }
static int fail_unprotect;
static int checked_mprotect(void *a, size_t n, int prot) {
    if (fail_unprotect && prot == (PROT_READ | PROT_WRITE)) {
        if (fail_unprotect == 3) { fail_unprotect = 1; return mprotect(a, n, prot); }
        if (fail_unprotect == 2) assert(mprotect(a, (size_t)getpagesize(), prot) == 0);
        fail_unprotect = 0; errno = ENOMEM; return -1;
    }
    return mprotect(a, n, prot);
}
#define mprotect checked_mprotect
DIRTY
typedef struct { uint64_t lo, hi, mem; uint8_t *shadow; int synced, flags, baseline_complete, noncoherent; uint8_t *baseline; uint64_t stamp, stamp0; const void *span; } vkpool_t;
static vkpool_t g_pool[5]; static int g_npool = 5;
static pthread_rwlock_t g_pool_mu = PTHREAD_RWLOCK_INITIALIZER;
struct vksub { struct { uint64_t lo, hi; } gw[2]; int ngw, readback_ranges; };
static int diag_enabled; static uint64_t scanned;
static int mem_diag_on(void) { return diag_enabled; }
static uint64_t reasons[4], reason_sizes[4];
static void mem_diag_baselines(const uint64_t c[4], const uint64_t b[4]) { memcpy(reasons,c,sizeof reasons); memcpy(reason_sizes,b,sizeof reason_sizes); }
static uint64_t mem_diag_ns(void) { static uint64_t ns; return ++ns; }
static uint64_t diag_invalidate_calls, diag_invalidate_bytes;
static void mem_diag_copyback(uint64_t total, uint64_t invalid_ns, uint64_t calls, uint64_t bytes) {
    assert(total>=invalid_ns && (calls ? invalid_ns>0 : invalid_ns==0));
    diag_invalidate_calls=calls; diag_invalidate_bytes=bytes;
}

static void mem_diag_note(unsigned phase, uint64_t t, uint64_t bytes) { (void)t; if (phase==5) scanned+=bytes; }
typedef void *VkCommandBuffer;
static uint64_t g_sync_bytes, g_sync_full;
typedef struct { int sType; const void *pNext; uint64_t memory, offset, size; } VkMappedMemoryRange;
#define VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE 6
#define VK_WHOLE_SIZE UINT64_MAX
#define VKCHK(x) do { if ((x)<0) _exit(88); } while (0)
static struct { int dev; struct { struct { uint64_t nonCoherentAtomSize; } limits; } props; } V;
static uint8_t *device_data;
static int flushes, invalidates, cache_failure;
static uint64_t last_flush_offset, last_flush_size, last_invalidate_offset, last_invalidate_size;
static int vkFlushMappedMemoryRanges(int dev, unsigned n, const VkMappedMemoryRange *r) {
    (void)dev; assert(n==1 && r->memory==1 && r->offset%64==0);
    if (cache_failure==13) return -1;
    size_t bytes=r->size==VK_WHOLE_SIZE ? g_pool[0].hi-g_pool[0].lo-r->offset : r->size;
    assert(r->offset+bytes<=g_pool[0].hi-g_pool[0].lo);
    memcpy(device_data+r->offset,g_pool[0].shadow+r->offset,bytes);
    last_flush_offset=r->offset; last_flush_size=r->size; flushes++; return 0;
}
static int vkInvalidateMappedMemoryRanges(int dev, unsigned n, const VkMappedMemoryRange *r) {
    (void)dev; assert(n==1 && r->memory==1 && r->offset%64==0);
    if (cache_failure==14) return -1;
    size_t bytes=r->size==VK_WHOLE_SIZE ? g_pool[0].hi-g_pool[0].lo-r->offset : r->size;
    assert(bytes>0 && r->offset+bytes<=g_pool[0].hi-g_pool[0].lo);
    assert(r->size==VK_WHOLE_SIZE || r->size%64==0);
    memcpy(g_pool[0].shadow+r->offset,device_data+r->offset,bytes);
    last_invalidate_offset=r->offset; last_invalidate_size=r->size; invalidates++; return 0;
}
CACHE_SYNC
static size_t snapshot_bytes;
static void *snapshot_memcpy(void *dst, const void *src, size_t n) {
    uintptr_t a=(uintptr_t)dst, b=(uintptr_t)g_pool[0].baseline;
    if (b && a>=b && a-b<g_pool[0].hi-g_pool[0].lo) snapshot_bytes+=n;
    return memcpy(dst,src,n);
}
#undef memcpy // macOS headers may provide a fortified memcpy macro
#define memcpy snapshot_memcpy
SYNC
#undef memcpy
COPY_BACK
static void handle_fault(int sig, siginfo_t *info, void *uc) {
    static volatile sig_atomic_t calls;
    if (++calls > 32) _exit(77); // bound the old repeated-fault failure
    (void)sig; if (!fault_hook(info->si_addr, 1, uc)) _exit(99);
}
// A reusable two-thread rendezvous; macOS has no pthread_barrier_t.
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; unsigned waiting, generation; } test_barrier_t;
#define TEST_BARRIER_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 }
static test_barrier_t cpu_start = TEST_BARRIER_INIT, cpu_done = TEST_BARRIER_INIT;
static void test_barrier_wait(test_barrier_t *b) {
    assert(!pthread_mutex_lock(&b->mu));
    unsigned generation = b->generation;
    if (++b->waiting == 2) {
        b->waiting = 0; b->generation++;
        assert(!pthread_cond_broadcast(&b->cv));
    } else {
        while (generation == b->generation) assert(!pthread_cond_wait(&b->cv, &b->mu));
    }
    assert(!pthread_mutex_unlock(&b->mu));
}
static void test_barrier_destroy(test_barrier_t *b) {
    assert(!pthread_cond_destroy(&b->cv)); assert(!pthread_mutex_destroy(&b->mu));
}
static void *cpu_writer(void *arg) {
    volatile uint8_t *mem = arg;
    for (int i=0;i<8;i++) { test_barrier_wait(&cpu_start); mem[16]=(uint8_t)(9+i); test_barrier_wait(&cpu_done); }
    return NULL;
}
int main(int argc, char **argv) {
    struct sigaction sa = {.sa_sigaction = handle_fault, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask); assert(sigaction(SIGSEGV, &sa, NULL) == 0);
    assert(sigaction(SIGBUS, &sa, NULL) == 0); // macOS reports protected-page writes as SIGBUS
    size_t pg = (size_t)getpagesize(), size = pg * (argc>1 && atoi(argv[1])>=10 ? 40 : 4);
    uint8_t *mem = mmap(NULL, size * 5, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED); memset(mem, 11, size * 5);
    if (argc > 1) {
        uint8_t *shadow = malloc(size); assert(shadow); memset(shadow, 23, size);
        uint8_t *baseline = malloc(size); assert(baseline); memset(baseline, 11, size);
        g_npool = 1; g_pool[0] = (vkpool_t){.lo=(uintptr_t)mem, .hi=(uintptr_t)(mem + size), .shadow=shadow, .synced=1, .baseline=baseline};
        int mode = atoi(argv[1]);
        if (mode == 8) {
            // Untracked/unaligned pools, partial final blocks, overlapping ranges and untouched guard bytes.
            for (size_t round = 0; round < 200; round++) {
                size_t n = 1 + (round * 997) % (size - 32);
                memset(mem, 0xa5, size * 5);
                g_pool[0].lo = (uintptr_t)(mem+13); g_pool[0].hi = g_pool[0].lo+n;
                g_pool[0].flags = round % 2 ? 2 : 4;
                for (size_t j = 0; j < n; j++) {
                    baseline[j] = (uint8_t)(j * 17 + round * 13);
                    shadow[j] = baseline[j] ^ (j % 7 == 0 ? 0x5a : 0);
                    mem[13+j] = baseline[j] ^ (j % 11 == 0 ? 0x33 : 0);
                }
                uint64_t a = g_pool[0].lo, b = g_pool[0].hi;
                struct vksub s = {.gw={{a-5,a+n/3},{a+n/(round%3 ? 2 : 4),b+5}}, .ngw=2};
                pools_copy_back(&s);
                for (size_t j = 0; j < n; j++) {
                    uint8_t original = (uint8_t)(j * 17 + round * 13);
                    int covered = !(g_pool[0].flags & 2) || a+j<s.gw[0].hi || a+j>=s.gw[1].lo;
                    uint8_t want = covered && j%7==0 ? shadow[j] : original ^ (j%11==0 ? 0x33 : 0);
                    assert(mem[13+j] == want);
                    assert(baseline[j] == (covered ? shadow[j] : original));
                }
                for (size_t j=0;j<13;j++) assert(mem[j]==0xa5);
                for (size_t j=13+n;j<size*5;j++) assert(mem[j]==0xa5);
            }
            free(baseline); free(shadow); assert(!munmap(mem,size*5)); return 0;
        }
        nvn_dirty_add_range(mem, size); nvn_dirty_protect(mem, size);
        if (mode==17) {
            diag_enabled=1; g_pool[0].synced=0; vk_pools_sync_(NULL);
            assert(reasons[0]==1 && reason_sizes[0]==size);
            nvn_dirty_remove_range(mem); g_pool[0].span=NULL; vk_pools_sync_(NULL);
            assert(reasons[1]==1 && reason_sizes[1]==size);
            nvn_dirty_add_range(mem,size); vk_pools_sync_(NULL); assert(reasons[0]==1);
            g_pool[0].flags=2; vk_pools_sync_(NULL); assert(reasons[2]==1 && reason_sizes[2]==size);
            g_pool[0].flags=4; vk_pools_sync_(NULL); assert(reasons[3]==1 && reason_sizes[3]==size);
            free(baseline); free(shadow); assert(!munmap(mem,size*5)); return 0;
        }
        if (mode==15 || mode==16 || mode==18 || mode==19) {
            int uncached=mode>=18, cached_memory=mode==16 || mode==19;
            if (uncached) g_pool[0].flags=2;
            if (cached_memory) { V.props.limits.nonCoherentAtomSize=64; g_pool[0].noncoherent=1; g_pool[0].mem=1; device_data=malloc(size); assert(device_data); }
            uint8_t *gpu=cached_memory ? device_data : shadow;
            g_pool[0].synced=0; vk_pools_sync_(NULL); diag_enabled=1;
            struct vksub s={.readback_ranges=1};
            dm_guest_write_prepare(mem+16,1); mem[16]=9;
            pools_copy_back(&s); assert(scanned==0 && mem[16]==9 && g_pool[0].baseline_complete);
            assert(!invalidates && !diag_invalidate_calls && !diag_invalidate_bytes);
            uint64_t sync_before=g_sync_bytes; snapshot_bytes=0;
            vk_pools_sync_(NULL); assert(shadow[16]==9 && !memcmp(baseline,shadow,size));
            assert(snapshot_bytes==g_sync_bytes-sync_before && snapshot_bytes<size);
            if (cached_memory) {
                // Writes into another pool must not invalidate this pool.
                s.ngw=1; s.gw[0].lo=(uintptr_t)(mem+size+8); s.gw[0].hi=s.gw[0].lo+4;
                pools_copy_back(&s); assert(!invalidates && !diag_invalidate_calls); vk_pools_sync_(NULL);
                // Unaligned disjoint GPU writes share atoms with independent CPU bytes.
                gpu[67]=42; gpu[130]=43; dm_guest_write_prepare(mem+66,1); mem[66]=19;
                s.ngw=2; s.gw[0].lo=(uintptr_t)(mem+130); s.gw[0].hi=s.gw[0].lo+1;
                s.gw[1].lo=(uintptr_t)(mem+67); s.gw[1].hi=s.gw[1].lo+1;
                pools_copy_back(&s);
                assert(invalidates==1 && last_invalidate_offset==64 && last_invalidate_size==128);
                assert(diag_invalidate_calls==1 && diag_invalidate_bytes==64);
                assert(mem[67]==42 && mem[130]==43 && mem[66]==19 && !memcmp(baseline,shadow,size));
                vk_pools_sync_(NULL); assert(device_data[66]==19 && !memcmp(baseline,shadow,size));
                // A range beyond the pool tail is clipped, then rounded to the allocation end.
                gpu[size-1]=28; s.ngw=1;
                s.gw[0].lo=(uintptr_t)(mem+size-3); s.gw[0].hi=(uintptr_t)(mem+size+17);
                pools_copy_back(&s);
                assert(last_invalidate_offset==size-64 && last_invalidate_size==VK_WHOLE_SIZE);
                assert(diag_invalidate_bytes==3 && mem[size-1]==28 && !memcmp(baseline,shadow,size));
                vk_pools_sync_(NULL);
                gpu[0]=29; s.gw[0].lo=(uintptr_t)mem-17; s.gw[0].hi=(uintptr_t)(mem+3);
                pools_copy_back(&s);
                assert(last_invalidate_offset==0 && last_invalidate_size==64);
                assert(diag_invalidate_bytes==3 && mem[0]==29 && !memcmp(baseline,shadow,size));
                vk_pools_sync_(NULL);
            }
            gpu[0]=31; gpu[size-1]=77; mem[16]=15;
            s.ngw=2; s.gw[0].lo=(uintptr_t)mem; s.gw[0].hi=(uintptr_t)(mem+4);
            s.gw[1].lo=(uintptr_t)(mem+size-4); s.gw[1].hi=(uintptr_t)(mem+size);
            scanned=0; pools_copy_back(&s);
            assert(scanned==8 && mem[0]==31 && mem[size-1]==77 && mem[16]==15);
            assert(g_pool[0].baseline_complete && !memcmp(baseline,shadow,size));
            vk_pools_sync_(NULL); assert(shadow[16]==15);
            for (int i=0;i<3;i++) { pools_copy_back(&s); snapshot_bytes=0; vk_pools_sync_(NULL); }
            pools_copy_back(&s); snapshot_bytes=0; vk_pools_sync_(NULL); assert(snapshot_bytes==0);
            // An uncertain writer disables range mode: even writes outside the transfer list must be merged.
            s.readback_ranges=0; gpu[20]=55; scanned=0; pools_copy_back(&s);
            if (cached_memory) assert(last_invalidate_offset==0 && last_invalidate_size==VK_WHOLE_SIZE && diag_invalidate_bytes==size);
            if (uncached) {
                // CPU_UNCACHED pools trust the tracked ranges, as before the baseline: no full snapshot at the next sync
                // (Unicorn Overlord's 860 MB pool would be copied before every submission)
                assert(scanned==8 && mem[20]==11 && g_pool[0].baseline_complete);
                snapshot_bytes=0; vk_pools_sync_(NULL); assert(snapshot_bytes<size);
                s.readback_ranges=1; gpu[0]=66; mem[16]=27; pools_copy_back(&s);
                assert(mem[0]==66 && mem[16]==27 && g_pool[0].baseline_complete);
                sync_before=g_sync_bytes; snapshot_bytes=0; vk_pools_sync_(NULL);
                assert(snapshot_bytes==g_sync_bytes-sync_before && !memcmp(baseline,shadow,size));
            } else assert(scanned==size && mem[20]==55 && g_pool[0].baseline_complete);
            if (cached_memory) free(device_data);
            free(baseline); free(shadow); assert(!munmap(mem,size*5)); return 0;
        }
        if (mode>=12) {
            V.props.limits.nonCoherentAtomSize=64;
            g_pool[0].noncoherent=1; g_pool[0].mem=1;
            device_data=malloc(size); assert(device_data); memset(device_data,0x55,size);
            cache_failure=mode==13 ? 13 : 0;
        }
        if (mode >= 4) {
            g_pool[0].synced = 0;
            if (mode == 7) { free(baseline); g_pool[0].baseline = NULL; }
            vk_pools_sync_(NULL);
            baseline = g_pool[0].baseline; assert(baseline && !memcmp(baseline, shadow, size));
            if (mode>=12) {
                assert(flushes==1 && !memcmp(device_data,shadow,size));
                cache_failure=mode==14 ? 14 : 0;
                device_data[0]=7; device_data[size-1]=99;
                dm_guest_write_prepare(mem+16,1); mem[16]=9;
                struct vksub s={0}; pools_copy_back(&s);
                assert(invalidates==1 && mem[0]==7 && mem[size-1]==99 && mem[16]==9);
                vk_pools_sync_(NULL);
                assert(flushes==2 && device_data[16]==9 && !memcmp(baseline,shadow,size));
                dm_guest_write_prepare(mem+16,1); mem[16]=15;
                pools_copy_back(&s); assert(mem[16]==15);
                vk_pools_sync_(NULL); assert(device_data[16]==15);
                // Partial completion still invalidates data outside its merge ranges. The untracked byte stays in the GPU copy only:
                // CPU_UNCACHED pools trust the tracked ranges, so the next sync takes no full snapshot.
                g_pool[0].flags=2; device_data[0]=31; device_data[size-1]=77;
                s.ngw=1; s.gw[0].lo=(uintptr_t)mem; s.gw[0].hi=(uintptr_t)(mem+4);
                pools_copy_back(&s); assert(mem[0]==31 && mem[size-1]==99 && shadow[size-1]==77);
                vk_pools_sync_(NULL); assert(baseline[size-1]==99 && !memcmp(baseline,shadow,size-1));
                for (int i=0;i<3;i++) { pools_copy_back(&s); vk_pools_sync_(NULL); }
                int before=flushes; pools_copy_back(&s); vk_pools_sync_(NULL); assert(flushes==before);
                dm_guest_write_prepare(mem+16,1); mem[16]=21; vk_pools_sync_(NULL);
                assert(device_data[16]==21 && last_flush_offset==0 && last_flush_size==(64u<<10));
                free(device_data); free(baseline); free(shadow); assert(!munmap(mem,size*5)); return 0;
            }
            if (mode == 10 || mode == 11) {
                // Multiple 64 KB blocks, including a partial tail. A CPU_UNCACHED pool's partial merge trusts its tracked ranges: an untracked
                // GPU byte stays in the GPU copy only, and the next sync takes no full snapshot (it copied 860 MB pools before every submission).
                struct vksub s = {0}; g_pool[0].flags = mode==11 ? 2 : 4;
                shadow[size-1]=23; pools_copy_back(&s);
                assert(mem[size-1]==(mode==11 ? 11 : 23));
                dm_guest_write_prepare(mem,1); mem[0]=7;
                uint64_t before=g_sync_bytes; snapshot_bytes=0;
                vk_pools_sync_(NULL);
                assert(shadow[0]==7 && baseline[0]==7);
                if (mode==11) assert(baseline[size-1]==11 && shadow[size-1]==23 && !memcmp(baseline,shadow,size-1));
                else assert(!memcmp(baseline,shadow,size));
                assert(snapshot_bytes==g_sync_bytes-before && snapshot_bytes<size);
                // No CPU or GPU changes after stamps settle: no snapshot reads.
                for (int i=0;i<3;i++) { pools_copy_back(&s); snapshot_bytes=0; vk_pools_sync_(NULL); }
                assert(snapshot_bytes==0);
                // A later GPU update must still preserve independent CPU data in the same cache line.
                dm_guest_write_prepare(mem+16,1); mem[16]=9; shadow[0]=31;
                s.gw[0].lo=(uintptr_t)mem; s.gw[0].hi=(uintptr_t)(mem+4); s.ngw=1;
                pools_copy_back(&s); assert(mem[0]==31 && mem[16]==9);
                vk_pools_sync_(NULL); assert(shadow[16]==9 && !memcmp(baseline,shadow,mode==11 ? size-1 : size));
                if (mode==11) {   // a later tracked range over the untracked byte merges it
                    s.gw[0].lo=(uintptr_t)(mem+size-4); s.gw[0].hi=(uintptr_t)(mem+size);
                    pools_copy_back(&s); assert(mem[size-1]==23 && !memcmp(baseline,shadow,size));
                }
                free(baseline); free(shadow); assert(!munmap(mem,size*5)); return 0;
            }
            if (mode == 9) {
                pthread_t cpu; assert(!pthread_create(&cpu,NULL,cpu_writer,mem));
                struct vksub s = {0};
                for (int i=0;i<8;i++) {
                    if (i) vk_pools_sync_(NULL);
                    test_barrier_wait(&cpu_start); test_barrier_wait(&cpu_done);
                    shadow[0]=(uint8_t)(31+i); pools_copy_back(&s);
                    assert(mem[0]==(uint8_t)(31+i) && mem[16]==(uint8_t)(9+i));
                }
                assert(!pthread_join(cpu,NULL)); test_barrier_destroy(&cpu_start); test_barrier_destroy(&cpu_done);
                free(baseline); free(shadow); assert(!munmap(mem,size*5)); return 0;
            }
            // CPU changes bytes the GPU does not write, including bytes in the same cache line.
            memset(mem + 16, 9, 4); memset(mem + pg + 32, 13, 4);
            if (mode != 6) { memset(shadow, 7, 4); shadow[size - 1] = 0; }
            struct vksub s = {.gw={{(uintptr_t)mem, (uintptr_t)(mem+size)}}, .ngw=mode==5};
            g_pool[0].flags = mode==5 ? 2 : 4;
            uint64_t before = nvn_dirty_now();
            pools_copy_back(&s);
            assert(mem[16]==9 && mem[pg+32]==13);
            assert(mem[0]==(mode==6 ? 11 : 7) && mem[size-1]==(mode==6 ? 11 : 0));
            assert(!memcmp(baseline, shadow, size));
            if (mode==6) assert(nvn_dirty_now()==before); // no GPU change: no extra invalidation
            vk_pools_sync_(NULL);
            assert(shadow[16]==9 && shadow[pg+32]==13 && !memcmp(baseline, shadow, size));
            memset(mem+16,15,4); shadow[8]=0;
            pools_copy_back(&s);
            assert(mem[16]==15 && mem[8]==0 && !memcmp(baseline,shadow,size));
            free(baseline); free(shadow); assert(!munmap(mem,size*5)); return 0;
        }
        fail_unprotect = atoi(argv[1]);
        if (fail_unprotect == 3) {
            // The first page succeeds; releasing the rest of its span fails in the signal handler.
            *(volatile uint8_t *)mem = 42; *(volatile uint8_t *)(mem + pg) = 42; return 0;
        }
        struct vksub s = {0}; pools_copy_back(&s); return 0;
    }
    // An overlapping registration must also be unprotected and marked dirty.
    nvn_dirty_add_range(mem + pg / 2, pg); nvn_dirty_protect(mem + pg / 2, pg);
    for (int i = 0; i < 5; i++) {
        uint8_t *a = mem + size * i;
        g_pool[i] = (vkpool_t){.lo=(uintptr_t)a, .hi=(uintptr_t)(a + size), .shadow=malloc(size), .synced=1, .baseline=malloc(size)};
        assert(g_pool[i].shadow && g_pool[i].baseline); memset(g_pool[i].shadow, 23, size); memset(g_pool[i].baseline,11,size);
        nvn_dirty_add_range(a, size); nvn_dirty_protect(a, size);
    }
    g_pool[1].flags = g_pool[4].flags = 2;
    g_pool[2].synced = 0;
    free(g_pool[3].shadow); g_pool[3].shadow = NULL; // zero-copy path
    uint64_t a = g_pool[1].lo;
    struct vksub s = {.gw = {{a - pg, a + pg / 2}, {a + pg * 2 + 17, a + size + pg}}, .ngw = 2};
    uint64_t stamp = nvn_dirty_now();
    pools_copy_back(&s);
    uint64_t faults, preps; nvn_dirty_stats(&faults, &preps);
    assert(faults == 0 && preps == 9); // 4 cached + 3 uncached + 2 overlapping pages
    for (int i = 0; i < 5; i++) {
        for (size_t k = 0; k < size; k++) {
            int changed = i == 0 || (i == 1 && (k < pg / 2 || k >= pg * 2 + 17));
            assert(mem[size * i + k] == (changed ? 23 : 11));
        }
        assert(nvn_dirty_since(mem + size * i, size, stamp) == (i < 2));
        free(g_pool[i].shadow);
        free(g_pool[i].baseline);
    }
    assert(nvn_dirty_since(mem + pg / 2, pg, stamp));
    // Re-protection must still catch a subsequent guest write normally.
    nvn_dirty_protect(mem, pg); stamp = nvn_dirty_now();
    *(volatile uint8_t *)mem = 42;
    assert(nvn_dirty_since(mem, pg, stamp));
    nvn_dirty_stats(&faults, &preps); assert(faults > 0);
    assert(munmap(mem, size * 5) == 0);
    return 0;
}
'''.replace('DIRTY', dirty).replace('CACHE_SYNC', cache_sync).replace('SYNC', sync).replace('COPY_BACK', copy_back)
        with tempfile.TemporaryDirectory(prefix='nx2a-copyback-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                            '-Wno-misleading-indentation', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)
            for case in ('4', '5', '6', '7', '8', '9', '10', '11', '12', '15', '16', '17', '18', '19'):
                subprocess.run([str(exe), case], check=True, timeout=15)
            for case in ('13', '14'):
                result = subprocess.run([str(exe), case], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 88, result.stderr)
            for failure in ('1', '2', '3'):  # total/partial failure before copying, and failure in the signal handler
                result = subprocess.run([str(exe), failure], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn('cannot make guest memory writable; stopping', result.stderr)
