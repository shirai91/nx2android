"""Observation gates retain an ABI call boundary when a checked cut removes BL."""
from pathlib import Path
import os
import struct
import sys
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/tools'
REWRITER = TOOLS / 'patch_nso.py'
NOP, BL, MOV, RET = 0xd503201f, 0x94000002, 0xaa1303e0, 0xd65f03c0


def word(value):
    return struct.pack('<I', value).hex()


def rewrite(previous=BL, replacement=NOP, *, original=None, gate_word=MOV,
            kinds=None, starts=(0,)):
    """Run production cut validation and observation emission on synthetic code."""
    source = REWRITER.read_text(encoding='utf-8')
    bid = '00' * 16
    cuts = [] if replacement is None else [(4, word(previous if original is None else original), word(replacement), 'synthetic cut')]
    scope = dict(IMG=bytearray(struct.pack('<4I', NOP, previous, MOV, RET)),
                 TEXT_END=16, BUILD_ID=bid, PATCHES={'CUTS': {bid: cuts},
                 'OBS': {bid: [(8, 21, f'{gate_word:08x}', 'synthetic allocator gate')]}},
                 PATCH_OFF=0x10000, KEY_PAGE_OFF=0x8000,
                 KEY_INDEX={'K18': 0, 'KTLS': 1, 'KTLR': 2, 'KGATE': 3},
                 starts=starts, kinds=kinds or {}, windows=[])
    with patch.object(sys, 'path', [str(TOOLS), *sys.path]), patch.dict(os.environ, {'DM_TARGET': 'android'}):
        exec('import struct, re, os\nfrom itertools import combinations\nfrom capstone import *\nfrom nso_asm import *', scope)
        exec(compile(source[source.index('cuts = list('):source.index('# Key page indices')], str(REWRITER), 'exec'), scope)
        exec(compile(source[source.index('md = Cs('):source.index('# ---- Exclusive windows')], str(REWRITER), 'exec'), scope)
        branch = next(line for line in source.splitlines() if line.startswith('def put_branch('))
        exec(branch, scope)
        exec(compile(source[source.index('def scratch_dead('):source.index('# ---- Frame step in whole vsyncs')], str(REWRITER), 'exec'), scope)
    return scope


class ObservationCutCalls(unittest.TestCase):
    def test_removed_call_retains_boundary_and_relocates_original_instruction(self):
        scope = rewrite()
        self.assertEqual(struct.unpack_from('<I', scope['IMG'], 4)[0], NOP)
        self.assertEqual([gate[1:] for gate in scope['gates']], [('obs', 21, 8)])
        code = list(scope['md'].disasm(bytes(scope['patch']), scope['PATCH_OFF']))
        self.assertEqual(bytes(code[-2].bytes), struct.pack('<I', MOV))
        self.assertEqual((code[-1].mnemonic, code[-1].operands[0].imm), ('b', 12))

    def test_existing_call_boundary_still_works(self):
        self.assertEqual(rewrite(replacement=None)['stats'], {'obs': 1})

    def test_cut_original_bytes_must_match(self):
        with self.assertRaisesRegex(AssertionError, 'cut original bytes mismatch'):
            rewrite(previous=NOP, original=BL)

    def test_nop_without_original_call_is_not_a_boundary(self):
        for previous in (NOP, 0x14000002):  # NOP or B, never a call.
            with self.subTest(previous=hex(previous)), self.assertRaisesRegex(AssertionError, 'observation gate neither'):
                rewrite(previous=previous)

    def test_call_replaced_by_non_nop_is_not_a_boundary(self):
        with self.assertRaisesRegex(AssertionError, 'observation gate neither'):
            rewrite(replacement=0xaa0003e9)  # mov x9, x0 can introduce a live value.

    def test_original_gate_instruction_must_match(self):
        with self.assertRaisesRegex(AssertionError, 'observation gate original instruction mismatch'):
            rewrite(gate_word=NOP)

    def test_gate_must_not_overlap_rewrite_site(self):
        with self.assertRaisesRegex(AssertionError, 'observation gate conflicts'):
            rewrite(kinds={8: ('x18', None)})


if __name__ == '__main__': unittest.main()
