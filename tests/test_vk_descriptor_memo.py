"""Check descriptor memo results against fresh resolution through state changes."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk


class DescriptorMemo(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_binding_invalidation(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        source = (DM / 'android/gfx_vk_exec.c').read_text()
        decl = source[source.index('enum { MEMO_CB'):source.index('static int set_memo_ok(')]
        start = source.index('static int set_memo_get(')
        lookup = source[start:source.index('\n}\n', start) + 3]
        bind = next(line.strip() for line in source.splitlines() if line.strip().startswith('case OP_BIND_UBO:'))
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
typedef struct { int reg; } dmsv_bind;
typedef struct { dmsv_bind b[4]; int ndyn; } vkshader_t;
typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;
static struct { p_bind_buf ubo[6][16]; } S;
enum { AB_UBO_EACH = 64, OP_BIND_UBO };
static int g_vk_ab;
static uint64_t g_set_epoch = 1, g_tex_gen = 1, g_samp_gen = 1, g_num_um, g_num_im;
uint64_t g_ubd_gen = 1;
static uint32_t g_draw_binst, pool_gen = 2;
static unsigned resolved, shadow_slots;
uint32_t vk_pool_gen(void) { return pool_gen; }
static void cbuf_desc(int st, const vkshader_t *s, const dmsv_bind *b, VkDescriptorBufferInfo *d) {
    (void)s; resolved++;
    const p_bind_buf *u = &S.ubo[st][b->reg];
    d->buffer = (VkBuffer)(uintptr_t)(1 + (u->addr >> 20) + pool_gen * 64);
    d->offset = u->addr & 0xfffff; d->range = u->size;
    if (shadow_slots & (1u << b->reg)) { d->buffer = (VkBuffer)(uintptr_t)(10000 + g_ubd_gen * 16 + b->reg); d->offset = 0; }
}
''' + decl + lookup + r'''
static void bind_ubo(int st, int slot, uint64_t addr, uint64_t size) {
    p_bind_buf b = {st, slot, addr, size}; const void *a = &b;
    switch (OP_BIND_UBO) {
''' + bind + r'''
    }
}
static void prime(int st, vkshader_t *s) {
    set_memo_t *m = &g_smemo[st]; memset(m, 0, sizeof *m);
    m->s = s; m->epoch = g_set_epoch; m->bind = g_bind_gen[st]; m->ubo = g_ubo_gen[st]; m->ubd = g_ubd_gen;
    m->tex = g_tex_gen; m->samp = g_samp_gen; m->pool = pool_gen; m->binst = g_draw_binst; m->ncb = s->ndyn;
    for (int c = 0; c < m->ncb; c++) {
        m->ci[c] = m->cpos[c] = c; m->cbgen[c] = g_ubo_slot_gen[st][s->b[c].reg];
        cbuf_desc(st, s, &s->b[c], &m->cd[c]); m->dofs[c] = m->cd[c].offset;
    }
}
static int check(int st, vkshader_t *s) {
    uint32_t dofs[96]; int hit = set_memo_get(st, s, dofs);
    if (hit) for (int c = 0; c < s->ndyn; c++) {
        VkDescriptorBufferInfo d; cbuf_desc(st, s, &s->b[c], &d);
        assert(d.buffer == g_smemo[st].cd[c].buffer && d.range == g_smemo[st].cd[c].range && d.offset == dofs[c]);
    }
    else prime(st, s);
    return hit;
}
static uint32_t rng = 1;
static uint32_t next(void) { rng = rng * 1664525u + 1013904223u; return rng; }
int main(void) {
    vkshader_t shader = {{{0}, {1}, {2}, {3}}, 4};
    for (int st = 0; st < 3; st++) {
        for (int slot = 0; slot < 4; slot++) bind_ubo(st, slot, 0x100000 + 256 * slot, 256);
        prime(st, &shader);
    }
    uint32_t dofs[96];
    bind_ubo(0, 1, 0x102000, 256); resolved = 0;
    assert(set_memo_get(0, &shader, dofs) && resolved == 1 && dofs[1] == 0x2000);
    g_vk_ab = AB_UBO_EACH; bind_ubo(0, 2, 0x103000, 256); resolved = 0;
    assert(set_memo_get(0, &shader, dofs) && resolved == 4 && dofs[2] == 0x3000);
    g_vk_ab = 0; g_ubd_gen++; resolved = 0;
    assert(set_memo_get(0, &shader, dofs) && resolved == 4);
    shadow_slots = 1; g_ubd_gen++;
    assert(!check(0, &shader));
    shadow_slots = 0; g_ubd_gen++; assert(!check(0, &shader));
    for (unsigned i = 0; i < 10000; i++) {
        int st = next() % 3, slot = next() % 4; uint32_t action = next() % 11;
        p_bind_buf old = S.ubo[st][slot];
        switch (action) {
        case 0: bind_ubo(st, slot, old.addr, old.size); break;
        case 1: bind_ubo(st, slot, 0x100000 + (next() % 1024) * 256, old.size); break;
        case 2: bind_ubo(st, slot, old.addr ^ 0x200000, old.size); break;
        case 3: bind_ubo(st, slot, old.addr, 256 * (1 + next() % 4)); break;
        case 4: shadow_slots ^= 1u << slot; g_ubd_gen++; break;
        case 5: pool_gen += 2; break;
        case 6: g_set_epoch++; break;
        case 7: g_bind_gen[st]++; break;
        case 8: g_tex_gen++; break;
        case 9: g_draw_binst++; break;
        case 10: g_vk_ab ^= AB_UBO_EACH; break;
        }
        check(st, &shader);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-memo-test-') as temp:
            cfile, exe = Path(temp) / 'memo.c', Path(temp) / 'memo-test'
            cfile.write_text(code)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-idirafter', str(headers), str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
