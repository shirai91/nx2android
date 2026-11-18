"""Gust chunked-zlib shader archives; synthetic payloads, no game assets."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'


def chunked(data, size=0x4000):
    blocks = [zlib.compress(data[o:o + size]) for o in range(0, len(data), size)]
    return b''.join(struct.pack('<I', len(b)) + b for b in blocks) + bytes(4)


@unittest.skipUnless(importlib.util.find_spec('lz4'), 'no lz4')
class GustArchives(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(TOOLS))
        spec = importlib.util.spec_from_file_location('split_gust_test', TOOLS / 'shader_split.py')
        cls.s = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}): spec.loader.exec_module(cls.s)
        ctl = bytearray(0x880); ctl[:4] = cls.s.CTL_MAGIC
        code = cls.s.CS_MAGIC + bytes(0xfc)
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        struct.pack_into('<I', ctl, 0x704, len(code))
        h = bytearray(0x50); h[:4] = bytes.fromhex('91688619')
        struct.pack_into('<I', h, 0x44, len(h) + len(ctl) + len(code))
        cls.ctl, cls.code = bytes(ctl), code
        cls.body = b'CRAE' + bytes(0x3fe0) + h + ctl + code  # shader crosses a chunk boundary

    def test_multiple_chunks_join(self):
        data = bytes(range(256)) * 200
        self.assertEqual(self.s.gust_gz(chunked(data)), data)

    def test_other_or_incomplete_forms_are_refused(self):
        data = b'CRAE' + bytes(60); z = zlib.compress(data)
        cases = [data, b'\x1f\x8b\x08\0' + bytes(60), b'', bytes(4),
                 chunked(data)[:-4], chunked(data) + b'more', chunked(data)[:-6],
                 struct.pack('<I', len(z) + 4) + z + b'junk' + bytes(4),
                 struct.pack('<I', len(z) - 1) + z[:-1] + bytes(4),
                 struct.pack('<I', len(z)) + z[:-1] + bytes([z[-1] ^ 1]) + bytes(4)]
        for d in cases:
            with self.subTest(data=d): self.assertIsNone(self.s.gust_gz(d))

    def scan(self, data, suffix='.elixir.gz'):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / ('pe_pack' + suffix); p.write_bytes(data)
            return self.s.scan_file((str(p), 'PostEffect/' + p.name))

    def test_scan_recovers_shader_bytes_and_key(self):
        for data in (self.body, chunked(self.body)):
            with self.subTest(compressed=data != self.body):
                rel, shaders, pairs, trees = self.scan(data)
                self.assertEqual(rel, 'PostEffect/pe_pack.elixir.gz')
                self.assertEqual(list(shaders), [self.s.skey(self.ctl, self.code)])
                [(where, stage, ctl, code)] = shaders.values()
                self.assertEqual((stage, ctl, code), ('cs', self.ctl, self.code))
                self.assertTrue(where.startswith('/' + rel + '#GLSLC@'))
                self.assertEqual((pairs, trees), (set(), {}))

    def test_probe_is_limited_to_gz_files(self):
        self.assertEqual(self.scan(chunked(self.body), '.bin')[1], {})


if __name__ == '__main__': unittest.main()
