"""A size query takes its texture's type from the shader's other instructions on that texture (dmsh_xlate Accessor.TextureTypesOf), on synthetic
shaders only; adapted from seedds/nxnative's dm/tools/tests/test_txq_type.py (MIT, with its follow-up 379e885e1).

The handle of a bindless TXQ and of a TEX OR the same texture word in c[2] with different sampler words, so they are two bindings of one texture
slot, as in Princess Peach: Showtime!. With no answer-table row the query was typed 2D, a 2D binding of what the sample shows is a 3D texture."""
import unittest

from maxwell_xlate import base, translate

TEXTURE_2D, TEXTURE_3D, TEXTURE_CUBE, ARRAY = 3, 4, 5, 1 << 8   # SamplerType
DIM_2D, DIM_2D_ARRAY, DIM_3D, DIM_CUBE = 2, 3, 4, 6             # TexDim
SLOT_128, SLOT_129 = 0x168, 0x170                               # c[2] offsets of separate textures
SAMPLER_1, SAMPLER_6 = 0x570, 0x598                             # and of separate samplers


def ldc(rd, offset):                                            # LDC.32 Rd, c[2][offset], RZ
    return base('Ldc') | (4 << 48) | (2 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | rd


def handle(rd, texture, sampler):                               # Rd = c[2][texture] | c[2][sampler], and R10 + R11 as scratch
    return [ldc(10, texture), ldc(11, sampler), base('LopR') | (1 << 41) | (11 << 20) | (7 << 16) | (10 << 8) | rd]


def mov(rd, value):
    return base('Mov32i') | (value << 20) | (7 << 16) | (15 << 12) | rd


def query(rd, texture, sampler=SAMPLER_6):                      # TXQ.B Rd, width, handle in R4 and level in R5
    return handle(4, texture, sampler) + [mov(5, 0), base('TxqB') | (1 << 31) | (1 << 22) | (7 << 16) | (4 << 8) | rd]


def sample(rd, texture, dim, sampler=SAMPLER_1):                # TEX.B.LZ Rd, red, coordinates in R0.., handle in R6
    return handle(6, texture, sampler) + [base('TexB') | (1 << 37) | (1 << 31) | (dim << 28) | (6 << 20) | (7 << 16) | (0 << 8) | rd]


def keep(reg):                                                  # SUATOM.ADD on image slot 0: uses the register, so its read stays
    return base('Suatom') | (0x48 << 36) | (3 << 33) | (reg << 20) | (7 << 16) | 255


def shader(*parts):
    insts = [mov(0, 0), mov(1, 0), mov(2, 0)]
    for part in parts: insts += part if isinstance(part, list) else [part]
    insts.append(base('Exit') | (7 << 16) | 15)
    return insts


class QueryType(unittest.TestCase):
    def translate(self, insts, tex=None):
        """the texture bindings as {(c[2] texture word's offset, sampler word's offset): type}, and the translator's notes"""
        _, res = translate(insts, tex=tex or {}, img={'0': dict(type='2d', fmt='R32Uint')}, local=[1, 1, 1])
        return {((t['handle'] & 0x3fff) * 4, ((t['handle'] >> 14) & 0x3fff) * 4): t['type'] & (0xff | ARRAY) for t in res['textures']}, res['notes']

    def test_query_takes_the_type_of_the_sample(self):
        for dim, want in ((DIM_3D, TEXTURE_3D), (DIM_CUBE, TEXTURE_CUBE), (DIM_2D_ARRAY, TEXTURE_2D | ARRAY)):
            with self.subTest(dim=dim):
                textures, notes = self.translate(shader(query(8, SLOT_128), keep(8), sample(9, SLOT_128, dim), keep(9)))
                self.assertEqual(textures, {(SLOT_128, SAMPLER_6): want, (SLOT_128, SAMPLER_1): want})
                self.assertTrue(any("from the shader's other instructions" in n for n in notes), notes)

    def test_the_order_of_the_instructions_does_not_matter(self):
        textures, _ = self.translate(shader(sample(9, SLOT_128, DIM_3D), keep(9), query(8, SLOT_128), keep(8)))
        self.assertEqual(textures, {(SLOT_128, SAMPLER_6): TEXTURE_3D, (SLOT_128, SAMPLER_1): TEXTURE_3D})

    def test_a_query_with_nothing_else_on_its_slot_stays_2d(self):
        textures, notes = self.translate(shader(query(8, SLOT_128), keep(8)))
        self.assertEqual(textures, {(SLOT_128, SAMPLER_6): TEXTURE_2D})
        self.assertTrue(any('using 2D' in n for n in notes), notes)

    def test_a_sample_of_another_slot_gives_no_type(self):
        textures, _ = self.translate(shader(query(8, SLOT_128), keep(8), sample(9, SLOT_129, DIM_3D), keep(9)))
        self.assertEqual(textures, {(SLOT_128, SAMPLER_6): TEXTURE_2D, (SLOT_129, SAMPLER_1): TEXTURE_3D})

    def test_a_2d_sample_changes_nothing(self):
        textures, notes = self.translate(shader(query(8, SLOT_128), keep(8), sample(9, SLOT_128, DIM_2D), keep(9)))
        self.assertEqual(textures, {(SLOT_128, SAMPLER_6): TEXTURE_2D, (SLOT_128, SAMPLER_1): TEXTURE_2D})
        self.assertFalse(any('other instructions' in n for n in notes), notes)

    def test_two_shapes_on_one_slot_keep_the_guess(self):
        textures, notes = self.translate(shader(query(8, SLOT_128), keep(8), sample(9, SLOT_128, DIM_3D), keep(9),
                                                sample(9, SLOT_128, DIM_CUBE, SAMPLER_6 + 8), keep(9)))
        self.assertEqual(textures[(SLOT_128, SAMPLER_6)], TEXTURE_2D)
        self.assertFalse(any('other instructions' in n for n in notes), notes)

    def test_the_answer_table_comes_first(self):
        textures, notes = self.translate(shader(query(8, SLOT_128), keep(8), sample(9, SLOT_128, DIM_3D), keep(9)), tex={'128': 'cube'})
        self.assertEqual(textures[(SLOT_128, SAMPLER_6)], TEXTURE_CUBE)
        self.assertFalse(any('other instructions' in n for n in notes), notes)


if __name__ == '__main__':
    unittest.main()
