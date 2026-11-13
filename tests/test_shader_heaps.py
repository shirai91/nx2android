"""Heaps PAK/cache extraction through synthetic archive bytes and real GLSLC decoding."""
import base64
import importlib.util
import io
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'


def name(s, flags): return bytes([len(s)]) + s.encode() + bytes([flags])
def entry(s, pos, size, wide=False): return name(s, 2 if wide else 0) + struct.pack('<d' if wide else '<i', pos) + struct.pack('<iI', size, 0)
def folder(s, children): return name(s, 1) + struct.pack('<i', len(children)) + b''.join(children)
def pak(root, data, version=1):
    body = (b'0' * 64 if version else b'') + root + b'DATA'
    return b'PAK' + bytes([version]) + struct.pack('<ii', 12 + len(body), len(data)) + body + data


@unittest.skipUnless(importlib.util.find_spec('lz4'), 'no lz4')
class HeapsShaders(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(TOOLS))
        spec = importlib.util.spec_from_file_location('split_heaps_test', TOOLS / 'shader_split.py')
        cls.s = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}): spec.loader.exec_module(cls.s)
        ctl = bytearray(0x880); ctl[:4] = cls.s.CTL_MAGIC
        code = cls.s.CS_MAGIC + bytes(0xfc)
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        struct.pack_into('<I', ctl, 0x704, len(code))
        h = bytearray(0x50); h[:4] = bytes.fromhex('91688619')
        struct.pack_into('<I', h, 0x44, len(h) + len(ctl) + len(code))
        cls.ctl, cls.code = bytes(ctl), code
        cls.cache = b'#version 140\n//BIN=' + base64.b64encode(h + ctl + code) + b'#\n'

    def test_versions_and_nested_offsets(self):
        for version in (0, 1):
            d = pak(folder('', [entry('a.png', 0, 5), folder('sub', [entry('shaders.cache.nx', 5, 7, True)])]), b'firstsecond!', version)
            members = self.s.heaps_pak_files(io.BytesIO(d))
            self.assertEqual([(n, s) for n, _, s in members], [('/a.png', 5), ('/sub/shaders.cache.nx', 7)])
            off, size = members[1][1:]
            self.assertEqual(d[off:off + size], b'second!')

    def test_other_or_damaged_archives_are_refused(self):
        valid = pak(folder('', [entry('a', 0, 1)]), b'x')
        cases = [b'PAK', b'PAK\x09' + bytes(40), b'PACK' + bytes(40),
                 b'PAK\0' + struct.pack('<ii', 0x7fffffff, 0) + bytes(40),
                 valid.replace(b'DATA', b'NOPE'), valid[:-1], valid[:30],
                 pak(name('', 1) + struct.pack('<i', 100) + entry('a', 0, 1), b'x'),
                 pak(name('', 1) + struct.pack('<i', -1), b'x')]
        for d in cases:
            with self.subTest(data=d[:20]): self.assertIsNone(self.s.heaps_pak_files(io.BytesIO(d)))

    def test_invalid_member_ranges_are_refused(self):
        for pos, size, wide in [(-1, 1, False), (0, -1, False), (1, 1, False), (0, 2, False),
                                (0.5, 1, True), (float('nan'), 1, True), (float('inf'), 1, True), (1e100, 1, True)]:
            with self.subTest(pos=pos, size=size):
                d = pak(folder('', [entry('a', pos, size, wide)]), b'x')
                self.assertIsNone(self.s.heaps_pak_files(io.BytesIO(d)))

    def test_each_cache_comment_uses_the_real_decoder(self):
        found = self.s.heaps_shaders(self.cache + b'GLSL without a binary\n' + self.cache, '/shaders.cache.nx')
        self.assertEqual([x[1:] for x in found], [('cs', self.ctl, self.code)] * 2)
        self.assertNotEqual(found[0][0], found[1][0])

    def test_missing_or_invalid_binary_is_a_named_error(self):
        for d in (b'GLSL only', b'//BIN=AAAA#', b'//BIN=A#'):
            with self.subTest(data=d), self.assertRaisesRegex(SystemExit, 'shaders.cache.nx'):
                self.s.heaps_shaders(d, '/res.pak/shaders.cache.nx')

    def test_bad_comment_cannot_hide_behind_a_valid_program(self):
        for bad in (b'//BIN=AA!A#', b'//BIN=AAAA#', b'//BIN=#', b'//BIN=AAAA'):
            with self.subTest(data=bad), self.assertRaisesRegex(SystemExit, 'shaders.cache.nx'):
                self.s.heaps_shaders(self.cache + bad, '/res.pak/shaders.cache.nx')

    def test_scan_pairs_and_deduplicates_multistage_programs(self):
        h = bytearray(0x90 * 3); h[:4] = bytes.fromhex('91688619')
        struct.pack_into('<I', h, 0x4c, 2)
        payload, expected = bytearray(), []
        for i, stage in enumerate((1, 5)):
            code = bytearray(self.code); code[:4] = self.s.GFX_MAGIC
            struct.pack_into('<I', code, 0x30, (stage << 10) | 0x61)
            part = self.ctl + code
            at = 0x90 * (i + 1)
            struct.pack_into('<3I', h, at, len(part), len(h) + len(payload), 0)
            struct.pack_into('<4I', h, at + 0x30, 0, len(self.ctl), len(code), len(self.ctl))
            payload += part; expected.append(self.s.skey(self.ctl, code))
        struct.pack_into('<I', h, 0x44, len(h) + len(payload))
        cache = b'//BIN=' + base64.b64encode(h + payload) + b'#'
        archive = pak(folder('', [entry('shaders.cache.nx', 0, len(cache) * 2)]), cache * 2)
        with patch('builtins.open', side_effect=lambda *a, **k: io.BytesIO(archive)):
            _, shaders, pairs, _ = self.s.scan_file(('res.pak', 'res.pak'))
        self.assertEqual(list(shaders), expected)
        self.assertEqual(pairs, {tuple(expected)})

    def test_scan_reads_only_header_and_shader_cache(self):
        data = bytes(1 << 20) + self.cache
        root = folder('', [entry('unrelated.bin', 0, 1 << 20), folder('sub', [entry('shaders.cache.nx', 1 << 20, len(self.cache), True)])])
        archive = pak(root, data)
        reads = []
        class Recorded(io.BytesIO):
            def read(self, n=-1):
                reads.append(n)
                return super().read(n)
        with patch('builtins.open', side_effect=lambda *a, **k: Recorded(archive)):
            rel, shaders, pairs, trees = self.s.scan_file(('res.pak', 'res.pak'))
        self.assertEqual(rel, 'res.pak')
        self.assertEqual(list(shaders), [self.s.skey(self.ctl, self.code)])
        [(where, stage, ctl, code)] = shaders.values()
        self.assertTrue(where.startswith('/res.pak/sub/shaders.cache.nx@'))
        self.assertEqual((stage, ctl, code), ('cs', self.ctl, self.code))
        self.assertEqual((pairs, trees), (set(), {}))
        self.assertNotIn(-1, reads)
        self.assertLess(sum(reads), len(self.cache) + 1024)

    def test_archive_without_cache_returns_no_shaders(self):
        archive = pak(folder('', [entry('ordinary.txt', 0, len(self.cache))]), self.cache)
        with patch('builtins.open', side_effect=lambda *a, **k: io.BytesIO(archive)):
            self.assertEqual(self.s.scan_file(('res.pak', 'res.pak')), ('res.pak', {}, set(), {}))


if __name__ == '__main__': unittest.main()
