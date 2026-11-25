"""Maxwell I2F of a 32-bit integer with .RM, .RP or .RZ (adapted from seedds/nxnative 645d3029d's Ampere rule and its DirectedRounding test, MIT).

The compiler's 32-bit integer division starts with I2F.U32.RP, so that its reciprocal estimate stays on the side its correction step expects;
rounded to nearest, an integer above 2^24 came out one ulp off about half the time. The inputs are MOV32I literals; maxwell_xlate.run executes
what the translator does not fold, against the exact value rounded by the mode."""
import random
import unittest

from maxwell_xlate import EXIT, base, f32_round, mov, run, sts, translate

MODE = {0: 'rn', 1: 'rm', 2: 'rp', 3: 'rz'}


def i2f(rm, signed):   # I2F.F32.{S32,U32}.<rm> R4, R1
    return base('I2fR') | 4 | 7 << 16 | 1 << 20 | 2 << 8 | 2 << 10 | int(signed) << 13 | rm << 39


def s32(x): return x - (1 << 32) if x & 0x80000000 else x


class I2fDirected(unittest.TestCase):
    def convert(self, rm, signed, v):
        values = run(translate([mov(1, v), i2f(rm, signed), sts(4), EXIT])[0])
        self.assertEqual(len(values), 1, values)
        return values[0]

    def check(self, rm, signed, v):
        want = f32_round(s32(v) if signed else v, MODE[rm])
        self.assertEqual(hex(self.convert(rm, signed, v)), hex(want), f'I2F.{"S32" if signed else "U32"}.{MODE[rm]} {v:#x}')

    def test_values_above_2_to_24(self):
        for v in ((1 << 24) + 1, (1 << 24) + 3, 0x7fffffff, 0x80000001, 0xffffffff, 0xfffffff0, 0x12345679):
            for rm in range(4):
                for signed in (False, True):
                    with self.subTest(v=hex(v), mode=MODE[rm], signed=signed):
                        self.check(rm, signed, v)

    def test_random_values(self):
        rng = random.Random(1411)
        for _ in range(24):
            v = rng.getrandbits(32)
            for rm in (1, 2, 3):
                for signed in (False, True):
                    with self.subTest(v=hex(v), mode=MODE[rm], signed=signed):
                        self.check(rm, signed, v)

    def test_exact_values_are_unchanged(self):
        for v in (0, 1, 1 << 24, 0x80000000, 0xff000000):
            for rm in (1, 2, 3):
                for signed in (False, True):
                    with self.subTest(v=hex(v), mode=MODE[rm], signed=signed):
                        self.check(rm, signed, v)

    def test_unsigned_round_up_past_2_to_32_is_2_to_32(self):
        self.assertEqual(self.convert(2, False, 0xffffffff), 0x4f800000)   # 2^32: no float of 32 bits lies between
        self.assertEqual(self.convert(3, False, 0xffffffff), 0x4f7fffff)


if __name__ == '__main__':
    unittest.main()
