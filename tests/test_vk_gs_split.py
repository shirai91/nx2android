"""Compile the production geometry split against synthetic side-effect data flows."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which(os.environ.get('CC', 'cc')) or shutil.which('clang') or shutil.which('gcc')


@unittest.skipUnless(CC, 'geometry split harness needs a host C compiler; also runnable with the Android NDK')
class GeometrySplit(unittest.TestCase):
    def test_geometry_effects_are_once_only_and_rasterization_is_independent(self):
        with tempfile.TemporaryDirectory(prefix='nx2a_gs_split_') as tmp:
            exe = Path(tmp) / ('gs_split.exe' if os.name == 'nt' else 'gs_split')
            subprocess.run([CC, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/vk_gs_split_harness.c'),
                            str(ROOT / 'nxnative/dm/android/gfx_vk_gs_split.c'), '-o', str(exe)],
                           check=True, capture_output=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('43 cases passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
