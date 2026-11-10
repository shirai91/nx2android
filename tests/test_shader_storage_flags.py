"""The Android exporter preserves Ryujinx's storage-buffer Write flag in DMSV."""
import ast
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'nxnative/dm/android/tools'
sys.path.insert(0, str(TOOLS))
import dmsv


def exporter():
    # Load the production classifier/serializer and their constants without
    # starting the command-line exporter or requiring a game shader package.
    source = TOOLS / 'shader_vk.py'
    tree = ast.parse(source.read_text())
    names = {'TEX_BASE', 'SSBO', 'KIND', 'DESC', 'SAMP_NONE'}
    nodes = [n for n in tree.body if
             isinstance(n, ast.FunctionDef) and n.name in ('classify', 'refl_bin') or
             isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in names or
                 isinstance(t, ast.Tuple) and any(isinstance(v, ast.Name) and v.id in names for v in t.elts)
                 for t in n.targets)]
    env = {'re': re, 'struct': struct, 'GAME': {}}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), env)
    return env


class StorageWriteFlags(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.export = exporter()

    def binding(self, stage, slot, offset, flags, index=0, entries=None):
        b = {'name': f'{stage}_s{index}', 'type': 'ssbo', 'binding': 7, 'count': 1}
        if entries is None:
            entries = [{'binding': 7, 'cb_slot': slot, 'cb_offset': offset // 4, 'flags': flags}]
        return self.export['classify'](stage, b, {'sbufs': entries})

    def test_write_bit_for_each_storage_address_source(self):
        # BufferUsageFlags.cs defines Write=1<<0. Bit 1 is not Write.
        for stage, slot, offset, kind in [('vs', 0, 0x110, 'sbuf'), ('ps', 0, 0x510, 'sbuf'),
                                          ('cs', 0, 0x310, 'sbuf'), ('cs', 0, 0x210, 'ubo_ptr'),
                                          ('vs', 3, 0x20, 'sbuf_ubo')]:
            for flags, written in [(0, False), (1, True), (2, False), (3, True)]:
                with self.subTest(stage=stage, kind=kind, flags=flags):
                    b = self.binding(stage, slot, offset, flags)
                    self.assertEqual(b['kind'], kind)
                    self.assertEqual(b['rw'], written)
                    packed = self.export['refl_bin']([b], b'', [1, 1, 1])
                    self.assertEqual(packed[23], 4 if written else 0)

    def test_reflection_is_matched_in_binding_order(self):
        entries = [{'binding': 9, 'cb_slot': 0, 'cb_offset': 0x114 // 4, 'flags': 0},
                   {'binding': 3, 'cb_slot': 0, 'cb_offset': 0x110 // 4, 'flags': 1}]
        self.assertTrue(self.binding('vs', 0, 0, 0, index=0, entries=entries)['rw'])
        self.assertFalse(self.binding('vs', 0, 0, 0, index=1, entries=entries)['rw'])

    def test_missing_reflection_keeps_unknown_register(self):
        b = self.binding('vs', 0, 0, 0, entries=[])
        self.assertFalse(b['rw'])
        self.assertIsNone(b['reg'])
        self.assertEqual(b['kind'], 'sbuf')

    def test_written_flag_survives_bundle_round_trip(self):
        b = self.binding('cs', 0, 0x310, 1)
        reflection = self.export['refl_bin']([b], b'constants', [8, 1, 1])
        # Structurally framed synthetic SPIR-V; no guest shader or GPU involved.
        spv = struct.pack('<5I', 0x07230203, 0x10000, 0, 1, 0)
        levels = [0, dmsv.LEVEL] if dmsv.zstd_mod() else [0]
        with tempfile.TemporaryDirectory(prefix='nx2a_storage_flags_') as tmp:
            for level in levels:
                with self.subTest(level=level):
                    path = Path(tmp) / f'{level}.dmsv'
                    dmsv.write(str(path), [(1, 5, spv, reflection)], level=level)
                    with dmsv.Bundle(str(path)) as bundle:
                        got = bytes(bundle.refl(bundle.entries[0]))
                    self.assertEqual(got[23], 4)
                    self.assertEqual(got[40:], b'constants')


if __name__ == '__main__':
    unittest.main()
