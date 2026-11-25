"""NVN texture layout (nvn_tex.c) and the CPU block-linear copies (nvn_deswizzle.c) through nxnative's own C tests.

t_tex_layout checks storage sizes, level offsets and block heights against shapes the original NVN driver gives; t_sparse_layout sparse
textures' sizes against the games' pre-swizzled data; t_tex the layout's sizes and the deswizzle (round trips, hand-computed addresses). Plain C.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

import host_cc

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'nxnative/dm/gfx'
TEX_SOURCES = [GFX / 'tex/nvn_deswizzle.c', GFX / 'nvn/nvn_tex.c']


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class NvnTexLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a-nvn-tex-layout-')
        cls.addClassCleanup(cls.tmp.cleanup)

    def run_c(self, name, main, *flags):
        exe = Path(self.tmp.name) / (name + ('.exe' if os.name == 'nt' else ''))
        sanitize = ['-fsanitize=undefined', '-fno-sanitize-recover=all'] if host_cc.ubsan() else []
        subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', *sanitize, *flags, str(main), *map(str, TEX_SOURCES), '-lm', '-o', str(exe)],
                       check=True, capture_output=True, text=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=600)
        self.assertEqual(r.returncode, 0, r.stdout[-4000:] + r.stderr[-2000:])
        return r.stdout

    def test_driver_shapes(self):
        out = self.run_c('t_tex_layout', GFX / 'tests/t_tex_layout.c')
        self.assertNotIn('FAIL', out)

    def test_sparse_layout(self):
        out = self.run_c('t_sparse_layout', GFX / 'tests/t_sparse_layout.c')
        self.assertIn(' 0 failed', out)

    def test_packaged_layout_setter(self):
        # nvnTextureBuilderSetPackagedTextureLayout as hle_nvn.c has it: byte 0 and byte 1's low nibble reach the description, and the width
        # alignment pads Bayonetta Origins' 2052x1026 ASTC 6x6 tutorial-card background to 96 GOBs (0x120000 bytes) where 86 (0x102000) left magenta
        hle = (GFX / 'nvn/hle_nvn.c').read_text(encoding='utf-8')
        start = hle.index('NVN(nvnTextureBuilderSetPackagedTextureLayout,')
        setter = hle[start:hle.index('\n}\n', start) + 3]
        main = Path(self.tmp.name) / 'pkg_setter.c'
        main.write_text(r'''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nvn_tex.h"
typedef struct { unsigned char layout[8]; } NVNpackagedTextureLayout;
typedef struct NVNtextureBuilder NVNtextureBuilder;
typedef struct { nvn_texdesc d; uint8_t has_layout, layout[8]; } tex_builder_t;
#define TB(b) ((tex_builder_t *)(b))
#define NVN(name, ret, ...) static ret name(__VA_ARGS__)
''' + setter + r'''
int main(void) {
    tex_builder_t b = {{1, 0x7d, 4, 0, 2052, 1026, 1, 1, 0, 0}, 0, {0}};
    NVNpackagedTextureLayout l = {{3, 4, 0, 0, 0, 0, 0, 0}};
    nvnTextureBuilderSetPackagedTextureLayout((NVNtextureBuilder *)&b, &l);
    nvn_texlayout L; nvn_tex_layout(&b.d, &L);
    assert(b.d.pkg_layout == 0x404 && L.size == 0x120000 && L.tileH == 3);
    l.layout[1] = 0xf4; nvnTextureBuilderSetPackagedTextureLayout((NVNtextureBuilder *)&b, &l);
    assert(b.d.pkg_layout == 0x404);   // only the low nibble
    l.layout[1] = 0; nvnTextureBuilderSetPackagedTextureLayout((NVNtextureBuilder *)&b, &l);
    nvn_tex_layout(&b.d, &L); assert(L.size == 0x102000);
    nvnTextureBuilderSetPackagedTextureLayout((NVNtextureBuilder *)&b, NULL);
    assert(b.d.pkg_layout == 0 && !b.has_layout);
    puts("packaged layout setter ok"); return 0;
}
''', encoding='utf-8')
        out = self.run_c('pkg_setter', main, '-I', str(GFX / 'nvn'))
        self.assertIn('setter ok', out)

    def test_deswizzle(self):
        out = self.run_c('t_tex', GFX / 'tex/tests/t_tex.c')
        self.assertIn('all passed', out)


if __name__ == '__main__':
    unittest.main()
