"""Host C unit tests for DM_VK_MINIMAL_BACK block-linear render-target write-back (gfx_vk_tex.c)."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class MinimalBackWriteback(unittest.TestCase):
    def _compile_harness(self, out_dir: Path) -> Path:
        tex = (ROOT / 'nxnative/dm/android/gfx_vk_tex.c').read_text(encoding='utf-8')
        header = (ROOT / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf-8')
        wb_section = csrc.between(
            tex,
            '// ---------------- block-linear render-target write-back (DM_VK_MINIMAL_BACK) ----------------',
            '// ---------------- views ----------------',
        )
        vkfmt_decl = csrc.between(header, 'typedef struct { VkFormat vk;', 'const vkfmt_t *vk_fmt')
        red_macro = next(l for l in header.splitlines() if l.startswith('#define VKRED_ONCE('))

        code = f'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nxnative/dm/gfx/nvn/nvn_obj.h"
#include "nxnative/dm/gfx/tex/nvn_deswizzle.h"

typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define pthread_mutex_lock(p) ((void)(p))
#define pthread_mutex_unlock(p) ((void)(p))
#define LOG(...) ((void)0)
#define VKLOG(...) ((void)0)
{red_macro}
{vkfmt_decl}
typedef struct vkimg {{
    VkImage img;
    uint32_t w, h, d, levels, layers, samples;
}} vkimg_t;

static struct {{ VkDevice dev; }} V = {{ .dev = (VkDevice)(uintptr_t)1 }};
static uint64_t g_last_fence;
static int g_buf_allocs, g_buf_frees, g_mem_allocs, g_mem_frees;
static int g_copies, g_barriers, g_host_barriers, g_write_preps, g_last_mem_flags;
static uint8_t *g_last_stage;
static VkDeviceSize g_last_stage_size;

uint64_t mtl_last_fence(void) {{ return g_last_fence; }}
static void vk_img_barrier_all(VkCommandBuffer cb) {{ (void)cb; g_barriers++; }}
static void vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                                 uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb,
                                 uint32_t ni, const VkImageMemoryBarrier *ib) {{
    (void)cb; (void)dep; (void)nb; (void)bb; (void)ni; (void)ib;
    if ((src & VK_PIPELINE_STAGE_TRANSFER_BIT) && (dst & VK_PIPELINE_STAGE_HOST_BIT) &&
        nm == 1 && (mb[0].srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) && (mb[0].dstAccessMask & VK_ACCESS_HOST_READ_BIT))
        g_host_barriers++;
}}
static void dm_guest_write_prepare(void *addr, size_t len) {{ (void)addr; (void)len; g_write_preps++; }}

static const vkfmt_t *vk_fmt(int f) {{
    static const vkfmt_t rg8  = {{ .vk = VK_FORMAT_R8G8_UNORM, .bpb = 2, .bw = 1, .bh = 1, .kind = 1 }};
    static const vkfmt_t bc1  = {{ .vk = VK_FORMAT_BC1_RGBA_UNORM_BLOCK, .bpb = 8, .bw = 4, .bh = 4, .compressed = 1, .kind = 1 }};
    static const vkfmt_t d32  = {{ .vk = VK_FORMAT_D32_SFLOAT, .bpb = 4, .bw = 1, .bh = 1, .depth = 1, .kind = 1 }};
    if (f == 0x0d) return &rg8;
    if (f == 0x42) return &bc1;
    if (f == 0x34) return &d32;
    return NULL;
}}

static uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {{
    (void)bits; (void)avoid;
    g_last_mem_flags = (int)want;
    return 0;
}}

typedef struct {{ VkDeviceSize size; uint8_t *data; }} mock_buf;
static VkResult vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {{
    (void)d; (void)a;
    mock_buf *b = calloc(1, sizeof *b); assert(b);
    b->size = ci->size; b->data = calloc(1, (size_t)ci->size); assert(b->data);
    g_buf_allocs++;
    *out = (VkBuffer)(uintptr_t)b;
    return VK_SUCCESS;
}}
static void vkGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *rq) {{
    (void)d; mock_buf *mb = (mock_buf *)(uintptr_t)b;
    *rq = (VkMemoryRequirements){{ .size = mb->size, .alignment = 16, .memoryTypeBits = 1 }};
}}
static VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {{
    (void)d; (void)ai; (void)a;
    g_mem_allocs++;
    *out = (VkDeviceMemory)(uintptr_t)0x2000;
    return VK_SUCCESS;
}}
static VkResult vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) {{
    (void)d; (void)m; (void)o;
    mock_buf *mb = (mock_buf *)(uintptr_t)b;
    g_last_stage = mb->data; g_last_stage_size = mb->size;
    return VK_SUCCESS;
}}
static VkResult vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize sz, VkMemoryMapFlags fl, void **pp) {{
    (void)d; (void)m; (void)o; (void)sz; (void)fl;
    *pp = g_last_stage;
    return VK_SUCCESS;
}}
static void vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage img, VkImageLayout lay, VkBuffer dst, uint32_t n, const VkBufferImageCopy *r) {{
    (void)cb; (void)lay; assert(n == 1);
    mock_buf *mb = (mock_buf *)(uintptr_t)dst;
    uint32_t tag = (uint32_t)(uintptr_t)img;
    uint32_t lv = r->imageSubresource.mipLevel;
    uint32_t lw = r->imageExtent.width, lh = r->imageExtent.height;
    uint8_t *p = mb->data + r->bufferOffset;
    for (uint32_t y = 0; y < lh; y++)
        for (uint32_t x = 0; x < lw; x++) {{
            size_t off = ((size_t)y * lw + x) * 2;
            p[off + 0] = (uint8_t)((x * 3 + y * 5 + lv * 17 + tag) & 0xff);
            p[off + 1] = (uint8_t)((x * 7 + y * 11 + lv * 29 + tag * 3) & 0xff);
        }}
    g_copies++;
}}
static void vkDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) {{
    (void)d; (void)a;
    mock_buf *mb = (mock_buf *)(uintptr_t)b;
    free(mb->data); free(mb); g_buf_frees++;
}}
static void vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {{
    (void)d; (void)m; (void)a; g_mem_frees++;
}}

{wb_section}

static tex_t make_tile(pool_t *pool, uint8_t *mem) {{
    tex_t t; memset(&t, 0, sizeof t);
    t.magic = MAGIC('T', 'E', 'X', 0);
    t.serial = 42;
    t.pool = pool;
    t.mem = mem;
    t.d.target = NVN_TEXTURE_TARGET_2D;
    t.d.format = 0x0d;   // RG8_UNORM
    t.d.width = 260;
    t.d.height = 260;
    t.d.depth = 1;
    t.d.levels = 9;
    t.d.samples = 1;
    t.d.flags = NVN_TEXTURE_FLAGS_MINIMAL_LAYOUT;
    nvn_tex_layout(&t.d, &t.L);
    return t;
}}

int main(int argc, char **argv) {{
    int expect_on = argc > 1 && atoi(argv[1]) != 0;
    pool_t pool = {{ .flags = 0x24 }};
    tex_t probe = make_tile(&pool, NULL);
    uint8_t *guest = calloc(1, (size_t)probe.L.size); assert(guest);
    tex_t t = make_tile(&pool, guest);
    vkimg_t im = {{ .img = (VkImage)(uintptr_t)100, .w = 260, .h = 260, .d = 1, .levels = 9, .layers = 1, .samples = 1 }};

    if (!expect_on) {{
        assert(!wb_want(&im, &t));
        wb_note_attachment(&im, &t);
        assert(g_nwb == 0);
        free(guest);
        return 0;
    }}

    // 1. Filter rules (general MINIMAL_LAYOUT uncompressed 2D color targets in CPU-accessible pools)
    assert(wb_want(&im, &t));
    tex_t bad = t; bad.d.flags = 0; assert(!wb_want(&im, &bad));
    bad = t; bad.d.flags |= NVN_TEXTURE_FLAGS_LINEAR; assert(!wb_want(&im, &bad));
    bad = t; bad.d.flags |= NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET; assert(!wb_want(&im, &bad));
    bad = t; bad.d.flags |= NVN_TEXTURE_FLAGS_SPARSE; assert(!wb_want(&im, &bad));
    bad = t; bad.d.format = 0x42; assert(!wb_want(&im, &bad));   // compressed
    bad = t; bad.d.format = 0x34; assert(!wb_want(&im, &bad));   // depth
    bad = t; bad.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; assert(!wb_want(&im, &bad));
    pool_t no_cpu = {{ .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS }};
    bad = t; bad.pool = &no_cpu; assert(!wb_want(&im, &bad));
    pool_t virt = {{ .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL }};
    bad = t; bad.pool = &virt; assert(!wb_want(&im, &bad));
    vkimg_t im_bad = im; im_bad.layers = 2; assert(!wb_want(&im_bad, &t));
    im_bad = im; im_bad.samples = 4; assert(!wb_want(&im_bad, &t));
    im_bad = im; im_bad.levels = 17; assert(!wb_want(&im_bad, &t));

    // 2. Deduplication within a submission + same-submission commit & completion before fence signals
    VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)0x10;
    g_last_fence = 10;
    wb_note_attachment(&im, &t);
    wb_note_attachment(&im, &t);
    assert(g_nwb == 1);
    vk_wb_commit(cb, 11);   // committed into submission 11 itself (no cross-submission lag)
    assert(g_copies == 9);
    assert(g_host_barriers == 1);
    assert((g_last_mem_flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    assert(g_buf_allocs == 1 && g_buf_frees == 0);

    // Earlier fence completion does not touch submission 11
    vk_wb_complete(10);
    assert(g_write_preps == 0 && g_buf_frees == 0 && g_nwb == 1);

    // Submission 11 completes: swizzled into guest RAM before fence wakes game, staging buffer freed
    vk_wb_complete(11);
    assert(g_write_preps == 1 && g_wb_done == 1 && g_buf_frees == 1 && g_mem_frees == 1 && g_nwb == 0);
    for (uint32_t lv = 0; lv < 9; lv++) {{
        uint32_t lw = 260u >> lv ? 260u >> lv : 1, lh = 260u >> lv ? 260u >> lv : 1;
        uint8_t *got = calloc(1, (size_t)lw * lh * 2); assert(got);
        assert(nvn_deswizzle(&t.d, lv, 0, guest, got, (size_t)lw * 2) == 0);
        for (uint32_t y = 0; y < lh; y++)
            for (uint32_t x = 0; x < lw; x++) {{
                size_t off = ((size_t)y * lw + x) * 2;
                assert(got[off + 0] == (uint8_t)((x * 3 + y * 5 + lv * 17 + 100u) & 0xff));
                assert(got[off + 1] == (uint8_t)((x * 7 + y * 11 + lv * 29 + 300u) & 0xff));
            }}
        free(got);
    }}

    // 3. Eviction while queued (state 0): cancelled cleanly without recording
    g_copies = 0; g_last_fence = 20;
    wb_note_attachment(&im, &t);
    assert(g_nwb == 1);
    wb_drop_img(&im);
    vk_wb_commit(cb, 21);
    assert(g_copies == 0 && g_nwb == 0);

    // 4. Eviction while copy is in flight (state 1): cancels write-back, frees staging buffer, no UAF or stale guest overwrite
    vkimg_t *dyn_im = calloc(1, sizeof *dyn_im); assert(dyn_im); *dyn_im = im;
    memset(guest, 0, (size_t)t.L.size);
    g_last_fence = 30;
    wb_note_attachment(dyn_im, &t);
    vk_wb_commit(cb, 31);
    assert(g_copies == 9);
    wb_drop_img(dyn_im);
    memset(dyn_im, 0xdd, sizeof *dyn_im); free(dyn_im);   // poison & free before completion
    vk_wb_complete(31);
    assert(g_wb_done == 1 && g_nwb == 0 && g_buf_allocs == g_buf_frees && g_mem_allocs == g_mem_frees);
    for (size_t i = 0; i < t.L.size; i++) assert(guest[i] == 0);

    // 5. Pool teardown while copy is in flight (state 1): waits for completion, frees VkBuffer, skips guest memcpy
    uint8_t *pool_mem = calloc(1, (size_t)t.L.size); assert(pool_mem);
    tex_t t2 = make_tile(&pool, pool_mem);
    int preps_before = g_write_preps;
    g_last_fence = 40;
    wb_note_attachment(&im, &t2);
    vk_wb_commit(cb, 41);
    wb_drop_pool(&pool);
    vk_wb_complete(41);
    assert(g_write_preps == preps_before);
    for (size_t i = 0; i < t2.L.size; i++) assert(pool_mem[i] == 0);
    assert(g_nwb == 0 && g_buf_allocs == g_buf_frees && g_mem_allocs == g_mem_frees);

    // 6. Multi-tile batching: 3 tiles in one submission share 1 staging buffer; dropping 1 pool mid-flight preserves the other 2
    pool_t p_keep = {{ .flags = 0x24 }}, p_drop = {{ .flags = 0x24 }};
    uint8_t *m0 = calloc(1, (size_t)t.L.size), *m1 = calloc(1, (size_t)t.L.size), *m2 = calloc(1, (size_t)t.L.size);
    assert(m0 && m1 && m2);
    tex_t bt[3] = {{ make_tile(&p_keep, m0), make_tile(&p_drop, m1), make_tile(&p_keep, m2) }};
    vkimg_t bi[3] = {{ im, im, im }};
    bi[0].img = (VkImage)(uintptr_t)201; bi[1].img = (VkImage)(uintptr_t)202; bi[2].img = (VkImage)(uintptr_t)203;
    int ba0 = g_buf_allocs, ma0 = g_mem_allocs, cp0 = g_copies;
    g_last_fence = 50;
    for (int k = 0; k < 3; k++) wb_note_attachment(&bi[k], &bt[k]);
    assert(g_nwb == 3);
    vk_wb_commit(cb, 51);
    assert(g_buf_allocs == ba0 + 1 && g_mem_allocs == ma0 + 1);   // exactly 1 shared staging allocation for all 3 tiles
    assert(g_copies == cp0 + 27);
    wb_drop_pool(&p_drop);
    vk_wb_complete(51);
    assert(g_nwb == 0 && g_buf_allocs == g_buf_frees && g_mem_allocs == g_mem_frees);
    for (size_t i = 0; i < t.L.size; i++) assert(m1[i] == 0);
    for (int k = 0; k < 3; k++) {{
        if (k == 1) continue;
        uint8_t *got = calloc(1, 260 * 260 * 2); assert(got);
        assert(nvn_deswizzle(&bt[k].d, 0, 0, k == 0 ? m0 : m2, got, 260 * 2) == 0);
        uint32_t tag = (uint32_t)(201 + k);
        assert(got[0] == (uint8_t)(tag & 0xff) && got[1] == (uint8_t)((tag * 3) & 0xff));
        free(got);
    }}
    free(m0); free(m1); free(m2);

    // 7. Command-buffer failure abort: vk_wb_abort releases staging buffer immediately without writing guest memory
    memset(guest, 0, (size_t)t.L.size);
    preps_before = g_write_preps;
    g_last_fence = 60;
    wb_note_attachment(&im, &t);
    vk_wb_commit(cb, 61);
    assert(g_nwb == 1 && g_buf_allocs == g_buf_frees + 1);
    vk_wb_abort(61);
    assert(g_nwb == 0 && g_buf_allocs == g_buf_frees && g_mem_allocs == g_mem_frees && g_write_preps == preps_before);
    for (size_t i = 0; i < t.L.size; i++) assert(guest[i] == 0);

    free(pool_mem);
    free(guest);
    return 0;
}}
'''
        src = out_dir / 'wb_test.c'
        exe = out_dir / ('wb_test.exe' if os.name == 'nt' else 'wb_test')
        src.write_text(code, encoding='utf-8')
        cmd = [
            'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT),
            '-I', str(ROOT / 'nxnative/dm/gfx/nvn'),
            '-I', str(ROOT / 'nxnative/dm/gfx/nvn/include'),
            '-I', str(ROOT / 'nxnative/dm/gfx/tex'),
            '-I', str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'),
            str(src),
            str(ROOT / 'nxnative/dm/gfx/nvn/nvn_tex.c'),
            str(ROOT / 'nxnative/dm/gfx/tex/nvn_deswizzle.c'),
            '-o', str(exe),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        return exe

    def test_minimal_layout_writeback_lifecycle_and_swizzle(self):
        with tempfile.TemporaryDirectory() as td:
            exe = self._compile_harness(Path(td))
            for val, flag in (('0', '0'), ('1', '1')):
                env = os.environ.copy()
                env['DM_VK_MINIMAL_BACK'] = val
                run = subprocess.run([str(exe), flag], env=env, capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, f'DM_VK_MINIMAL_BACK={val}: ' + run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
