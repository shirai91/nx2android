"""Unreal's 16-byte contract must survive Android's eight-byte tiny size class."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class UnrealAllocator(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='nx2a-unreal-alloc-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = (ROOT / 'nxnative/dm/host/hle_uemalloc.c').read_text(encoding='utf-8')
        cut = source[source.index('static malloc_zone_t *g_zone;'):source.index('static void *g_vt[NSLOT];')]
        harness = (ROOT / 'tests/unreal_allocator.c').read_text(encoding='utf-8')
        c = Path(cls.temp.name) / 'check.c'
        cls.exe = Path(cls.temp.name) / 'check'
        c.write_text(harness.replace('PRODUCTION_ALLOCATOR', cut), encoding='utf-8')
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', str(c), '-o', str(cls.exe)], check=True)

    def check_contract(self, mode):
        run = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_small_allocations_keep_sixteen_byte_minimum(self):
        self.check_contract('alloc')

    def test_resize_preserves_alignment_and_contents(self):
        self.check_contract('realloc')

    def test_failed_try_allocation_preserves_live_storage(self):
        self.check_contract('failure')
