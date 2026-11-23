"""Switch tables, decoded from synthetic AArch64 instructions without game files."""
import ast, re, struct, sys, unittest
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/tools'
sys.path.insert(0, str(TOOLS))
from nso_asm import ADRP, ADDI, NOP


class AdrOffsetTables(unittest.TestCase):
    def setUp(self):
        tree = ast.parse((TOOLS / 'patch_nso.py').read_text(encoding='utf-8'))
        names = {'num', 'jump_adr_offsets', 'half_count', 'table_base', 'table_cands'}
        ns = {'re': re, 'struct': struct}
        exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names], type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), ns)
        self.rewriter = ns

    def disassemble(self, words, at):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), at))
        self.assertEqual(len(ins), len(words))
        return ins

    def recognize(self, *, wide=True, between=(), checked=9, truncate=False):
        at, table, base = 0x1000, 0x2080, 0x1010
        words = [ADRP(15, at, table), ADDI(15, 15, table & 0xfff),
                 (0xf100001f if wide else 0x7100001f) | 4 << 10 | checked << 5,  # cmp x9/w9, #4
                 0x540001a8, 0x10000012]                                       # b.hi; adr x18, .
        words.extend(between)
        words += [0xb8a979e0, 0x8b000252, 0xd61f0240]                           # ldrsw x0,[x15,x9,lsl #2]; add x18,x18,x0; br x18
        dispatch = len(words) - 1
        words += [NOP] * 16
        ins = self.disassemble(words, at)
        image = bytearray(table + 20)
        offsets = (-16, -12, 16, 32, 48)
        struct.pack_into('<5i', image, table, *offsets)
        self.rewriter['IMG'] = image[:-4] if truncate else image
        return self.rewriter['jump_adr_offsets'](ins, dispatch), [base + x for x in offsets]

    def halfword_count(self, compare, *, branch=0x54000168, between=(), truncate=False):
        at, table = 0x1000, 0x2080
        words = [ADRP(15, at, table), ADDI(15, 15, table & 0xfff), compare, branch, *between]
        load = len(words)
        words += [0x786979e0] + [NOP] * 16  # ldrh w0, [x15, x9, lsl #1]
        ins = self.disassemble(words, at)
        image = bytearray(table) + struct.pack('<5H', 1, 0, 8, 3, 4)
        self.rewriter['IMG'] = image[:-2] if truncate else image
        return self.rewriter['half_count'](ins, ins[load], at)

    def test_halfword_pad_count_uses_the_largest_entry(self):
        for compare in (0x7100113f, 0xf100113f):  # cmp w9 / x9, #4
            with self.subTest(compare=compare):
                self.assertEqual(self.halfword_count(compare), 9)
        # cmp x8, #4; b.hi; mov w9, w8
        self.assertEqual(self.halfword_count(0xf100111f, between=(0x2a0803e9,)), 9)

    def test_halfword_count_falls_back_when_the_bound_or_table_is_unusable(self):
        compare = 0xf100113f  # cmp x9, #4
        self.assertIsNone(self.halfword_count(compare, between=(ADDI(9, 9, 1),)))
        self.assertIsNone(self.halfword_count(compare, between=(0x2a0803e9,)))  # overwritten with unchecked w8
        self.assertIsNone(self.halfword_count(0xf100111f))  # cmp x8, but the load uses x9
        self.assertIsNone(self.halfword_count(compare, branch=0x54000160))  # b.eq
        self.assertIsNone(self.halfword_count(compare, truncate=True))

    def test_64_bit_bounds_check(self):
        got, want = self.recognize()
        self.assertEqual(got, want)

    def test_32_bit_bounds_check_keeps_its_targets(self):
        got, want = self.recognize(wide=False)
        self.assertEqual(got, want)

    def test_changed_or_unchecked_index_is_rejected(self):
        self.assertIsNone(self.recognize(between=(ADDI(9, 9, 1),))[0])
        self.assertIsNone(self.recognize(checked=8)[0])

    def test_bounds_follow_the_source_of_a_copied_index(self):
        copy = (0x2a0803e9,)  # mov w9, w8
        for wide in (False, True):
            with self.subTest(wide=wide):
                got, want = self.recognize(wide=wide, checked=8, between=copy)
                self.assertEqual(got, want)
                self.assertIsNone(self.recognize(wide=wide, checked=9, between=copy)[0])

    def test_truncated_table_is_rejected(self):
        self.assertIsNone(self.recognize(truncate=True)[0])


if __name__ == '__main__': unittest.main()
