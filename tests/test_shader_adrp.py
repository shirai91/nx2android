"""Backward ADRP references must participate in raw shader discovery."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch


TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'


def reference(image, pc, target, rd=3, gap=1):
    pages = ((target & ~0xfff) - (pc & ~0xfff)) // 0x1000
    assert -(1 << 20) <= pages < (1 << 20)
    imm = pages & 0x1fffff
    adrp = 0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd
    add = 0x91000000 | (target & 0xfff) << 10 | rd << 5 | rd
    struct.pack_into('<I', image, pc, adrp)
    struct.pack_into('<I', image, pc + gap * 4, add)


@unittest.skipUnless(importlib.util.find_spec('lz4'), 'no lz4')
class ShaderAdrp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(TOOLS))
        spec = importlib.util.spec_from_file_location('shader_split_adrp', TOOLS / 'shader_split.py')
        cls.s = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}):
            spec.loader.exec_module(cls.s)

    def test_preceding_page_instruction(self):
        image = bytearray(0x3000)
        # ADRP x0, PC page - 4096; ADD x0, x0, #0x100 at module +0x2000.
        struct.pack_into('<II', image, 0x2000, 0xf0ffffe0, 0x91040000)
        self.assertEqual(self.s.adrp_targets(image, 0, len(image)), {0x1100: [0x2004]})

    def test_signed_immediate_boundaries_and_low_bits(self):
        pc = 0x2014
        for pages in (-(1 << 20), -4097, -4, -3, -2, -1, 0, 1, 2, 3, 4, (1 << 20) - 1):
            with self.subTest(pages=pages):
                target = (pc & ~0xfff) + pages * 0x1000 + 0xfff
                image = bytearray(0x2100)
                reference(image, pc, target, rd=12, gap=8)
                self.assertEqual(self.s.adrp_targets(image, target, target + 1),
                                 {target: [pc + 32]})
                self.assertEqual(self.s.adrp_targets(image, target - 1, target), {})

    def raw_image(self, pc, before=False):
        image = bytearray(0x5000)
        # A minimal MOD0/dynamic table, with no relocations to pair the shader.
        struct.pack_into('<I', image, 4, 0x40)
        struct.pack_into('<i', image, 0x44, 0x40)
        ctl_at, ctl_size, code_size = 0x1100, 0x880, 0x100
        code_at = ctl_at - code_size if before else ctl_at + ctl_size
        ctl = bytearray(ctl_size)
        ctl[:4] = self.s.CTL_MAGIC
        struct.pack_into('<I', ctl, 0x20, ctl_size)
        struct.pack_into('<I', ctl, 0x704, code_size)
        code = bytearray(code_size)
        code[:4] = self.s.GFX_MAGIC
        struct.pack_into('<I', code, 0x30, 0x461)   # vertex SPH
        image[ctl_at:ctl_at + ctl_size] = ctl
        image[code_at:code_at + code_size] = code
        reference(image, pc, ctl_at)
        reference(image, pc + 0x10, code_at, rd=5)
        return image, bytes(ctl), bytes(code)

    def test_raw_shader_pair_from_forward_and_backward_references(self):
        for pc in (0x200, 0x3000):
            for before in (False, True):
                with self.subTest(pc=pc, before=before):
                    image, ctl, code = self.raw_image(pc, before)
                    out = io.StringIO()
                    with contextlib.redirect_stdout(out):
                        shaders = self.s.raw_shaders(image, 'synthetic')
                    self.assertEqual(shaders, [('synthetic#RAW@0x1100', 'vs', ctl, code)])
                    self.assertEqual(out.getvalue(), '')
                    self.assertEqual(self.s.raw_shaders(image, 'synthetic', [ctl]), [])

    def test_adjacency_without_a_code_reference_is_still_rejected(self):
        image, _, _ = self.raw_image(0x3000)
        image[0x3010:0x3018] = bytes(8)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(self.s.raw_shaders(image, 'synthetic'), [])
        self.assertIn('no code area', out.getvalue())


if __name__ == '__main__':
    unittest.main()
