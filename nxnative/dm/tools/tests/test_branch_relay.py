"""The rewriter's branches beyond B's reach (dm/tools/patch_nso.py, a text longer than ~125 MB such as Another Eden's 187 MB): which NOPs may serve as relays, and that B() reaches a far
target through one. Real AArch64 words, no game files; the driver's top level is not executed (its functions are compiled alone, as in test_jump_copies.py)."""
import ast
from pathlib import Path
import bisect
import struct
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import nso_asm
from nso_asm import B, NOP

RET = 0xD65F03C0
BODY = 0xAA0103E0   # mov x0, x1: any instruction that is neither a NOP nor a terminator

tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
SOURCE = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('find_relay_slots', 'B')]
assert len(SOURCE) == 2


def scope_for(words, ro_pointers=(), reach=0x100):
    """the names the two functions read, for a text of `words` followed by a read-only segment holding 8-byte `ro_pointers`"""
    text = struct.pack('<%dI' % len(words), *words)
    img = bytearray(text + struct.pack('<%dQ' % len(ro_pointers), *ro_pointers))
    stats = {}
    scope = {'bisect': bisect, 'nso_asm': nso_asm, 'IMG': img, 'TEXT_END': len(text), 'RO_OFF': len(text), 'NAME': 'main', 'RELAY_REACH': reach,
             'relay_slots': None, 'relay_writes': [], 'relay_cache': {}, 'stat': lambda k: stats.__setitem__(k, stats.get(k, 0) + 1), 'stats': stats}
    exec(compile(ast.Module(body=SOURCE, type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)
    return scope


class RelaySlots(unittest.TestCase):
    def slots(self, words, ro_pointers=()): return scope_for(words, ro_pointers)['find_relay_slots']()

    def test_padding_after_a_terminator_but_not_its_last_nop(self):
        # 0: ret, 4/8/12: padding, 12 may be the next function's first instruction
        self.assertEqual(self.slots([RET, NOP, NOP, NOP, BODY]), [4, 8])

    def test_nops_inside_a_function_are_not_padding(self):
        self.assertEqual(self.slots([BODY, NOP, NOP, NOP, BODY, RET]), [])

    def test_every_terminator_starts_padding(self):
        for term in (RET, B(0, 0x40), 0xD61F0200):   # ret, b, br x16
            with self.subTest(term=hex(term)):
                self.assertEqual(self.slots([term, NOP, NOP, BODY]), [4])

    def test_a_single_nop_is_never_a_slot(self):
        self.assertEqual(self.slots([RET, NOP, BODY]), [])

    def test_a_branch_target_is_not_a_slot(self):
        # slots 4, 8, 12 (16 ends the run); the word at 24 branches back to 8
        d = (8 - 24) >> 2
        encoded = {'b': 0x14000000 | d & 0x3FFFFFF, 'bl': 0x94000000 | d & 0x3FFFFFF, 'b.eq': 0x54000000 | (d & 0x7FFFF) << 5, 'cbz': 0xB4000000 | (d & 0x7FFFF) << 5,
                   'tbz': 0x36000000 | (d & 0x3FFF) << 5, 'adr': 0x10000000 | ((8 - 24) & 3) << 29 | (((8 - 24) >> 2) & 0x7FFFF) << 5}
        self.assertEqual(self.slots([RET, NOP, NOP, NOP, NOP, BODY, BODY]), [4, 8, 12])
        for name, word in encoded.items():
            with self.subTest(name=name):
                self.assertEqual(self.slots([RET, NOP, NOP, NOP, NOP, BODY, word]), [4, 12])

    def test_a_pointer_in_the_read_only_segment_is_not_a_slot(self):
        self.assertEqual(self.slots([RET, NOP, NOP, NOP, NOP, BODY], ro_pointers=[8]), [4, 12])


class FarBranch(unittest.TestCase):
    def padded(self, n=200):   # 8 NOPs of padding after a ret every 25 instructions, filler between
        words = []
        while len(words) < n: words += [BODY] * 15 + [RET] + [NOP] * 8 + [BODY]
        return words

    def test_a_near_branch_is_the_encoders(self):
        s = scope_for(self.padded())
        self.assertEqual(s['B'](0x10, 0x80), B(0x10, 0x80)); self.assertEqual(s['relay_writes'], [])

    def test_a_far_branch_goes_through_a_padding_nop_in_reach_of_both_ends(self):
        s = scope_for(self.padded(), reach=0x100)
        frm, to = -0x20, 0x180   # 0x1a0 apart
        word = s['B'](frm, to)
        (v, target), = s['relay_writes']
        self.assertEqual(target, to); self.assertEqual(word, B(frm, v))
        self.assertTrue(-0x100 <= v - frm < 0x100 and -0x100 <= to - v < 0x100)
        self.assertEqual(struct.unpack_from('<I', s['IMG'], v)[0], NOP, 'the slot is only reserved: the image changes when the writes are applied at the end')

    def test_the_same_target_reuses_its_relay_and_slots_are_not_shared(self):
        s = scope_for(self.padded(), reach=0x100)
        a, b = s['B'](-0x20, 0x180), s['B'](-0x30, 0x180)
        self.assertEqual(len(s['relay_writes']), 1); self.assertEqual(a & 0xFC000000, b & 0xFC000000)
        s['B'](-0x20, 0x1a0)
        self.assertEqual(len({v for v, _ in s['relay_writes']}), 2)

    def test_no_slot_in_reach_fails_loudly(self):
        s = scope_for([BODY] * 100, reach=0x100)
        with self.assertRaises(AssertionError): s['B'](-0x20, 0x180)


if __name__ == '__main__': unittest.main()
