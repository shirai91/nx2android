"""ADR-relative switch tables, decoded from synthetic AArch64 instructions without game files."""
import ast, re, struct, sys, unittest
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/tools'
sys.path.insert(0, str(TOOLS))
from nso_asm import ADRP, ADDI, NOP


class AdrOffsetTables(unittest.TestCase):
    def recognize(self, *, wide=True, clobber=False, checked=9, truncate=False):
        tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
        names = {'num', 'jump_adr_offsets', 'table_base', 'table_cands'}
        ns = {'re': re, 'struct': struct}
        exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names], type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), ns)
        at, table, base = 0x1000, 0x2080, 0x1010
        words = [ADRP(15, at, table), ADDI(15, 15, table & 0xfff),
                 (0xf100001f if wide else 0x7100001f) | 4 << 10 | checked << 5,  # cmp x9/w9, #4
                 0x540001a8, 0x10000012]                                       # b.hi; adr x18, .
        if clobber: words.append(ADDI(9, 9, 1))
        words += [0xb8a979e0, 0x8b000252, 0xd61f0240]                           # ldrsw x0,[x15,x9,lsl #2]; add x18,x18,x0; br x18
        dispatch = len(words) - 1
        words += [NOP] * 16
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), at))
        self.assertEqual(len(ins), len(words))
        image = bytearray(table + 20)
        offsets = (-16, -12, 16, 32, 48)
        struct.pack_into('<5i', image, table, *offsets)
        ns['IMG'] = image[:-4] if truncate else image
        return ns['jump_adr_offsets'](ins, dispatch), [base + x for x in offsets]

    def test_64_bit_bounds_check(self):
        got, want = self.recognize()
        self.assertEqual(got, want)

    def test_32_bit_bounds_check_keeps_its_targets(self):
        got, want = self.recognize(wide=False)
        self.assertEqual(got, want)

    def test_changed_or_unchecked_index_is_rejected(self):
        self.assertIsNone(self.recognize(clobber=True)[0])
        self.assertIsNone(self.recognize(checked=8)[0])

    def test_truncated_table_is_rejected(self):
        self.assertIsNone(self.recognize(truncate=True)[0])


if __name__ == '__main__': unittest.main()
