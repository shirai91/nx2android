"""Maxwell FMNMX orders -0 below +0, whatever the operand order (adapted from the Maxwell contracts of seedds/nxnative's
test_dmsh_ampere_orin.py, MIT): min(+0, -0) is -0 and max(+0, -0) is +0. SPIR-V's FMin / FMax (and NMin / NMax) may return either zero;
maxwell_xlate.run models a device that returns the second operand of two equal ones, which is how main gave the wrong zero."""
import struct
import unittest

from maxwell_xlate import EXIT, PT, base, mov, run, s2r_tid, sts, translate

POS0, NEG0, ONE, NAN = 0, 0x80000000, 0x3f800000, 0x7fc00000


def bits(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def fmnmx(is_min=True, pred=PT, nega=False, absb=False):   # FMNMX R4, R1, R2, [!]pred
    return base('FmnmxR') | 4 | 1 << 8 | 7 << 16 | 2 << 20 | pred << 39 | int(not is_min) << 42 | int(nega) << 48 | int(absb) << 49


class SignedZeroMinMax(unittest.TestCase):
    def result(self, a, b, **kw):
        values = run(translate([mov(1, a), mov(2, b), fmnmx(**kw), sts(4), EXIT])[0])
        self.assertEqual(len(values), 1, values)
        return values[0]

    def test_zeros_in_both_orders(self):
        for a, b in ((POS0, NEG0), (NEG0, POS0)):
            with self.subTest(a=hex(a), b=hex(b)):
                self.assertEqual(self.result(a, b, is_min=True), NEG0)
                self.assertEqual(self.result(a, b, is_min=False), POS0)
        for z in (POS0, NEG0):
            with self.subTest(zero=hex(z)):
                self.assertEqual(self.result(z, z, is_min=True), z)
                self.assertEqual(self.result(z, z, is_min=False), z)

    def test_after_operand_modifiers(self):
        # -a of +0 is -0, |b| of -0 is +0
        self.assertEqual(self.result(POS0, POS0, is_min=True, nega=True), NEG0)
        self.assertEqual(self.result(NEG0, NEG0, is_min=False, absb=True), POS0)

    def test_numbers_and_nan_are_unchanged(self):
        self.assertEqual(self.result(ONE, bits(-2.0), is_min=True), bits(-2.0))
        self.assertEqual(self.result(ONE, bits(-2.0), is_min=False), ONE)
        self.assertEqual(self.result(NAN, ONE, is_min=True), ONE)
        self.assertEqual(self.result(NEG0, NAN, is_min=False), NEG0)

    def test_run_time_predicate_translates(self):
        # FMNMX with P0 from a run-time compare: both sides stay, and the module validates
        isetp = base('IsetpR') | 7 << 16 | 1 << 8 | 2 << 20 | 2 << 49 | PT << 39 | PT << 3   # ISETP.EQ.AND P0, PT, R1, R2, PT
        translate([s2r_tid(1), s2r_tid(2), isetp, fmnmx(pred=0), sts(4), EXIT])


if __name__ == '__main__':
    unittest.main()
