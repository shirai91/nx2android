"""Exercise texel buffer view retirement with the real hash table and mocked Vulkan."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk


class TexelBufferCache(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_pool_retirement(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (DM / 'android/gfx_vk_tex.c').read_text(encoding='utf-8')
        defer_decl = next(line for line in (DM / 'android/gfx_vk.h').read_text().splitlines() if line.startswith('void vk_defer_free_buffer_view('))
        start = src.index('typedef struct { uint64_t mem, pool; uint32_t fmt, n; } tbuf_key;')
        helper = src[start:src.index('// CPU readback of texels', start)]
        code = r'''
#include <assert.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include "nvn_hmap.h"
typedef struct { const void *mem; struct { uint32_t format, width; } d; } tex_t;
typedef struct { uint64_t serial; } vkpool_t;
typedef struct { VkFormat vk; unsigned bpb; } vkfmt_t;
static struct { VkDevice dev; VkPhysicalDeviceProperties props; } V = {.props.limits.minTexelBufferOffsetAlignment = 4};
static vkpool_t pools[] = {{11}, {22}};
static const uint64_t base[] = {0x100000, 0x200000};
enum { POOL_BYTES = 65536, MAX_VIEWS = 64 };
static unsigned created, deferred, fail_next;
static uint64_t retire_serial, fence, queued[MAX_VIEWS];
static vkpool_t *vk_pool_of(uint64_t mem, uint64_t len) {
    for (int i = 0; i < 2; i++) if (pools[i].serial && mem >= base[i] && mem + len <= base[i] + POOL_BYTES) return &pools[i];
    return NULL;
}
static int vk_buf_at(uint64_t mem, uint64_t len, VkBuffer *b, VkDeviceSize *off) {
    vkpool_t *p = vk_pool_of(mem, len); if (!p) return 0;
    *b = (VkBuffer)(uintptr_t)p->serial; *off = mem - base[p - pools]; return 1;
}
static const vkfmt_t *vk_fmt(uint32_t fmt) {
    static const vkfmt_t formats[] = {{VK_FORMAT_R32_UINT, 4}, {VK_FORMAT_R32_SFLOAT, 4}};
    return fmt < 2 ? &formats[fmt] : NULL;
}
static VkResult create_view(VkDevice dev, const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *alloc, VkBufferView *out) {
    (void)dev; (void)alloc; assert(ci->buffer && ci->range && ci->offset % 4 == 0);
    if (fail_next) { fail_next = 0; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    assert(created + 1 < MAX_VIEWS); *out = (VkBufferView)(uintptr_t)++created; return VK_SUCCESS;
}
#define vkCreateBufferView create_view
#define VKRED_ONCE(...) ((void)0)
''' + defer_decl + '\n' + helper + r'''
void vk_defer_free_buffer_view(VkBufferView view) {
    uintptr_t id = (uintptr_t)view;
    assert(id > 0 && id <= created && !queued[id]);
    // Deferring may wait on the GPU. All retired keys must already be gone and the mutex released.
    assert(pthread_mutex_trylock(&g_tbuf_mu) == 0);
    const tbuf_key *keys = (const tbuf_key *)g_tbuf_cache.keys;
    for (uint32_t i = 0; i < g_tbuf_cache.cap; i++) if (g_tbuf_cache.vals[i]) assert(keys[i].pool != retire_serial);
    pthread_mutex_unlock(&g_tbuf_mu);
    queued[id] = fence; deferred++;
}
static void drop(uint64_t serial, uint64_t at_fence) { retire_serial = serial; fence = at_fence; vk_tbuf_pool_drop(serial); }
int main(void) {
    drop(11, 7); assert(deferred == 0);
    tex_t textures[12]; VkBufferView views[12]; uint64_t off[2] = {0, 0};
    // Every key starts at the final slot of the initial table. The interleaved pool chain wraps to slot zero.
    for (int i = 0; i < 12; i++) {
        int p = i % 2; tbuf_key k = {0, pools[p].serial, 0, 1};
        do { off[p] += 4; k.mem = base[p] + off[p]; } while ((nvn_hm_hash(&k, sizeof k) & 63) != 63);
        assert(off[p] + 4 <= POOL_BYTES);
        textures[i] = (tex_t){(const void *)(uintptr_t)k.mem, {0, 1}};
        views[i] = vk_tbuf_view(&textures[i]); assert(views[i] && vk_tbuf_view(&textures[i]) == views[i]);
    }
    assert(created == 12 && g_tbuf_cache.n == 12 && g_tbuf_cache.cap == 64);
    assert(g_tbuf_cache.vals[63] && g_tbuf_cache.vals[0]);

    pools[0].serial = 0; drop(11, 7);
    assert(deferred == 6 && g_tbuf_cache.n == 6);
    for (int i = 0; i < 12; i++) {
        assert(queued[(uintptr_t)views[i]] == (i % 2 ? 0 : 7));
        if (i % 2) assert(vk_tbuf_view(&textures[i]) == views[i]);
    }
    assert(!vk_tbuf_view(&textures[0]) && created == 12);
    drop(11, 8); assert(deferred == 6);

    // Reusing the guest address is a different pool; format and width are still independent key fields.
    pools[0].serial = 33; VkBufferView reused = vk_tbuf_view(&textures[0]);
    assert(reused && reused != views[0] && vk_tbuf_view(&textures[0]) == reused);
    tex_t alias = textures[1]; alias.d.format = 1;
    VkBufferView format = vk_tbuf_view(&alias); assert(format && format != views[1]);
    alias.d.format = 0; alias.d.width = 2;
    VkBufferView width = vk_tbuf_view(&alias); assert(width && width != views[1] && width != format);
    alias.d.width = 3; fail_next = 1; assert(!vk_tbuf_view(&alias));
    VkBufferView retried = vk_tbuf_view(&alias); assert(retried && retried != width);
    assert(created == 16 && g_tbuf_cache.n == 10);

    pools[1].serial = 0; drop(22, 9);
    assert(deferred == 15 && g_tbuf_cache.n == 1 && vk_tbuf_view(&textures[0]) == reused);
    for (int i = 1; i < 12; i += 2) assert(queued[(uintptr_t)views[i]] == 9);
    assert(queued[(uintptr_t)format] == 9 && queued[(uintptr_t)width] == 9 && queued[(uintptr_t)retried] == 9);
    pools[0].serial = 0; drop(33, 10); drop(22, 11); drop(33, 11);
    assert(deferred == created && g_tbuf_cache.n == 0 && queued[(uintptr_t)reused] == 10);
    free(g_tbuf_cache.keys); free(g_tbuf_cache.vals);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_tbuf_cache_') as tmp:
            cfile, exe = Path(tmp) / 'tbuf.c', Path(tmp) / 'check'
            cfile.write_text(code, encoding='utf-8')
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-idirafter', str(headers), '-I' + str(DM / 'gfx/nvn'),
                            str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
