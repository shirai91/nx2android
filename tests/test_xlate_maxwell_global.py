"""Maxwell global memory: ATOM.CAS, and the signed displacement of an extended (.E) 40-bit address (adapted from seedds/nxnative's
dm/tools/tests/test_global_memory.py, MIT, whose GPU runs need Metal; these check the translated SPIR-V).

The address registers hold MOV32I literals, so the translator folds the address and passes it, low and high word, to the helper that resolves a
global address it cannot trace to a storage buffer: the high word shows the displacement's sign. ATOM.CAS goes through a c0 descriptor's
storage buffer (GlobalToStorage) as compiled code's does."""
import unittest

from maxwell_xlate import EXIT, RZ, base, call_args, instructions, mov, stored, sts, translate

OP_ATOMIC_COMPARE_EXCHANGE = 230


def ldc(r, off): return base('Ldc') | (4 << 48) | (off << 20) | (7 << 16) | (RZ << 8) | r
def ldg(rd, disp, e=True): return base('Ldg') | (4 << 48) | (int(e) << 45) | ((disp & 0xffffff) << 20) | (7 << 16) | (2 << 8) | rd
def red(rb, disp): return base('Red') | (1 << 48) | ((disp & 0xfffff) << 28) | (7 << 16) | (2 << 8) | rb   # RED.E.ADD.U32 [R2 + disp], rb


def cas(rd=4, rb=6, disp=0, size64=False, bits50=0):
    # envytools gm107.c's 0xeef0 row: a signed byte displacement at 28..47, E at 48, .64 at 49
    return base('AtomCas') | (1 << 48) | (int(size64) << 49) | (bits50 << 50) | ((disp & 0xfffff) << 28) | (rb << 20) | (7 << 16) | (2 << 8) | rd


def stg(r): return base('Stg') | (4 << 48) | (1 << 45) | (0x20 << 20) | (7 << 16) | (8 << 8) | r   # STG.E [R8 + 0x20], r


class ExtendedDisplacement(unittest.TestCase):
    def address(self, low, high, access):
        calls = call_args(translate([mov(2, low), mov(3, high), mov(7, 1), access, sts(4), EXIT])[0])
        self.assertEqual(len(calls), 1, calls)
        return tuple(calls[0][:2])

    def test_ldg_negative_displacement_keeps_the_high_word(self):
        self.assertEqual(self.address(0x10000, 2, ldg(4, -4)), (0xfffc, 2))   # main: (0xfffc, 3), 4 GB up

    def test_ldg_negative_displacement_borrows_from_the_high_word(self):
        self.assertEqual(self.address(4, 2, ldg(4, -8)), (0xfffffffc, 1))

    def test_ldg_positive_displacement_carries(self):
        self.assertEqual(self.address(0x10000, 2, ldg(4, 4)), (0x10004, 2))
        self.assertEqual(self.address(0xfffffffc, 1, ldg(4, 8)), (4, 2))

    def test_red_displacement_is_signed(self):
        self.assertEqual(self.address(0x10000, 2, red(7, -4)), (0xfffc, 2))   # main: (0x10fffc, 2), 0xffffc read unsigned


class AtomCas(unittest.TestCase):
    def program(self, rd=4, **kw):
        return [ldc(2, 0x140), ldc(3, 0x144), mov(6, 5), mov(7, 9), cas(rd=rd, **kw), ldc(8, 0x140), ldc(9, 0x144), stg(4 if rd == RZ else rd), EXIT]

    def test_compare_and_swap_is_emitted(self):
        for disp in (0, 4, -4):
            with self.subTest(disp=disp):
                data, res = translate(self.program(disp=disp))
                ops = [o for o, _ in instructions(data)]
                self.assertIn(OP_ATOMIC_COMPARE_EXCHANGE, ops, 'main logs "AtomCas is not implemented" and emits nothing')
                self.assertEqual(len(res['sbufs']), 1, res['sbufs'])

    def test_compare_and_value_operands(self):
        # Rb is compared and Rb + 1 swapped in: OpAtomicCompareExchange takes (pointer, scope, equal, unequal, value, comparator)
        data, _ = translate(self.program())
        ins = instructions(data)
        consts = {a[1]: a[2] for o, a in ins if o == 43 and len(a) == 3}
        for o, a in ins:
            if o == 124 and a[2] in consts: consts[a[1]] = consts[a[2]]
        (args,) = [a for o, a in ins if o == OP_ATOMIC_COMPARE_EXCHANGE]
        self.assertEqual((consts.get(args[6]), consts.get(args[7])), (9, 5))

    def test_unsupported_forms_are_refused(self):
        for name, kw in (('64-bit', dict(size64=True)), ('bits 50..51', dict(bits50=1)), ('odd Rb', dict(rb=7)), ('Rb = RZ', dict(rb=RZ))):
            with self.subTest(form=name):
                out = translate(self.program(**kw), refused=True)
                self.assertIn('ATOM.CAS', out)

    def test_rd_rz_still_swaps(self):
        data, _ = translate(self.program(rd=RZ))
        self.assertIn(OP_ATOMIC_COMPARE_EXCHANGE, [o for o, _ in instructions(data)])
        self.assertEqual(stored(data), [])


if __name__ == '__main__':
    unittest.main()
