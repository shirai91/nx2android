"""Geometry index-building needs the same NVN buffers in reflection and driver c0."""
import ast
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def classifier():
    path = ROOT / 'nxnative/dm/android/tools/shader_vk.py'
    tree = ast.parse(path.read_text(encoding='utf-8'))
    nodes = [n for n in tree.body if
             isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'SSBO' for t in n.targets)
             or isinstance(n, ast.FunctionDef) and n.name == 'classify']
    import re
    scope = {'re': re, 'GAME': {}}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(path), 'exec'), scope)
    return scope['classify']


def driver_source():
    text = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text(encoding='utf-8')
    start = text.index('static void drv_segment(')
    function = text[start:text.index('\n}', start) + 2]
    return r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
static struct {
    struct { uint64_t addr, size; } ssbo[6][16], ubo[6][16];
    uint64_t img[6][8], tex[6][256], samp[6][128];
} S;
static uint32_t g_dispatch[3], g_draw_binst;
static uint64_t c2_handle(int st, int off) { (void)st; (void)off; return 0; }
''' + function + r'''
int main(void) {
    uint8_t d[0x1000]; uint64_t addr; uint32_t size;
    S.ssbo[2][0].addr = 0x123456789ab0ull; S.ssbo[2][0].size = 60000;
    S.ssbo[2][2].addr = 0x123456799ac0ull; S.ssbo[2][2].size = 4;
    drv_segment(2, 0, d, 0);
    memcpy(&addr, d + 0x410, 8); memcpy(&size, d + 0x418, 4);
    assert(addr == S.ssbo[2][0].addr && size == 60000);
    memcpy(&addr, d + 0x430, 8); memcpy(&size, d + 0x438, 4);
    assert(addr == S.ssbo[2][2].addr && size == 4);
    // Address tables for other stages must not leak into this stage's c0.
    memcpy(&addr, d + 0x110, 8); assert(addr == 0);
    return 0;
}
'''


class GeometryStorage(unittest.TestCase):
    def test_geometry_reflection_maps_index_builder_buffers(self):
        classify = classifier()
        resources = {'sbufs': [
            {'binding': 1, 'cb_slot': 0, 'cb_offset': 0x410 // 4, 'flags': 1},
            {'binding': 2, 'cb_slot': 0, 'cb_offset': 0x430 // 4, 'flags': 1}]}
        for index, slot in ((0, 0), (1, 2)):
            binding = classify('gs', {'name': f'gs_s{index}', 'type': 'storage'}, resources)
            self.assertEqual((binding['kind'], binding['reg']), ('sbuf', slot))

    def test_storage_tables_remain_stage_specific(self):
        classify = classifier()
        for stage, offset, kind, slot in (('vs', 0x110, 'sbuf', 0),
                                          ('ps', 0x600, 'sbuf', 15),
                                          ('cs', 0x310, 'sbuf', 0),
                                          ('cs', 0x210, 'ubo_ptr', 0),
                                          ('gs', 0x500, 'sbuf', 15),
                                          ('gs', 0x418, 'sbuf', None),
                                          ('gs', 0x510, 'sbuf', None)):
            with self.subTest(stage=stage, offset=offset):
                resources = {'sbufs': [{'binding': 1, 'cb_slot': 0, 'cb_offset': offset // 4, 'flags': 1}]}
                binding = classify(stage, {'name': f'{stage}_s0', 'type': 'storage'}, resources)
                self.assertEqual((binding['kind'], binding['reg']), (kind, slot))

    @unittest.skipUnless(shutil.which('cc'), 'the driver-segment harness needs a C compiler')
    def test_geometry_driver_segment_contains_bound_addresses(self):
        with tempfile.TemporaryDirectory(prefix='nx2a_gs_') as tmp:
            src, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
            src.write_text(driver_source(), encoding='utf-8')
            subprocess.run(['cc', str(src), '-o', str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, capture_output=True)
