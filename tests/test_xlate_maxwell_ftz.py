"""Maxwell float comparisons with .FTZ (adapted from seedds/nxnative's test_dmsh_maxwell_ftz.py, MIT): a denormal operand compares as zero, as on
NVIDIA. The flag is FSETP's bit 47, FSET's bit 55 and FCMP's bit 47; FCMP flushes only the source it compares. A flushed operand is
|x| < FLT_MIN ? 0 : x, so the tests count the comparisons with FLT_MIN (0x00800000) in the SPIR-V."""
import unittest

from maxwell_xlate import EXIT, PT, RZ, instructions, op, s2r_tid, sts, translate

NEU = 0xd
FLT_MIN = 0x00800000
FLOAT_COMPARES = range(180, 192)   # OpFOrdEqual .. OpFUnordGreaterThanEqual


def fsetp(a, b, ftz=True): return op(0x5bb, 52, a=a, b=b) | NEU << 48 | int(ftz) << 47 | PT << 39 | PT   # FSETP.NEU.AND P0, PT, a, b, PT
def fset(rd, a, b, ftz=True): return op(0x58, 56, rd=rd, a=a, b=b) | int(ftz) << 55 | 1 << 52 | NEU << 48 | PT << 39   # FSET.BF.NEU.AND
def fcmp(rd, a, b, c, ftz=True): return op(0x5ba, 52, rd=rd, a=a, b=b) | NEU << 48 | int(ftz) << 47 | c << 39           # FCMP.NEU rd, a, b, c
def sel(rd, a, b): return op(0x5ca, 52, rd=rd, a=a, b=b)                                                                 # SEL rd, a, b, P0


class MaxwellCompareFtz(unittest.TestCase):
    def flushes(self, *body):
        """comparisons with FLT_MIN in a program whose R1, R2, R3 are the thread index (nothing folds) and whose R3 is stored"""
        data, _ = translate([s2r_tid(1), s2r_tid(2), s2r_tid(3), *body, sts(3, 8), EXIT])
        ins = instructions(data)
        flt_min = {a[1] for o, a in ins if o == 43 and len(a) == 3 and a[2] == FLT_MIN}
        return sum(1 for o, a in ins if o in FLOAT_COMPARES and set(a[2:]) & flt_min)

    def test_fsetp_flushes_both_operands(self):
        self.assertEqual(self.flushes(fsetp(1, 2), sel(3, 1, 2)), 2)

    def test_fsetp_without_ftz_compares_as_is(self):
        self.assertEqual(self.flushes(fsetp(1, 2, ftz=False), sel(3, 1, 2)), 0)

    def test_fsetp_against_zero_flushes_one(self):
        self.assertEqual(self.flushes(fsetp(1, RZ), sel(3, 1, 2)), 1)

    def test_fset_flushes_both_operands(self):
        self.assertEqual(self.flushes(fset(3, 1, 2)), 2)

    def test_fset_without_ftz_compares_as_is(self):
        self.assertEqual(self.flushes(fset(3, 1, 2, ftz=False)), 0)

    def test_fcmp_flushes_only_what_it_compares(self):
        self.assertEqual(self.flushes(fcmp(3, 1, 2, 3)), 1)

    def test_fcmp_without_ftz_compares_as_is(self):
        self.assertEqual(self.flushes(fcmp(3, 1, 2, 3, ftz=False)), 0)


if __name__ == '__main__':
    unittest.main()
