"""An SLCT's stage 5 with no code area header is compute when its control's thread group is filled."""
import importlib.util
import os
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'


class HeaderlessCompute(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('split_slct_headerless', TOOLS / 'shader_split.py')
        cls.s = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}): spec.loader.exec_module(cls.s)

    def programs(self, stage, code, group):
        ctl = bytearray(0x880); ctl[:4] = self.s.CTL_MAGIC
        struct.pack_into('<I', ctl, 0x20, len(ctl)); struct.pack_into('<I', ctl, 0x704, len(code)); struct.pack_into('<3I', ctl, 0x748, *group)
        nv = bytearray(0x20) + struct.pack('<5I', stage, 0, len(code), 0, len(ctl)); struct.pack_into('<II', nv, 0x18, 0x20, 1)   # one stage entry: stage, code offset, length, control offset, length
        slct = bytearray(0x20); struct.pack_into('<5I', slct, 4, 0, 0, 0, 1, 0x18); struct.pack_into('<II', slct, 0x18, 0x20, len(nv))   # one program at +0x20
        return self.s.slct_programs(bytes(slct + nv), code, 0, bytes(ctl), 0, 'test', strict=False), bytes(ctl)

    def test_headerless_stage_5_with_a_thread_group_is_compute(self):
        code = bytes(range(1, 0x41)) * 4
        out, ctl = self.programs(5, code, (8, 8, 1))
        self.assertEqual(out, [('var0', 'cs', ctl, code)])

    def test_headerless_stage_5_without_a_thread_group_is_no_shader(self):
        self.assertIsNone(self.programs(5, bytes(range(1, 0x41)) * 4, (0, 0, 0))[0])

    def test_headerless_code_of_another_stage_is_no_shader(self):
        self.assertIsNone(self.programs(0, bytes(range(1, 0x41)) * 4, (8, 8, 1))[0])

    def test_compute_header_is_compute_at_any_stage_index(self):
        code = self.s.CS_MAGIC + bytes(0xfc)
        out, ctl = self.programs(5, code, (0, 0, 0))
        self.assertEqual(out, [('var0', 'cs', ctl, code)])


if __name__ == '__main__': unittest.main()
