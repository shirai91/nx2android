"""Exercise the production ClearTexture dispatcher against a pixel-array oracle.

The optional Vulkan probe uses the same source with a real driver; this test
keeps the range/mask regression available on hosts without Vulkan or an NDK.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def clear_source():
    source = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text()
    start = source.index('static VkClearColorValue clear_tex_value(')
    return source[start:source.index('// ---------------- the record walker', start)]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class TextureClear(unittest.TestCase):
    def test_failed_pipeline_is_cached(self):
        source = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text()
        start = source.index('typedef struct { uint32_t ncol, cfmt[8], dfmt, samples, mask, type; } mckey;')
        pipeline = source[start:source.index('static VkPipeline mc_pipeline(uint32_t mask)', start)]
        fixture = (ROOT / 'tests/vk_clear_pipeline_fixture.c').read_text()
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / 'pipeline.c'
            src.write_text(fixture.replace('/* PRODUCTION_PIPELINE */', pipeline))
            exe = Path(folder) / 'pipeline'
            build = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-missing-field-initializers',
                                    '-I', str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'),
                                    '-I', str(ROOT / 'nxnative/dm/gfx/nvn'),
                                    str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_regions_masks_and_subresources(self):
        fixture = (ROOT / 'tests/vk_texture_clear_fixture.c').read_text()
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / 'clear.c'
            src.write_text(fixture.replace('/* PRODUCTION_CLEAR */', clear_source()))
            exe = Path(folder) / 'clear'
            build = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-function', '-Wno-missing-field-initializers',
                                    '-I', str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'),
                                    '-I', str(ROOT / 'nxnative/dm/android'),
                                    str(src), '-lm', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            for case in ('', 'msaa_color', 'msaa_depth', 'partial_depth', 'decoded',
                         'nonattachment', '1d', '1d_array', 'format', 'view', 'pipeline',
                         'compressed', 'compressed_full', 'compressed_volume', 'compressed_tail',
                         'compressed_chunks', 'compressed_mask0', 'compressed_msaa', 'compressed_bad_usage'):
                with self.subTest(case=case):
                    run = subprocess.run([str(exe)] + ([case] if case else []), capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
