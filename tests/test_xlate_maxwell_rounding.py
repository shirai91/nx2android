"""Directed rounding (.RM, .RP, .RZ) of Maxwell FADD, FMUL and FFMA, adapted from the Maxwell cases of seedds/nxnative's
test_dmsh_ampere_orin.py (MIT), whose runs need Metal.

The inputs are MOV32I literals. The translator folds what it can and maxwell_xlate.run executes the rest of the SPIR-V with exact IEEE
binary32 arithmetic; the result is compared with the exact value rounded by the mode. Before the fix every mode gave the nearest result."""
import random
import struct
import unittest
from fractions import Fraction

from maxwell_xlate import EXIT, base, f32_round, f32_value, instructions, mov, run, s2r_tid, sts, translate

RN, RM, RP, RZ = 0, 1, 2, 3
MODE = {RN: 'rn', RM: 'rm', RP: 'rp', RZ: 'rz'}


def bits(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def fadd(rm, ftz=False): return base('FaddR') | 4 | 1 << 8 | 7 << 16 | 2 << 20 | rm << 39 | int(ftz) << 44
def fmul(rm, ftz=False, scale=0): return base('FmulR') | 4 | 1 << 8 | 7 << 16 | 2 << 20 | rm << 39 | scale << 41 | int(ftz) << 44
def ffma(rm, ftz=False): return base('FfmaR') | 4 | 1 << 8 | 7 << 16 | 2 << 20 | 3 << 39 | rm << 51 | int(ftz) << 53


def flush(x): return x & 0x80000000 if (x & 0x7fffffff) < 0x00800000 else x


def oracle(kind, rm, a, b, c=0, ftz=False, scale=Fraction(1)):
    if ftz: a, b, c = flush(a), flush(b), flush(c)
    va, vb, vc = f32_value(a), f32_value(b), f32_value(c)
    exact = {'fadd': va + vb, 'fmul': va * vb * scale, 'ffma': va * vb + vc}[kind]
    if exact == 0:   # an exact zero: x + y keeps the sign of two zeros alike, else is +0, or -0 rounding down
        if kind == 'fmul': r = (a ^ b) & 0x80000000
        else:
            (sx, zx), (sy, zy) = ((a & 0x80000000, va == 0), (b & 0x80000000, vb == 0)) if kind == 'fadd' else \
                (((a ^ b) & 0x80000000, va * vb == 0), (c & 0x80000000, vc == 0))
            r = sx if zx and zy and sx == sy else (0x80000000 if rm == RM else 0)
    else:
        r = f32_round(exact, MODE[rm])
    return flush(r) if ftz else r


def finite(rng, lo=-20, hi=20):
    while True:
        x = rng.getrandbits(32)
        e = (x >> 23 & 0xff) - 127
        if lo <= e <= hi: return x


class DirectedRounding(unittest.TestCase):
    def result(self, inst, a, b, c=0):
        values = run(translate([mov(1, a), mov(2, b), mov(3, c), inst, sts(4), EXIT])[0])
        self.assertEqual(len(values), 1, values)
        return values[0]

    def check(self, kind, rm, a, b, c=0, ftz=False):
        inst = {'fadd': fadd, 'fmul': fmul, 'ffma': ffma}[kind](rm, ftz)
        got, want = self.result(inst, a, b, c), oracle(kind, rm, a, b, c, ftz)
        self.assertEqual(hex(got), hex(want), f'{kind}.{MODE[rm]}{".ftz" if ftz else ""}({a:#x}, {b:#x}, {c:#x})')

    def test_inexact_results_round_by_mode(self):
        one, tiny = bits(1.0), bits(2.0 ** -30)
        for rm, want in ((RN, one), (RM, one), (RP, one + 1), (RZ, one)):
            with self.subTest(mode=MODE[rm]):
                self.assertEqual(self.result(fadd(rm), one, tiny), want)
                self.assertEqual(self.result(ffma(rm), one, tiny, one), want)
                self.assertEqual(self.result(fadd(rm), one | 1 << 31, tiny | 1 << 31), {RM: one + 1}.get(rm, one) | 1 << 31)
        third = bits(1 / 3)
        for rm in (RM, RP, RZ):   # 3 * (1/3 rounded to nearest) is just below 1
            with self.subTest(mode=MODE[rm], op='fmul'):
                self.assertEqual(self.result(fmul(rm), bits(3.0), third), oracle('fmul', rm, bits(3.0), third))

    def test_random_operands(self):
        rng = random.Random(2051)
        for kind in ('fadd', 'fmul', 'ffma'):
            for rm in (RM, RP, RZ):
                for _ in range(12):
                    a, b, c = finite(rng), finite(rng), finite(rng)
                    if kind == 'fadd' and rng.random() < 0.5: b = (a ^ 0x80000000) + rng.randrange(-3, 4)   # near cancellation
                    if kind == 'ffma' and rng.random() < 0.5:
                        c = f32_round(-f32_value(a) * f32_value(b)) ^ rng.randrange(0, 4)   # the product nearly cancels c
                    with self.subTest(kind=kind, mode=MODE[rm], a=hex(a), b=hex(b), c=hex(c)):
                        self.check(kind, rm, a, b, c)

    def test_overflow_rounds_toward_zero_to_the_largest_finite_value(self):
        big = 0x7f7fffff
        for rm, want in ((RZ, big), (RM, big), (RP, 0x7f800000)):
            with self.subTest(mode=MODE[rm]):
                self.assertEqual(self.result(fadd(rm), big, big), want)
                self.assertEqual(self.result(fmul(rm), big, bits(2.0)), want)
        self.assertEqual(self.result(fadd(RP), big | 1 << 31, big | 1 << 31), 0xff7fffff)

    def test_exact_zero_sums_are_minus_zero_rounding_down(self):
        one = bits(1.0)
        self.assertEqual(self.result(fadd(RM), one, one | 1 << 31), 0x80000000)
        self.assertEqual(self.result(fadd(RP), one, one | 1 << 31), 0)
        self.assertEqual(self.result(fadd(RM), 0, 0), 0)
        self.assertEqual(self.result(ffma(RM), one, one, one | 1 << 31), 0x80000000)

    def test_scaled_multiply(self):
        # FMUL.D2 / .D8 / .M4 / .M2: the scale is applied once with the product's rounding
        for scale, factor in ((1, Fraction(1, 2)), (3, Fraction(1, 8)), (5, Fraction(4)), (6, Fraction(2))):
            for rm in (RM, RP, RZ):
                a, b = bits(1 / 3), bits(3.0000002)
                with self.subTest(scale=scale, mode=MODE[rm]):
                    got = self.result(fmul(rm, scale=scale), a, b)
                    self.assertEqual(hex(got), hex(oracle('fmul', rm, a, b, scale=factor)))

    def test_directed_ftz_keeps_the_sign(self):
        denormal = 0x80000005
        self.assertEqual(self.result(fadd(RP, ftz=True), denormal, 0x80000000), 0x80000000)
        self.assertEqual(self.result(fmul(RZ, ftz=True), denormal, bits(1.0)), 0x80000000)

    def test_run_time_operands_keep_the_error_terms_exact(self):
        # FADD's and FFMA's TwoSum terms are NoContraction (a decoration valid on any arithmetic result), so a driver cannot fuse them into an
        # FMA; FMUL's error is one Fma of its own. spirv-val checks every module in translate
        for inst, precise in ((fadd(RM), True), (ffma(RZ), True), (ffma(RM, ftz=True), True), (fmul(RP), False), (fmul(RZ, scale=1), False)):
            with self.subTest(inst=hex(inst)):
                data, _ = translate([s2r_tid(1), s2r_tid(2), s2r_tid(3), inst, sts(4), EXIT])
                self.assertEqual(any(o == 71 and a[1] == 42 for o, a in instructions(data)), precise)

    def test_round_to_nearest_is_unchanged(self):
        data, _ = translate([mov(1, bits(1.0)), mov(2, bits(2.0 ** -30)), fadd(RN), sts(4), EXIT])
        self.assertEqual(run(data), [bits(1.0)])


if __name__ == '__main__':
    unittest.main()
