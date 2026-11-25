"""Maxwell PRMT (byte permute) in its four encodings, translated to SPIR-V (adapted from seedds/nxnative's test_dmsh_maxwell_prmt.py, MIT).

The byte and sign oracle is NVIDIA PTX's prmt table. The inputs are MOV32I literals, so the translator folds each PRMT to the word it stores;
before the fix every form left Rd unchanged (0xdeadbeef here)."""
import struct
import unittest

from maxwell_xlate import EXIT, RZ, mov, s2r_tid, stored, sts, translate

# PTX table, low output byte first; specialized modes never sign-extend.
SELECT = (
    (),
    ((0, 1, 2, 3), (1, 2, 3, 4), (2, 3, 4, 5), (3, 4, 5, 6)),
    ((0, 7, 6, 5), (1, 0, 7, 6), (2, 1, 0, 7), (3, 2, 1, 0)),
    ((0, 0, 0, 0), (1, 1, 1, 1), (2, 2, 2, 2), (3, 3, 3, 3)),
    ((0, 1, 2, 3), (1, 1, 2, 3), (2, 2, 2, 3), (3, 3, 3, 3)),
    ((0, 0, 0, 0), (0, 1, 1, 1), (0, 1, 2, 2), (0, 1, 2, 3)),
    ((0, 1, 0, 1), (2, 3, 2, 3), (0, 1, 0, 1), (2, 3, 2, 3)),
)
A, B, OLD = 0x807f0180, 0x55aa00ff, 0xdeadbeef


def expected(a, selector, b, mode):
    src = struct.pack('<II', a, b)
    picks = SELECT[mode][selector & 3] if mode else [(selector >> (4 * i)) & 15 for i in range(4)]
    out = bytes((255 if src[s & 7] & 128 else 0) if s & 8 else src[s] for s in picks)
    return int.from_bytes(out, 'little')


def prmt(form, mode, rd=4, a=1, selector=2, b=3, pred=7, inv=False):
    # PRMT Rd, Ra, selector, Rb. Rc puts the selector in bit 39 and the data in c[3][20].
    top = {'r': 0x5bc, 'i': 0x36c, 'c': 0x4bc, 'rc': 0x53c}[form] << 52
    fields = rd | a << 8 | pred << 16 | int(inv) << 19 | mode << 48
    if form == 'r': fields |= selector << 20 | b << 39
    elif form == 'i': fields |= (selector & 0x7ffff) << 20 | (selector & 0x80000) << 37 | b << 39
    elif form == 'c': fields |= 3 << 34 | 5 << 20 | b << 39
    else: fields |= 3 << 34 | 5 << 20 | selector << 39
    return top | fields


class MaxwellPrmt(unittest.TestCase):
    def folded(self, form, mode, sel, rd=4, a=1, b=3, pred=7, inv=False):
        insts = [mov(1, A), mov(2, sel), mov(3, B), mov(4, OLD), prmt(form, mode, rd, a, sel if form == 'i' else 2, b, pred, inv), sts(rd), EXIT]
        values = stored(translate(insts)[0])
        self.assertEqual(len(values), 1, values)
        self.assertIsNotNone(values[0], 'the PRMT of literals did not fold')
        return values[0]

    def test_generic_literals_and_sign_replication(self):
        for form in ('r', 'i'):
            for sel, want in ((0x3210, A), (0x7654, B), (0x5410, 0x00ff0180), (0xba98, 0xff0000ff), (0xfedc, 0x00ff00ff), (0xffff, 0)):
                with self.subTest(form=form, selector=hex(sel)):
                    self.assertEqual(self.folded(form, 0, sel), want)
                    self.assertEqual(want, expected(A, sel, B, 0))

    def test_specialized_modes_and_high_selector_bits(self):
        for form in ('r', 'i'):
            for mode in range(1, 7):
                for sel in range(4):
                    with self.subTest(form=form, mode=mode, selector=sel):
                        self.assertEqual(self.folded(form, mode, sel | 0xfffc), expected(A, sel, B, mode))

    def test_signed_immediate_only_uses_low_selector_bits(self):
        self.assertEqual(self.folded('i', 0, 0xfffff), 0)
        self.assertEqual(self.folded('i', 1, 0x80003), 0xaa00ff80)

    def test_destination_aliases_sources(self):
        for rd in (1, 2, 3):
            with self.subTest(rd=rd): self.assertEqual(self.folded('r', 0, 0x5410, rd=rd), 0x00ff0180)

    def test_zero_register_sources_and_false_guard(self):
        self.assertEqual(self.folded('r', 0, 0x3210, a=RZ), 0)
        self.assertEqual(self.folded('r', 0, 0x7654, b=RZ), 0)
        self.assertEqual(self.folded('r', 0, 0x5410, inv=True), OLD)   # @!PT: not executed

    def test_constant_buffer_forms_translate(self):
        # c[3][20] is not a literal, so only the translation and its validity are checked here
        for form in ('c', 'rc'):
            for mode in range(7):
                with self.subTest(form=form, mode=mode):
                    data, res = translate([s2r_tid(1), s2r_tid(2), s2r_tid(3), prmt(form, mode), sts(4), EXIT])
                    self.assertEqual([c['slot'] for c in res['cbufs']], [3])
                    self.assertEqual(stored(data), [None])

    def test_reserved_modes_are_refused(self):
        for form in ('r', 'i', 'c', 'rc'):
            for mode in (7, 8, 15):
                with self.subTest(form=form, mode=mode):
                    translate([s2r_tid(1), s2r_tid(2), s2r_tid(3), prmt(form, mode), sts(4), EXIT], refused=True)


if __name__ == '__main__':
    unittest.main()
