"""Synthetic embedded and archived Media.Vision shaders."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

import lz4.block

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'
sys.path.insert(0, str(TOOLS))


class InlineShaders(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault('DM_GAME', '0100000000010000')
        spec = importlib.util.spec_from_file_location('split_inline_under_test', TOOLS / 'shader_split.py')
        cls.split = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.split)

    def sections(self, stage):
        code = bytearray(0x100)
        code[:4] = self.split.CS_MAGIC if stage == 'cs' else self.split.GFX_MAGIC
        struct.pack_into('<I', code, 0x30, (1 if stage == 'vs' else 5) << 10)
        control = bytearray(0x880)
        control[:4] = self.split.CTL_MAGIC
        struct.pack_into('<I', control, 0x20, len(control))
        struct.pack_into('<I', control, 0x704, len(code))
        return bytes(control), bytes(code)

    def test_nx_shaders_embedded_in_main(self):
        blob = bytearray(b'NXFS' + bytes(0x40))
        expected = []
        for stage, magic in (('vs', b'NXVS'), ('ps', b'NXFS'), ('cs', b'NXCS')):
            control, code = self.sections(stage)
            padding = bytes(0x28)
            code_at = 0x40 + len(control) + len(padding)
            header = struct.pack('<4s6I', magic, code_at + len(code), 0, 0x40,
                                 len(control) + len(padding), code_at, len(code))
            expected.append((f'main#NX@{len(blob):#x}', stage, control, code))
            record = header.ljust(0x40, b'\0') + control + padding + code
            blob.extend(record)
        blob.extend(record[:-1])
        self.assertEqual(self.split.embedded_nx_shaders(bytes(blob), 'main'), expected)

    def test_wide_mvgl_pair_and_bounded_entry(self):
        nodes = bytes(16)
        labels = bytes(128)
        records = bytearray()
        payload = bytearray()
        keys = []
        for idx, (stage, magic, name) in enumerate((('vs', b'NXVS', b'material_vp'), ('ps', b'NXFS', b'material_fp'))):
            control, code = self.sections(stage)
            code_at = 0x40 + len(control) + 16
            header = struct.pack('<4s6I', magic, code_at + len(code), 0, 0x40, len(control) + 16, code_at, len(code))
            shader = header.ljust(0x40, b'\0') + control + bytes(16) + code
            body = lz4.block.compress(shader, store_size=False) if idx == 0 else shader
            nodes += struct.pack('<4I', 0, idx, 0, 0)
            labels += (b'shad' + name).ljust(128, b'\0')
            records.extend(struct.pack('<QII', len(payload), len(shader), len(body)))
            payload.extend(body)
            keys.append(self.split.skey(control, code))
        start = 32 + len(nodes) + len(labels) + len(records)
        header = struct.pack('<4sIIIQQ', b'MDB1', 3, 3, 2, start, start + len(payload))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'archive.nx64.mvgl'
            path.write_bytes(header + nodes + labels + records + payload)
            _, found, pairs, _ = self.split.scan_file((str(path), path.name))
            self.assertEqual(set(found), set(keys))
            self.assertEqual(pairs, {tuple(keys)})

            struct.pack_into('<Q', records, 0, len(payload))
            path.write_bytes(header + nodes + labels + records + payload)
            with self.assertRaisesRegex(SystemExit, 'material_vp: invalid shader member range'):
                self.split.scan_file((str(path), path.name))


if __name__ == '__main__':
    unittest.main()
