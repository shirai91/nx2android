"""nxnative/dm/tools/patch_nso.py's search for exclusive loads and stores without ordering (exclusive_order_sites), on words assembled by clang, without game files."""
import ast
import os
import subprocess
import sys
import tempfile
from pathlib import Path
import re
import struct
from typing import Any
import unittest

REWRITER = Path(__file__).resolve().parents[1] / 'nxnative/dm/tools/patch_nso.py'

tree = ast.parse(REWRITER.read_text(encoding='utf-8'))   # the program loads and rewrites an image: compile only the search
ns: dict[str, Any] = {'struct': struct, 're': re}
exec(compile(ast.Module([n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'exclusive_order_sites'], []), str(REWRITER), 'exec'), ns)
find = ns['exclusive_order_sites']

def img(*w): return b''.join(struct.pack('<I', x) for x in w)
NOP = 0xD503201F
# clang -target arm64-apple-macos: each without ordering, the same with it (bit 15)
UNORDERED = {'ldxrb w1,[x2]': 0x085F7C41, 'ldxrh w1,[x2]': 0x485F7C41, 'ldxr w1,[x2]': 0x885F7C41, 'ldxr x1,[x2]': 0xC85F7C41,
             'ldxp w1,w3,[x2]': 0x887F0C41, 'ldxp x1,x3,[x2]': 0xC87F0C41,
             'stxrb w4,w1,[x2]': 0x08047C41, 'stxrh w4,w1,[x2]': 0x48047C41, 'stxr w4,w1,[x2]': 0x88047C41, 'stxr w4,x1,[x2]': 0xC8047C41,
             'stxp w4,w1,w3,[x2]': 0x88240C41, 'stxp w4,x1,x3,[x2]': 0xC8240C41}
OTHERS = {'ldaxr w1,[x2]': 0x885FFC41, 'stlxp w4,x1,x3,[x2]': 0xC8248C41, 'cas w4,w1,[x2]': 0x88A47C41, 'casp w4,w5,w0,w1,[x2]': 0x08247C40,
          'casp x4,x5,x0,x1,[x2]': 0x48247C40, 'ldar w1,[x2]': 0x88DFFC41, 'stlr w1,[x2]': 0x889FFC41,
          'ldxr w1,[x2] with Rt2 not all ones (unallocated)': 0x885F0041, 'nop': NOP}

class ExclusiveOrder(unittest.TestCase):
    def test_every_unordered_form_found(self):
        for name, w in UNORDERED.items():
            code = img(NOP, w, NOP)
            self.assertEqual(find(code, [(0, len(code))]), [4], name)
            self.assertEqual(w | 0x8000, w + 0x8000, name)   # bit 15 is clear: setting it gives the ordered form

    def test_ordered_and_other_atomics_left_alone(self):
        for name, w in OTHERS.items():
            code = img(NOP, w, NOP)
            self.assertEqual(find(code, [(0, len(code))]), [], name)

    def test_a_whole_loop(self):   # Breath of the Wild 1.6.0 main+0x13a0784: ldxr w9,[x23]; cbnz w9; stxr w9,w8,[x23]; cbnz w9: the load and the store
        code = img(0x885F7EE9, 0x35FFFFE9, 0x88097EE8, 0x35FFFFA9)
        self.assertEqual(find(code, [(0, len(code))]), [0, 8])

    def test_only_aligned_words(self):   # the forms' top byte at another position is not an instruction
        code = bytes([0x41, 0x7C, 0x5F, 0x88, 0x00]) + img(NOP, NOP)[:7]
        self.assertEqual(find(code, [(0, len(code))]), [0])
        shifted = b'\x00\x00' + img(UNORDERED['stxr w4,w1,[x2]']) + b'\x00\x00'
        self.assertEqual(find(shifted, [(0, len(shifted))]), [])

    def test_ranges_are_respected(self):   # data blocks between code ranges
        w = UNORDERED['ldxr x1,[x2]']
        code = img(w, NOP, w, NOP, w)
        self.assertEqual(find(code, [(0, 4), (8, 12)]), [0, 8])
        self.assertEqual(find(code, [(4, 8), (12, 16)]), [])
        self.assertEqual(find(code, [(16, 20)]), [16])


class Placement(unittest.TestCase):
    """The rule is not gated by DM_TARGET (Android's cores reorder the same way), and it runs before the windows are found and copied, so their copies carry the ordered forms"""
    def test_runs_for_every_target_before_the_windows(self):
        src = REWRITER.read_text(encoding='utf-8')
        at = src.index('\nORDERED = exclusive_order_sites(IMG, CODE_RANGES)')
        self.assertLess(src.index('\nCODE_RANGES = '), at)
        self.assertLess(at, src.index('ws = find_windows(insl)'))
        self.assertLess(at, src.index('\nx18 = set()'))
        self.assertLess(src.index('trinity = trinity_completer_sites(IMG, CODE_RANGES)'), src.index('ws = find_windows(insl)'))
        stmt = next(n for n in ast.parse(src).body if isinstance(n, ast.Assign) and any(isinstance(x, ast.Name) and x.id == 'ORDERED' for x in n.targets))
        self.assertEqual(stmt.col_offset, 0)   # top level, under no target condition


class AndroidRewrite(unittest.TestCase):
    """The whole rewriter with DM_TARGET=android on a synthetic module (its text is 0x40 bytes, too small for Trinity's completer): the exclusive pair gains its ordering, next to an
    Android thread-slot site"""
    def test_android_output(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_provisional_patch_warning import fixture_nso
        with tempfile.TemporaryDirectory(prefix='nx2a_exclusive_order_') as tmp:
            src, out = Path(tmp) / 'main', Path(tmp) / 'out'
            fixture_nso(src)
            d = bytearray(src.read_bytes())   # the text starts at file offset 0x100
            struct.pack_into('<III', d, 0x100 + 0x14, UNORDERED['ldxr w1,[x2]'], UNORDERED['stxr w4,w1,[x2]'], 0xD53BD040)   # ...; mrs x0,tpidr_el0
            src.write_bytes(d)
            env = dict(os.environ, DM_TARGET='android'); env.pop('DM_PATCHES_EXTRA', None)
            r = subprocess.run([sys.executable, str(REWRITER), str(src), str(out), 'main'], env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("'exclusive_ordered': 2", r.stdout)
            self.assertIn("'tpidr_r': 1", r.stdout)
            b = (out / 'main.patched.bin').read_bytes()
            self.assertEqual(struct.unpack_from('<2I', b, 0x14), (0x885FFC41, 0x8804FC41))   # ldaxr w1,[x2]; stlxr w4,w1,[x2]


if __name__ == '__main__':
    unittest.main()
