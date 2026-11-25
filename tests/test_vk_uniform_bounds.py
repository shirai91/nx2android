"""Reads past a uniform binding's size give 0 (gfx_vk_exec_descriptors.inc cbuf_desc, ub_padded, cbsize_desc), cut from the source and run on
the host's C compiler: the descriptor's range is the bound size rounded up to 256 bytes where the pool reaches that far, a binding shorter than its
shader's reads at fixed offsets (the binding's off) is bound from a copy with the memory's bytes up to the rounded size and zeros after, the size table
holds each slot's rounded size, and DM_VK_UB_BOUNDS=0 (g_ub_bounds) gives the old ranges and a table of 64 KiB. The exporter (shader_vk.py classify)
gives c25 its binding kind and a slot its fixed bytes."""
import ast
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/tools'

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t VkDeviceSize; typedef struct VkBuffer_T *VkBuffer;
typedef struct { VkBuffer buffer; VkDeviceSize offset, range; } VkDescriptorBufferInfo;
typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;
typedef struct __attribute__((packed)) { uint8_t kind, desc, dim, flags; uint16_t binding, count; int16_t reg; uint8_t samp, fmt; int32_t off, samp_arg; } dmsv_bind;
typedef struct { uint64_t key; } vkshader_t;
typedef struct { uint64_t lo, hi; VkBuffer buf; uint64_t end, boff; int has_inner; } vkpool_t;
enum { RK_SUPPORT, RK_CBUF, RK_CBSIZE = 17 };
static struct { p_bind_buf ubo[6][16]; } S;
static struct { struct { struct { VkDeviceSize minUniformBufferOffsetAlignment; } limits; } props; } V = {{{64}}};
static int g_ub_bounds = 1, g_var_now, g_ubolog_n, g_sub_pass; static uint64_t g_ubolog_key, g_tex_frame, g_set_epoch = 1, g_ubd_gen = 1, g_cbpath[3];
static char last[512];
#define VKLOG(...) snprintf(last, sizeof last, __VA_ARGS__)
#define VKRED_ONCE(id, ...) VKLOG(__VA_ARGS__)
static VkBuffer const POOL = (VkBuffer)0x1000, SCRATCH = (VkBuffer)0x2000, ZERO = (VkBuffer)0x3000;
static vkpool_t pool; static uint8_t mem[0x4000], scratch[0x10000]; static VkDeviceSize used;
static uint32_t vk_pool_gen(void) { return 1; }
static vkpool_t *vk_pool_of(uint64_t a, uint64_t n) { (void)n; return a >= pool.lo && a < pool.hi ? &pool : NULL; }
static VkBuffer vk_zero_buffer(void) { return ZERO; }
static void *vk_scratch(VkDeviceSize n, VkDeviceSize al, VkBuffer *buf, VkDeviceSize *off) { used = (used + al - 1) & ~(al - 1); *buf = SCRATCH; *off = used; used += n; return scratch + *off; }
static void vk_ubd_read(uint64_t a, uint64_t n, uint8_t *dst) { memcpy(dst, (const void *)(uintptr_t)a, n); }
static int vk_ubd_shadow(uint64_t a, uint64_t n, VkBuffer *buf, VkDeviceSize *off) { (void)a; (void)n; (void)buf; (void)off; return 0; }
static void vk_cross(int k, uint64_t a, uint64_t n, const vkpool_t *p) { (void)k; (void)a; (void)n; (void)p; }
static void ubo_log(const vkshader_t *s, int st, int reg, uint64_t a, uint64_t n) { (void)s; (void)st; (void)reg; (void)a; (void)n; }
@CUT@
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static VkDescriptorBufferInfo desc(int reg, int32_t fixed) { dmsv_bind b = {0}; b.kind = RK_CBUF; b.reg = (int16_t)reg; b.off = fixed; vkshader_t s = {0x1234};
    VkDescriptorBufferInfo d; cbuf_desc(0, &s, &b, &d); return d; }
static void bind(int reg, uint64_t at, uint64_t size) { S.ubo[0][reg] = (p_bind_buf){0, reg, (uint64_t)(uintptr_t)mem + at, size}; }
int main(void) {
    for (int i = 0; i < (int)sizeof mem; i++) mem[i] = (uint8_t)(i * 7 + 1);
    pool = (vkpool_t){(uint64_t)(uintptr_t)mem, (uint64_t)(uintptr_t)mem + sizeof mem, POOL, (uint64_t)(uintptr_t)mem + sizeof mem, 0, 0};
    // the range is the size NVN's driver gives the GPU: rounded up to 256 bytes
    bind(0, 0x100, 16); VkDescriptorBufferInfo d = desc(0, -1);
    CHECK(d.buffer == POOL && d.offset == 0x100 && d.range == 256, "16 bytes: %p +%llu range %llu", (void *)d.buffer, (unsigned long long)d.offset, (unsigned long long)d.range);
    d = desc(0, 0x100); CHECK(d.buffer == POOL && d.range == 256, "fixed reads inside the rounded size need no copy (range %llu)", (unsigned long long)d.range);
    bind(1, 0x200, 300); d = desc(1, 0); CHECK(d.range == 512, "300 bytes: range %llu", (unsigned long long)d.range);
    // Paper Mario's case: 16 bytes bound, read at fixed offsets up to 0x930: a copy, the memory's 256 bytes, then zeros
    bind(2, 0x400, 16); d = desc(2, 0x930);
    CHECK(d.buffer == SCRATCH && d.range == 0x930 && d.offset % 64 == 0, "padded: %p range %llu", (void *)d.buffer, (unsigned long long)d.range);
    const uint8_t *c = scratch + d.offset; int same = !memcmp(c, mem + 0x400, 256), zero = 1; for (int i = 256; i < 0x930; i++) zero &= !c[i];
    CHECK(same && zero, "padded copy: memory up to 256 %d, zeros after %d", same, zero);
    VkDeviceSize at = d.offset; d = desc(2, 0x930); CHECK(d.offset == at, "the same binding again is the same copy");
    // the pool ends 128 bytes after the binding: the range stops there, and a padded copy holds those 128 bytes
    bind(3, sizeof mem - 128, 16); d = desc(3, 0); CHECK(d.buffer == POOL && d.range == 128, "near the pool's end: range %llu", (unsigned long long)d.range);
    d = desc(3, 0x200); c = scratch + d.offset; zero = 1; for (int i = 128; i < 0x200; i++) zero &= !c[i];
    CHECK(d.buffer == SCRATCH && d.range == 0x200 && !memcmp(c, mem + sizeof mem - 128, 128) && zero, "near the pool's end, padded: range %llu", (unsigned long long)d.range);
    // a size of 0 and one past 64 KiB keep their 64 KiB, as before
    bind(4, 0x800, 0); d = desc(4, 0x40); CHECK(d.range == 65536 || d.range == sizeof mem - 0x800, "size 0: range %llu", (unsigned long long)d.range);
    // the size table: the rounded sizes, 0 for a slot with no buffer, 64 KiB for 0 bytes and past 64 KiB
    bind(5, 0x900, 70000); dmsv_bind t = {0}; t.kind = RK_CBSIZE; t.reg = -1; vkshader_t s = {1}; cbuf_desc(0, &s, &t, &d);
    const uint32_t *w = (const uint32_t *)(const void *)(scratch + d.offset);
    CHECK(d.buffer == SCRATCH && d.range == 64 && w[0] == 256 && w[1] == 512 && w[2] == 256 && w[4] == 65536 && w[5] == 65536 && w[6] == 0 && w[13] == 0,
          "size table %u %u %u %u %u %u range %llu", w[0], w[1], w[2], w[4], w[5], w[6], (unsigned long long)d.range);
    at = d.offset; cbuf_desc(0, &s, &t, &d); CHECK(d.offset == at, "an unchanged table is the same copy");
    bind(6, 0xa00, 32); cbuf_desc(0, &s, &t, &d); w = (const uint32_t *)(const void *)(scratch + d.offset); CHECK(d.offset != at && w[6] == 256, "a new binding makes a new table (%u)", w[6]);
    // DM_VK_UB_BOUNDS=0: the exact size, no copy, a table of 64 KiB
    g_ub_bounds = 0; g_set_epoch++;
    d = desc(0, -1); CHECK(d.buffer == POOL && d.range == 16, "off: range %llu", (unsigned long long)d.range);
    d = desc(2, 0x930); CHECK(d.buffer == POOL && d.range == 16, "off: no padded copy (range %llu)", (unsigned long long)d.range);
    cbuf_desc(0, &s, &t, &d); w = (const uint32_t *)(const void *)(scratch + d.offset); CHECK(w[0] == 65536 && w[2] == 65536 && w[13] == 0, "off: table %u %u", w[0], w[2]);
    if (!fails) puts("ok");
    return fails != 0;
}
'''


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class UniformBounds(unittest.TestCase):
    def test_ranges_padded_copies_and_size_table(self):
        src = csrc.read('nxnative/dm/android/gfx_vk_exec_descriptors.inc')
        cut = csrc.between(src, 'static inline uint64_t ub_bound(uint64_t size)', '// The set a stage\'s last draw got', 'cbuf_desc')
        with tempfile.TemporaryDirectory(prefix='nx2a_ub_bounds_') as d:
            c = Path(d) / 't.c'; c.write_text(HARNESS.replace('@CUT@', cut), encoding='utf-8')
            exe = Path(d) / ('t.exe' if shutil.which('cc').lower().endswith('.exe') else 't')
            subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-o', str(exe), str(c)], check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)



def exporter():
    source = TOOLS / 'shader_vk.py'
    tree = ast.parse(source.read_text(encoding='utf-8'))
    names = {'TEX_BASE', 'SSBO', 'CBSIZE_BANK', 'KIND', 'DESC', 'SAMP_NONE'}
    nodes = [n for n in tree.body if
             isinstance(n, ast.FunctionDef) and n.name in ('classify', 'refl_bin') or
             isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in names or
                 isinstance(t, ast.Tuple) and any(isinstance(v, ast.Name) and v.id in names for v in t.elts)
                 for t in n.targets)]
    env = {'re': re, 'struct': struct, 'GAME': {}}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), env)
    return env


class ExporterUniformBounds(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.export = exporter()

    def classify(self, n, cbufs):
        return self.export['classify']('ps', {'name': f'fp_c{n}', 'type': 'ubo', 'binding': 4, 'count': 1}, {'cbufs': cbufs})

    def test_size_table_and_fixed_bytes(self):
        self.assertEqual(self.classify(25, [])['kind'], 'cbsize')
        self.assertEqual(self.export['KIND']['cbsize'], 17)   # gfx_vk.h RK_CBSIZE
        e = self.classify(6, [{'binding': 4, 'slot': 6, 'fixed': 0x930}])
        self.assertEqual((e['kind'], e['reg'], e['off']), ('cbuf', 3, 0x930))
        self.assertIsNone(self.classify(6, [{'binding': 4, 'slot': 6, 'fixed': 0}]).get('off'))
        self.assertEqual(self.classify(6, [{'binding': 4, 'slot': 6, 'fixed': 1 << 29}])['off'], 0x10000)   # at most the 64 KiB a slot addresses
        self.assertEqual(self.classify(2, [])['kind'], 'drvcb')

    def test_fixed_bytes_reach_the_binary_binding(self):
        e = self.classify(6, [{'binding': 4, 'slot': 6, 'fixed': 0x930}]); e.update(type='ubo')
        blob = self.export['refl_bin']([e], b'', [1, 1, 1])
        kind, desc, dim, flags, binding, count, reg, samp, fmt, off, sarg = struct.unpack_from('<BBBBHHhBBii', blob, 20)
        self.assertEqual((kind, reg, off), (1, 3, 0x930))


if __name__ == '__main__':
    unittest.main()
