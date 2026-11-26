"""Unit tests for cross-chunk memory pool buffer bridging and multi-chunk copies/fills/updates."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


def slice_between(source, start_marker, end_marker):
    a = source.index(start_marker)
    b = source.index(end_marker, a)
    return source[a:b]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


HARNESS = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

#undef VKLOG
#undef VKRED_ONCE
#define LOG(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define VKLOG(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define VKRED_ONCE(id, ...) do { g_red_count++; } while (0)
#define die(...) abort()

vkdev_t V;
#define DEFINE(f) PFN_##f f;
VK_FNS(DEFINE) VK_DEV_FNS(DEFINE)

static int g_red_count;
static uint64_t g_fence = 10, g_done = 0;
static uint32_t g_pool_gen = 1;
static int g_pass_open = 1, g_pass_ended = 0;

// Mock dirty-page tracker over one guest buffer
static uint8_t *g_guest_mem;
static size_t g_guest_size;
static uint64_t g_dirty_now = 1;
static uint64_t g_dirty_last_write = 0;
static int g_dirty_protected = 0, g_dirty_protect_calls = 0, g_dirty_release_calls = 0;
static const void *nvn_dirty_span(const void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, b = (uintptr_t)g_guest_mem;
    return (a >= b && a + len <= b + g_guest_size) ? (const void *)1 : NULL;
}
static void nvn_dirty_protect(const void *addr, size_t len) { (void)addr; (void)len; g_dirty_protected = 1; g_dirty_protect_calls++; }
static void nvn_dirty_release(const void *addr, size_t len) { (void)addr; (void)len; g_dirty_protected = 0; g_dirty_release_calls++; }
static uint64_t nvn_dirty_now(void) { return g_dirty_now; }
static int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp) {
    (void)addr; (void)len;
    if (!span) return 1;
    return g_dirty_last_write > stamp;
}
static void guest_cpu_write(size_t off, uint8_t val) {
    g_guest_mem[off] = val;
    g_dirty_last_write = ++g_dirty_now;
}

// Mock Vulkan buffer/memory/copy/fill/barrier sink
typedef struct { uint8_t *data; size_t size; int freed; } MockBuf;
static MockBuf *g_bufs[128]; static int g_nbuf;
static int g_copy_ops, g_fill_ops, g_barrier_ops, g_defer_frees;
static VkResult mock_create_buf(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *ac, VkBuffer *out) {
    (void)d; (void)ac;
    MockBuf *b = calloc(1, sizeof *b); assert(b);
    b->data = calloc(1, (size_t)ci->size); assert(b->data);
    b->size = (size_t)ci->size;
    g_bufs[g_nbuf++] = b;
    *out = (VkBuffer)b;
    return VK_SUCCESS;
}
static void mock_destroy_buf(VkDevice d, VkBuffer b, const VkAllocationCallbacks *ac) { (void)d; (void)ac; ((MockBuf *)b)->freed = 1; }
static void mock_reqs(VkDevice d, VkBuffer b, VkMemoryRequirements *r) { (void)d; *r = (VkMemoryRequirements){((MockBuf *)b)->size, 16, 1}; }
static VkResult mock_alloc(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *ac, VkDeviceMemory *out) {
    (void)d; (void)ai; (void)ac; *out = (VkDeviceMemory)(uintptr_t)(0x1000 + g_nbuf); return VK_SUCCESS;
}
static void mock_free_mem(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *ac) { (void)d; (void)m; (void)ac; }
static VkResult mock_bind(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off) { (void)d; (void)b; (void)m; assert(off == 0); return VK_SUCCESS; }
static void mock_copy(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r) {
    (void)cb;
    MockBuf *s = (MockBuf *)src, *d = (MockBuf *)dst;
    for (uint32_t i = 0; i < n; i++) {
        assert(r[i].srcOffset + r[i].size <= s->size);
        assert(r[i].dstOffset + r[i].size <= d->size);
        memcpy(d->data + r[i].dstOffset, s->data + r[i].srcOffset, (size_t)r[i].size);
        g_copy_ops++;
    }
}
static void mock_fill(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, uint32_t val) {
    (void)cb;
    MockBuf *d = (MockBuf *)dst;
    assert(off + size <= d->size && (off & 3) == 0 && (size & 3) == 0);
    for (VkDeviceSize o = 0; o < size; o += 4) memcpy(d->data + off + o, &val, 4);
    g_fill_ops++;
}
static void mock_barrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f,
                         uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
                         uint32_t ni, const VkImageMemoryBarrier *im) {
    (void)cb; (void)s; (void)d; (void)f; (void)nm; (void)m; (void)nb; (void)b; (void)ni; (void)im;
    g_barrier_ops++;
}

uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) { (void)bits; (void)want; (void)avoid; return 0; }
void vk_defer_free_buffer(VkBuffer b, VkDeviceMemory m) { (void)b; (void)m; g_defer_frees++; ((MockBuf *)b)->freed = 1; }
VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)1; }
void vk_img_barrier_all(VkCommandBuffer cb) { (void)cb; g_barrier_ops++; }
uint32_t vk_pool_gen(void) { return g_pool_gen; }
static int g_ubd_ov = 0;
int vk_ubd_overlaps(uint64_t a, uint64_t n) { (void)a; (void)n; return g_ubd_ov; }
void vk_end_pass_land(void) { if (g_pass_open) { g_pass_open = 0; g_pass_ended++; } g_ubd_ov = 0; }
static int g_gpu_wrote_calls = 0;
void vk_gpu_wrote(uint64_t a, uint64_t n) { g_gpu_wrote_calls++; vk_cross_invalidate(a, n); }
const void *nvn_sparse_address_pool(uint64_t a, uint64_t n) { (void)a; (void)n; return NULL; }
int nvn_sparse_address_translate(uint64_t a, uint64_t n, uint64_t *p) { (void)a; (void)n; (void)p; return 0; }

// Chunked pool table (zero-copy: each chunk's MockBuf->data points directly into g_guest_mem)
static vkpool_t g_pools[8];
static int g_npools;
vkpool_t *vk_pool_of(uint64_t addr, uint64_t len) {
    for (int i = 0; i < g_npools; i++) {
        if (addr >= g_pools[i].lo && len <= g_pools[i].hi - addr && addr < g_pools[i].hi)
            return &g_pools[i];
    }
    return NULL;
}
int vk_buf_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    if (!len) len = 1;
    if (len > UINT64_MAX - addr) return 0;
    vkpool_t *p = vk_pool_of(addr, len);
    if (!p) return 0;
    *buf = p->buf; *off = addr - p->lo + p->boff; return 1;
}

/* USAGE */
/* CROSS */

static void setup_chunked_pool(uint8_t *mem, size_t total, size_t chunk_sz) {
    g_guest_mem = mem; g_guest_size = total;
    uint64_t base = (uint64_t)(uintptr_t)mem;
    g_npools = 0;
    for (size_t off = 0; off < total; off += chunk_sz) {
        size_t sz = total - off < chunk_sz ? total - off : chunk_sz;
        MockBuf *mb = calloc(1, sizeof *mb);
        mb->data = mem + off; mb->size = sz;
        g_pools[g_npools] = (vkpool_t){
            .lo = base + off, .hi = base + off + sz, .buf = (VkBuffer)mb,
            .zero_copy = 1, .serial = (uint64_t)(g_npools + 1), .base = base, .end = base + total,
        };
        g_npools++;
    }
    g_pool_gen++;
}

int main(void) {
    vkCreateBuffer = mock_create_buf;
    vkDestroyBuffer = mock_destroy_buf;
    vkGetBufferMemoryRequirements = mock_reqs;
    vkAllocateMemory = mock_alloc;
    vkFreeMemory = mock_free_mem;
    vkBindBufferMemory = mock_bind;
    vkCmdCopyBuffer = mock_copy;
    vkCmdFillBuffer = mock_fill;
    vkCmdPipelineBarrier = mock_barrier;

    // 3 chunks of 256 KiB each (768 KiB total)
    enum { CHUNK = 256 << 10, TOTAL = 3 * CHUNK };
    uint8_t *mem = calloc(1, TOTAL);
    for (size_t i = 0; i < TOTAL; i++) mem[i] = (uint8_t)((i * 31 + 7) & 0xff);
    setup_chunked_pool(mem, TOTAL, CHUNK);
    uint64_t base = (uint64_t)(uintptr_t)mem;

    // 1. Non-crossing range returns 0 (fast path untouched)
    VkBuffer b = VK_NULL_HANDLE; VkDeviceSize o = 999;
    assert(!vk_cross_buf(base + 0x1000, 0x2000, &b, &o));
    assert(g_copy_ops == 0);

    // 2. 2-chunk crossing range (mirrors NieR's Stream 1 crossing chunk boundary)
    uint64_t vaddr = base + CHUNK - 0x4000;
    uint64_t vlen1 = 0x10000;
    g_pass_open = 1;
    assert(vk_cross_buf(vaddr, vlen1, &b, &o));
    assert(o == 0 && b != VK_NULL_HANDLE);
    assert(g_copy_ops == 2); // 2 chunks copied
    assert(g_pass_ended == 1);
    assert(memcmp(((MockBuf *)b)->data, mem + (CHUNK - 0x4000), vlen1) == 0);
    assert(g_dirty_protected == 1);

    // 3. Cache hit for exact range and smaller sub-ranges -> 0 copies, 0 pass breaks
    g_pass_open = 1;
    int copies_before = g_copy_ops, ended_before = g_pass_ended;
    VkBuffer b2 = VK_NULL_HANDLE; VkDeviceSize o2 = 999;
    assert(vk_cross_buf(vaddr, 0x8000, &b2, &o2));
    assert(b2 == b && o2 == 0);
    assert(vk_cross_buf(vaddr + 0x1000, 0x6000, &b2, &o2));
    assert(b2 == b && o2 == 0x1000);
    assert(g_copy_ops == copies_before && g_pass_ended == ended_before);

    // 3b. Unaligned base (e.g. 2-byte aligned UINT16 index buffer) rounds down to 16 bytes
    // so a subsequent 4-byte aligned sub-range preserves its exact modulo-4 offset.
    uint64_t uaddr = base + 2 * CHUNK - 0x2002;
    VkBuffer bu = VK_NULL_HANDLE, bu2 = VK_NULL_HANDLE; VkDeviceSize ou = 999, ou2 = 999;
    assert(vk_cross_buf(uaddr, 0x4000, &bu, &ou));
    assert((ou & 15) == (uaddr & 15) && ou == 14);
    assert(memcmp(((MockBuf *)bu)->data + ou, mem + (2 * CHUNK - 0x2002), 0x4000) == 0);
    assert(vk_cross_buf(uaddr + 2, 0x3ffc, &bu2, &ou2));
    assert(bu2 == bu && (ou2 & 3) == 0 && ou2 == 16);
    copies_before = g_copy_ops;

    // 4. Same-base growth within allocated headroom copies ONLY the extra tail
    uint64_t vlen2 = vlen1 + 0x100; // +256 bytes, like NieR's +0xadd680 -> +0xadd780
    assert(vk_cross_buf(vaddr, vlen2, &b2, &o2));
    assert(b2 == b && o2 == 0);
    assert(g_copy_ops == copies_before + 1); // only tail in chunk 2
    assert(memcmp(((MockBuf *)b2)->data, mem + (CHUNK - 0x4000), vlen2) == 0);

    // 5. Same-base growth past headroom (3 chunks!) reallocates & defers free of old buffer
    int frees_before = g_defer_frees;
    uint64_t vlen3 = CHUNK + 0x8000; // crosses chunk 0 -> chunk 1 -> chunk 2 (3 chunks!)
    assert(vk_cross_buf(vaddr, vlen3, &b2, &o2));
    assert(b2 != b && o2 == 0 && g_defer_frees == frees_before + 1);
    assert(memcmp(((MockBuf *)b2)->data, mem + (CHUNK - 0x4000), vlen3) == 0);
    b = b2;

    // 6. CPU write detected via dirty-page tracker triggers refresh
    guest_cpu_write(CHUNK + 0x123, 0xAB);
    copies_before = g_copy_ops;
    assert(vk_cross_buf(vaddr, vlen3, &b2, &o2));
    assert(b2 == b && g_copy_ops == copies_before + 3);
    assert(((MockBuf *)b2)->data[0x4000 + 0x123] == 0xAB);

    // 7. Repeated CPU writes switch entry to dynamic mode (releases protection, refreshes once per submission)
    guest_cpu_write(CHUNK + 0x124, 0xCD);
    assert(vk_cross_buf(vaddr, vlen3, &b2, &o2));
    int rel_before = g_dirty_release_calls;
    guest_cpu_write(CHUNK + 0x125, 0xEF);
    assert(vk_cross_buf(vaddr, vlen3, &b2, &o2));
    assert(g_dirty_release_calls > rel_before && g_dirty_protected == 0);
    // Same submission: cached without recopying
    copies_before = g_copy_ops;
    assert(vk_cross_buf(vaddr, vlen3, &b2, &o2));
    assert(g_copy_ops == copies_before);
    // Next submission: refreshes once
    g_fence++;
    mem[CHUNK + 0x126] = 0x5A;
    assert(vk_cross_buf(vaddr, vlen3, &b2, &o2));
    assert(g_copy_ops == copies_before + 3);
    assert(((MockBuf *)b2)->data[0x4000 + 0x126] == 0x5A);

    // 8. GPU write invalidation triggers immediate refresh even within same submission
    mem[CHUNK + 0x200] = 0x77;
    vk_cross_invalidate(base + CHUNK + 0x200, 16);
    copies_before = g_copy_ops;
    assert(vk_cross_buf(vaddr, vlen3, &b2, &o2));
    assert(g_copy_ops == copies_before + 3);
    assert(((MockBuf *)b2)->data[0x4000 + 0x200] == 0x77);

    // 9. Multi-chunk mtl_sub_copy & mtl_sub_fill across chunk boundaries
    mtl_sub_fill(base + CHUNK - 16, 48, 0x11223344u);
    for (size_t k = 0; k < 48; k += 4) {
        uint32_t v; memcpy(&v, mem + CHUNK - 16 + k, 4); assert(v == 0x11223344u);
    }
    mtl_sub_copy(base + CHUNK - 16, base + 2 * CHUNK - 20, 48);
    assert(memcmp(mem + 2 * CHUNK - 20, mem + CHUNK - 16, 48) == 0);

    // 10. Pool destruction drops cached bridge entries and defers freeing
    frees_before = g_defer_frees;
    cross_drop_pool(base);
    assert(g_defer_frees == frees_before + 2);
    assert(g_cross_live == 0 && g_cross_bytes == 0);
    assert(g_red_count == 0);

    free(mem);
    printf("PASS cross-chunk buffer tests\n");
    return 0;
}
'''


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class CrossChunkBuffers(unittest.TestCase):
    def test_bridge_cache_coherence_and_multi_chunk_transfers(self):
        core = (DM / 'android/gfx_vk_core.c').read_text(encoding='utf-8')
        header = (DM / 'android/gfx_vk.h').read_text(encoding='utf-8')
        header = '\n'.join(
            line for line in header.splitlines()
            if not any(x in line for x in ('VK_USE_PLATFORM_ANDROID_KHR', '#include <android/', '#include "dm.h"'))
        ).replace('<vulkan/vulkan.h>', '<vulkan/vulkan_core.h>')
        for name in ('vkCreateAndroidSurfaceKHR', 'vkGetAndroidHardwareBufferPropertiesANDROID'):
            header = header.replace('X(' + name + ')', '')

        usage = slice_between(core, 'static const VkBufferUsageFlags POOL_USAGE0', 'static VkResult pool_copied_alloc')
        transfers = slice_between(core, 'static int pool_range_ok(', '// an inline uniform update (UpdateUniformBuffer)')
        cross = slice_between(core, 'enum { CROSS_CAP = 16 };', '// GPU writes to pool memory')

        with tempfile.TemporaryDirectory(prefix='vk-cross-chunk-') as tmp:
            folder = Path(tmp)
            (folder / 'runtime.h').write_text(header, encoding='utf-8')
            code = HARNESS.replace('/* USAGE */', usage).replace('/* CROSS */', transfers + '\n' + cross)
            src = folder / 'check.c'
            exe = folder / 'check'
            src.write_text(code, encoding='utf-8')
            subprocess.run([
                'cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
                '-Wno-missing-field-initializers',
                '-I' + str(folder),
                '-I' + str(DM / 'third_party/vulkan-headers/include'),
                '-I' + str(DM / 'gfx/nvn'),
                '-I' + str(DM / 'gfx/nvn/include'),
                '-I' + str(DM / 'android'),
                str(src), '-o', str(exe),
            ], check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_replay_callers_bridge_cross_chunk_vertex_index_and_indirect_buffers(self):
        exec_src = csrc.unit(DM / 'android/gfx_vk_exec.c')
        self.assertIn('vk_cross_buf(S.vbuf[b].address, S.vbuf[b].size, &xvb[b], &xvo[b])', exec_src)
        self.assertIn('vk_cross_buf(d->ibuf, index_bytes, &xib, &xio)', exec_src)
        self.assertIn('vk_cross_buf(d->ind, ind_bytes, &xind, &xindo)', exec_src)
        self.assertIn('vk_cross_invalidate(a, orig_n ? orig_n : p->end - a)', exec_src)


if __name__ == '__main__':
    unittest.main()
