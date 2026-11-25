"""NVN texture layout (nvn_tex.c) and the CPU block-linear copies (nvn_deswizzle.c) through nxnative's own C tests.

t_tex_layout checks storage sizes, level offsets and block heights against shapes the original NVN driver gives; t_sparse_layout sparse
textures' sizes against the games' pre-swizzled data. Plain C.
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

    def run_c(self, name, main):
        exe = Path(self.tmp.name) / (name + ('.exe' if os.name == 'nt' else ''))
        sanitize = ['-fsanitize=undefined', '-fno-sanitize-recover=all'] if host_cc.ubsan() else []
        subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', *sanitize, str(main), *map(str, TEX_SOURCES), '-lm', '-o', str(exe)],
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


if __name__ == '__main__':
    unittest.main()
