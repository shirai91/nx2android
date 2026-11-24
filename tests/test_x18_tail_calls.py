"""Tail-call recognition on synthetic AArch64 code, without a game image."""
import ast
import os
from pathlib import Path
import re
import struct
import sys
import unittest
from unittest.mock import patch

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/tools'
sys.path.insert(0, str(TOOLS))
from nso_asm import ADDI, B, BEQ, BLR, BR, LDP, LDP_POST, LDRX, NOP, STP, STP_PRE, STRX, SUBI

tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
scope = {'re': re}
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('tail_call', 'tail_call_frame', 'tail_call_padded', 'tail_call_proven')], type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)


def mov(dst, src, wide=True): return (0xaa0003e0 if wide else 0x2a0003e0) | src << 16 | dst


class TailCalls(unittest.TestCase):
    def decode(self, words):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0x1000))
        self.assertEqual(len(ins), len(words))
        return ins

    def accepts(self, words):
        ins = self.decode(words)
        j = next(k for k, p in enumerate(ins) if p.mnemonic == 'br' and p.op_str == 'x18')
        return scope['tail_call'](ins, j) or scope['tail_call_proven'](ins, j)

    def frame(self):
        # Synthetic control flow, not a reconstruction of missing game instructions.
        # The target load and 0x90-byte epilogue match the reported instruction form.
        return [SUBI(31, 31, 0x90), STRX(21, 31, 12), STP(20, 19, 31, 0x70),
                STP(29, 30, 31, 0x80), ADDI(29, 31, 0x80),
                BEQ(0x1014, 0x1068), BEQ(0x1018, 0x1028),
                BLR(8), mov(10, 0), B(0x1024, 0x1030),
                ADDI(10, 10, 16), BEQ(0x102c, 0x1028),
                LDP(18, 10, 10, 0), mov(1, 3), STP(12, 11, 29, 0x30),
                mov(2, 4), STP(17, 10, 29, 0x40), mov(4, 9),
                STP(16, 15, 29, 0x10), LDP(29, 30, 31, 0x80),
                LDRX(21, 31, 12), LDP(20, 19, 31, 0x70),
                ADDI(31, 31, 0x90), BR(18), NOP, NOP,
                0x94000400, 0]  # an error call and trap after the tail

    def test_fixed_frame_pair_load_with_body_branches_and_call(self):
        self.assertTrue(self.accepts(self.frame()))

    def test_fixed_frame_plain_pointer_load(self):
        words = self.frame(); words[12] = LDRX(18, 10)
        self.assertTrue(self.accepts(words))

    def test_frame_pointer_offset_can_be_zero_or_printed_in_decimal(self):
        for disp in (0, 8):
            with self.subTest(disp=disp):
                self.assertTrue(self.accepts([SUBI(31, 31, 32), STP(29, 30, 31, disp),
                                             ADDI(29, 31, disp), LDP(18, 10, 10, 0),
                                             LDP(29, 30, 31, disp), ADDI(31, 31, 32), BR(18), 0]))

    def test_tail_stub_preserves_return_address_and_stack(self):
        from test_x18_window_exits import load
        ins = self.decode(self.frame()); self.assertTrue(scope['tail_call_proven'](ins, 23))
        for target in ('android', 'apple'):
            with self.subTest(target=target), patch.dict(os.environ):
                ns = load(); ns['TARGET'] = target
                ns['site_br_call'](ins[23])
                stub = list(ns['md'].disasm(bytes(ns['patch']), ns['PATCH_OFF']))
                self.assertEqual((stub[-1].mnemonic, stub[-1].op_str), ('br', 'x16'))
                writes = {p.reg_name(r) for p in stub for r in p.regs_access()[1]}
                self.assertFalse(writes & {'sp', 'wsp', 'lr', 'x30', 'w30', 'x18', 'w18'})
                self.assertTrue(any(p.mnemonic == 'ldr' and p.op_str == 'x16, [x16]' for p in stub))

    def test_existing_terminal_and_post_index_forms(self):
        self.assertTrue(self.accepts([LDRX(18, 0, 3), BR(18)]))
        self.assertTrue(self.accepts([STP_PRE(29, 30, 31, -16), ADDI(29, 31, 0),
                                     LDRX(18, 0, 2), LDP_POST(29, 30, 31, 16), BR(18), 0]))

    def test_frame_must_be_balanced_saved_and_not_clobbered(self):
        cases = [(0, NOP), (0, SUBI(31, 31, 0x80)), (3, NOP),
                 (3, STP(29, 30, 31, 0x70)), (4, ADDI(29, 31, 0x70)),
                 (13, mov(29, 0, False)), (13, mov(30, 0, False)),
                 (13, ADDI(31, 31, 16)), (19, LDP(29, 30, 31, 0x70)),
                 (20, mov(30, 0)), (22, ADDI(31, 31, 0x80))]
        for at, word in cases:
            with self.subTest(at=at, word=hex(word)):
                words = self.frame(); words[at] = word
                self.assertFalse(self.accepts(words))

    def test_saved_pair_cannot_be_overwritten_or_aliased(self):
        for word in (STRX(0, 31, 16), STRX(0, 29), STP(0, 1, 31, 0x78)):
            with self.subTest(word=hex(word)):
                words = self.frame(); words[14] = word
                self.assertFalse(self.accepts(words))

    def test_target_must_be_an_unclobbered_pointer_load(self):
        for at, word in ((12, LDP(18, 10, 10, 0) & 0x7fffffff),
                         (12, ADDI(18, 10, 8)), (13, mov(18, 0, False)),
                         (13, mov(18, 0)), (13, BLR(8))):
            with self.subTest(at=at, word=hex(word)):
                words = self.frame(); words[at] = word
                self.assertFalse(self.accepts(words))

    def test_branches_cannot_skip_or_reenter_frame_setup_or_tail(self):
        for at, target in ((2, 0x1030), (5, 0x1000), (5, 0x100c),
                           (5, 0x1010), (5, 0x1058), (5, 0x105c), (13, 0x105c),
                           (26, 0x1030), (26, 0x1050)):
            with self.subTest(at=at, target=hex(target)):
                words = self.frame(); words[at] = BEQ(0x1000 + 4 * at, target)
                self.assertFalse(self.accepts(words))

    def test_unframed_pair_load_and_incomplete_disassembly_stay_unproven(self):
        self.assertFalse(self.accepts([LDP(18, 10, 10, 0), BR(18), mov(0, 1)]))
        ins = self.decode(self.frame()); del ins[10]
        self.assertFalse(scope['tail_call'](ins, 22) or scope['tail_call_proven'](ins, 22))


if __name__ == '__main__': unittest.main()
