"""Koei Tecmo's chunked zlib (shader_split.py kt_gz): the 64 KB form stays the default, and the 128 KB form is read only when asked for."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch
import zlib


TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'


def chunked(data, chunk):
    """data in Koei Tecmo's chunked form: u32 chunk size, u32 count, u32 inflated length, u32 stored length per chunk, then each chunk at 0x80 alignment
    (u32 length + one zlib stream, or the plain bytes when stored whole)"""
    parts = [data[i:i + chunk] for i in range(0, len(data), chunk)]
    blobs = []
    for p in parts:
        z = zlib.compress(p)
        blobs.append(struct.pack('<I', len(z)) + z if len(z) + 4 < len(p) else p)
    head = struct.pack(f'<III{len(parts)}I', chunk, len(parts), len(data), *map(len, blobs))
    out = bytearray(head)
    for b in blobs:
        out += bytes(-len(out) % 0x80) + b
    return bytes(out)


class KtGz(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(TOOLS))
        spec = importlib.util.spec_from_file_location('shader_split_ktgz', TOOLS / 'shader_split.py')
        cls.s = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}):
            spec.loader.exec_module(cls.s)

    def setUp(self):
        self.data = bytes(range(256)) * 700 + b'\0' * 50000   # 229,200 bytes: several chunks of either size, compressible and not

    def test_default_reads_64k_chunks(self):
        self.assertEqual(self.s.kt_gz(chunked(self.data, 0x10000)), self.data)

    def test_default_refuses_128k_chunks(self):   # every caller from before the parameter sees what it saw then
        self.assertIsNone(self.s.kt_gz(chunked(self.data, 0x20000)))

    def test_128k_chunks_when_asked(self):
        self.assertEqual(self.s.kt_gz(chunked(self.data, 0x20000), 0x20000), self.data)
        self.assertIsNone(self.s.kt_gz(chunked(self.data, 0x10000), 0x20000))

    def test_not_chunked(self):
        self.assertIsNone(self.s.kt_gz(b'\0\0\2\0' + bytes(60), 0x20000))


if __name__ == '__main__':
    unittest.main()
