"""nxnative's t_tex on the host: nvn_tex.c's texture layout and nvn_deswizzle.c's block-linear copies (sizes, round trips, hand-computed addresses). Plain C."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

import host_cc

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'nxnative/dm/gfx'


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class TTex(unittest.TestCase):
    def test_t_tex(self):
        with tempfile.TemporaryDirectory(prefix='nx2a-t-tex-') as tmp:
            exe = Path(tmp) / ('t_tex' + ('.exe' if os.name == 'nt' else ''))
            sanitize = ['-fsanitize=undefined', '-fno-sanitize-recover=all'] if host_cc.ubsan() else []
            subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', *sanitize, str(GFX / 'tex/tests/t_tex.c'),
                            str(GFX / 'tex/nvn_deswizzle.c'), str(GFX / 'nvn/nvn_tex.c'), '-lm', '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=600)
        self.assertEqual(r.returncode, 0, r.stdout[-4000:] + r.stderr[-2000:])
        self.assertIn('all passed', r.stdout)


if __name__ == '__main__':
    unittest.main()
