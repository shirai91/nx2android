"""Nested shader archives must be scanned without unbounded decompression."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
path = ROOT / 'nxnative/dm/gfx/tools/shader_split.py'
spec = importlib.util.spec_from_file_location('shader_split_nested', path)
shader = importlib.util.module_from_spec(spec)
with mock.patch.dict(os.environ, {'DM_GAME': '0100000000010000'}):
    spec.loader.exec_module(shader)


def sarc(member):
    data = bytearray(0x38 + len(member))
    data[:4] = b'SARC'
    struct.pack_into('<HHII', data, 4, 0x14, 0xFEFF, len(data), 0x38)
    struct.pack_into('<H', data, 0x1A, 1)
    struct.pack_into('<IIII', data, 0x20, 0, 0, 0, len(member))
    data[0x38:] = member
    return bytes(data)


class NestedZstdTest(unittest.TestCase):
    def test_scan_file_visits_compressed_sarc_member(self):
        bnsh = bytearray(0x80); bnsh[:8] = b'BNSH\0\0\0\0'
        struct.pack_into('<I', bnsh, 0x1C, len(bnsh)); bnsh[0x60:0x64] = b'grsc'
        payload = sarc(shader.zstd_mod().compress(sarc(bytes(bnsh))))
        with tempfile.TemporaryDirectory() as temp, mock.patch.object(shader, 'bnsh_shaders', return_value=[]) as visit:
            path = Path(temp) / 'model.pack'; path.write_bytes(payload)
            shader.scan_file((str(path), 'model.pack'))
        visit.assert_called_once()

    def test_expansion_and_invalid_frames_fail(self):
        packed = shader.zstd_mod().compress(bytes(4096))
        with self.assertRaisesRegex(SystemExit, 'expansion limit'):
            shader.nested_zstd(packed, 'test', 64)
        with self.assertRaisesRegex(SystemExit, 'incomplete or trailing'):
            shader.nested_zstd(packed[:-1], 'test', 8192)
        with self.assertRaisesRegex(SystemExit, 'incomplete or trailing'):
            shader.nested_zstd(packed + b'extra', 'test', 8192)

    def test_nested_budget_and_depth_are_bounded(self):
        with self.assertRaisesRegex(SystemExit, 'nesting'):
            shader.scan_blob(b'', 'test', [], depth=33)
        data = shader.zstd_mod().compress(bytes(128))
        with self.assertRaisesRegex(SystemExit, 'expansion limit'):
            shader.scan_blob(data, 'test', [], budget=[64])

    def test_missing_dictionary_is_reported(self):
        with mock.patch.dict(shader.ZDICTS, {}, clear=True):
            with self.assertRaisesRegex(SystemExit, 'needs zstd dictionary 1'):
                shader.nested_zstd(bytes.fromhex('28b52ffd010001'), 'test', 8192)


if __name__ == '__main__':
    unittest.main()
