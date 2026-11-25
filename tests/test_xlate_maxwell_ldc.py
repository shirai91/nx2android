"""Maxwell LDC's 16-bit immediate (InstEmitMemory.Ldc), adapted from seedds/nxnative's test_dmsh_ldc_offset.py (MIT).

Added to a register the immediate is signed and the sum 32 bits, as nvdisasm prints it and Xenoblade Chronicles 2's pixel shaders use it
(c6 at R - 8 to R - 128). Alone (Ra = RZ) it is the address as it is, as Mortal Kombat 11 reads c[14][0x8448]: sign-extended, that read's word
index overflowed into the operand's slot field and became a read of c15 at word 33,552,452. A fixed word past the 64 KiB bank reads 0."""
import unittest

from maxwell_xlate import EXIT, instructions, mov, s2r_tid, stored, sts, translate, uniform_reads

LDC_U16 = 0xef9200e84487ff31      # Mortal Kombat 11's own encoding: LDC.U16 R49, c[14][0x8448]
LDC_64_NEG = 0xef95006ffc073b04   # Xenoblade Chronicles 2's own encoding (pixel shader 09030ba6b1d1283e): LDC.64 R4, c[6][R59 - 0x40]


def with_imm(inst, imm): return inst & ~(0xffff << 20) | imm << 20


class LdcOffset(unittest.TestCase):
    def reads(self, *insts):
        data, res = translate(list(insts))
        return uniform_reads(data), sorted(c['slot'] for c in res['cbufs']), data

    def test_the_game_read_alone_is_its_immediate(self):
        reads, slots, _ = self.reads(LDC_U16, sts(49), EXIT)
        self.assertEqual(slots, [14], 'main: c15')
        self.assertEqual(reads, [('cp_c14', [0, 2116, 2])])   # 0x8448 bytes = word 8466 = vector 2116, element .z

    def test_immediates_alone_across_the_sign_bit(self):
        for imm in (0x120, 0x7ffc, 0x8000, 0xfffc):
            with self.subTest(imm=hex(imm)):
                reads, slots, _ = self.reads(with_imm(LDC_U16, imm), sts(49), EXIT)
                self.assertEqual(slots, [14])
                self.assertEqual(reads, [('cp_c14', [0, imm // 16, imm // 4 & 3])])

    def test_added_to_a_register_the_immediate_is_signed(self):
        reads, slots, _ = self.reads(mov(59, 0x100), LDC_64_NEG, sts(4), sts(5, 4), EXIT)
        self.assertEqual(slots, [6])
        self.assertEqual(sorted(i for _, i in reads), [[0, 12, 0], [0, 12, 1]])   # 0x100 - 0x40 = 0xc0: word 48, vector 12

    def test_added_to_a_run_time_register_it_is_still_signed(self):
        reads, slots, data = self.reads(s2r_tid(59), LDC_64_NEG, sts(4), sts(5, 4), EXIT)
        self.assertEqual(slots, [6])
        consts = {a[2] for o, a in instructions(data) if o == 43 and len(a) == 3}
        self.assertIn(0xffffffc0, consts)      # R59 + -0x40
        self.assertNotIn(0xffc0, consts)       # 0xffc0 read unsigned: 64 KiB away

    def test_a_fixed_word_outside_the_bank_reads_zero(self):
        reads, slots, data = self.reads(mov(59, 0x10), with_imm(LDC_64_NEG, 0xffe0), sts(4), sts(5, 4), EXIT)   # c[6][0x10 - 0x20]
        self.assertEqual((reads, slots), ([], []))
        self.assertEqual(stored(data), [0, 0])


if __name__ == '__main__':
    unittest.main()
