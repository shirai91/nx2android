"""Synthetic PlatinumGames archives exercise the real reader, decompressor and shader scan."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
with patch.dict(os.environ, DM_GAME='010015100B514000'):
    spec = importlib.util.spec_from_file_location('pkz_shader_split', REPO / 'nxnative/dm/gfx/tools/shader_split.py')
    split = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(split)


def shader_dat():
    # One vertex shader inside a DAT, with no proprietary instructions or assets.
    ctl, code = bytearray(0x880), bytearray(0x80)
    ctl[:4] = bytes.fromhex('34127698')
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    code[:4] = bytes.fromhex('78563412')
    struct.pack_into('<I', code, 0x30, 1 << 10)
    shader = b'VSO\0\0\0\1\x0f' + struct.pack('<4I', len(ctl), 0x18, len(code), 0x18 + len(ctl)) + ctl + code
    dat = bytearray(0x40)
    struct.pack_into('<4s6I', dat, 0, b'DAT\0', 1, 0x1c, 0x20, 0x24, 0x34, 0)
    struct.pack_into('<I', dat, 0x1c, len(dat))
    dat[0x20:0x24] = b'vso\0'
    struct.pack_into('<I', dat, 0x24, 12)
    dat[0x28:0x34] = b'shader.vso\0\0'
    struct.pack_into('<I', dat, 0x34, len(shader))
    return bytes(dat) + shader, bytes(ctl), bytes(code)


def archive(version, compression, data):
    names = b'shaders.dat\0' + compression.encode() + b'\0'
    encoded = split.zstd_mod().compress(data) if compression == 'ZStandard' else data
    offset = 0x40 + len(names)
    return (struct.pack('<4sIQ4I', b'pkzl', version, offset + len(encoded), 1, 0x20, len(names), 0 if version == 0x10000 else 0x1000)
            + struct.pack('<IIQQQ', 0, len(b'shaders.dat\0'), len(data), offset, len(encoded)) + names + encoded)


class PkzShaders(unittest.TestCase):
    def test_known_versions_extract_nested_shader_objects(self):
        data, ctl, code = shader_dat()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'root.pkz'
            for version in (0x10000, 0x20000):
                for compression in ('ZStandard', 'None'):
                    with self.subTest(version=hex(version), compression=compression):
                        path.write_bytes(archive(version, compression, data))
                        jobs, paths = split.pkz_jobs([(str(path), path.name)])
                        self.assertEqual(paths, {str(path)})
                        out = []
                        for p, rel, entries in jobs:
                            self.assertEqual(entries[0][:2], ('shaders.dat', compression))
                            split.pkz_shaders(p, rel, entries, out)
                        self.assertEqual(out, [('/root.pkz/shaders.dat/shader.vso#PGSO', 'vs', ctl, code)])

    def test_unknown_version_still_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'root.pkz'
            path.write_bytes(archive(0x30000, 'None', b''))
            with self.assertRaisesRegex(SystemExit, 'PKZ version 0x30000'):
                split.pkz_entries(path)

    def test_non_pkz_is_not_consumed(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'other.pkz'
            path.write_bytes(b'not an archive')
            self.assertIsNone(split.pkz_entries(path))


if __name__ == '__main__':
    unittest.main()
