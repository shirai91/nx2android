"""Reads past a uniform binding's size give 0 (TranslationFlags.UniformBounds, adapted from seedds/nxnative): synthetic Maxwell compute shaders through
the real translator. A read of a game uniform slot at a fixed offset stays a plain load and is reported as the slot's "fixed" bytes; a read at a computed
offset compares its row with the size table in c[25] and gives 0 past it; without the job's uniform_bounds nothing changes. spirv-val (on PATH) checks the
modules."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Maxwell encodings, as seedds/nxnative's test_dmsh_ldc_offset.py builds them
PT, RZ = 7, 255
SCHED = 0x001f8000fc0007e0     # the scheduling word before each three instructions
NOP = 0x50b0000000070f00
EXIT = 0xe30000000007000f
LDC_U16 = 0xef9200e84487ff31   # LDC.U16 R49, c[14][0x8448] (Mortal Kombat 11)
LDC_64_NEG = 0xef95006ffc073b04   # LDC.64 R4, c[6][R59 - 0x40] (Xenoblade Chronicles 2)


def op(top, shift, **fields):
    bit = dict(rd=0, a=8)
    v = top << shift | PT << 16
    for k, value in fields.items(): v |= value << bit[k]
    return v


def sts(rd, offset): return op(0xef58, 48, rd=rd, a=RZ) | 4 << 48 | offset << 20   # STS.B32 [RZ + offset], rd: keeps the result
def s2r_tid_x(rd): return op(0xf0c8, 48, rd=rd) | 0x21 << 20   # S2R rd, SR_TID.X: a value only known at run time
def with_imm(inst, imm): return inst & ~(0xffff << 20) | imm << 20


def code(*insts):
    out = b''
    for k in range(0, len(insts), 3):
        out += struct.pack('<4Q', SCHED, *(tuple(insts[k:k + 3]) + (NOP, NOP))[:3])
    return out


FIXED = (with_imm(LDC_U16, 0x120), sts(49, 0), EXIT)                          # c[14][0x120] alone
COMPUTED = (with_imm(LDC_U16, 0x120), sts(49, 0), s2r_tid_x(59), LDC_64_NEG, sts(4, 4), sts(5, 8), EXIT)   # and c[6][tid.x - 0x40]


def instructions(data):
    words = struct.unpack(f'<{len(data) // 4}I', data)
    out, i = [], 5
    while i < len(words):
        n = words[i] >> 16
        if not n: raise AssertionError('zero-length SPIR-V instruction')
        out.append((words[i] & 0xffff, words[i + 1:i + n])); i += n
    return out


def name(args):
    b = struct.pack(f'<{len(args) - 1}I', *args[1:]); return b[:b.index(b'\0')].decode()


@unittest.skipUnless(shutil.which('dotnet'), 'the shader translator needs .NET')
class TranslatorUniformBounds(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_ub_'); cls.out = Path(cls.tmp.name)
        subprocess.run(['dotnet', 'build', str(ROOT / 'nxnative/dm/gfx/tools/dmsh_xlate'), '-c', 'Release', '-o', str(cls.out / 'xlate'), '-nologo', '-v', 'quiet'],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def translate(self, key, insts, bounds=True):
        (self.out / f'{key}.bin').write_bytes(code(*insts))
        job = dict(key=key, stage='cs', code=str(self.out / f'{key}.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], shared=0x1000,
                   debug=False, reduced=True, uniform_bounds=bounds)
        jobs = self.out / f'{key}.json'; jobs.write_text(json.dumps([job]), encoding='utf-8')
        r = subprocess.run(['dotnet', str(self.out / 'xlate/dmsh_xlate.dll'), str(jobs), str(self.out)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        spv = (self.out / f'{key}.spv').read_bytes()
        if shutil.which('spirv-val'):
            v = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(self.out / f'{key}.spv')], capture_output=True, text=True)
            self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        res = json.loads((self.out / f'{key}.res.json').read_text())
        return instructions(spv), {c['slot']: c for c in res['cbufs']}

    def test_a_fixed_read_is_a_plain_load_counted_for_the_host(self):
        ins, cbufs = self.translate('fixed', FIXED)
        self.assertEqual(cbufs[14]['fixed'], 0x124)   # to the end of the word at 0x120
        self.assertNotIn(25, cbufs)                    # no size table without a computed read
        self.assertFalse(any(o == 176 for o, _ in ins), 'no OpULessThan')
        self.assertFalse(any(o == 169 for o, _ in ins), 'no OpSelect')

    def test_a_computed_read_compares_its_row_with_the_size_table(self):
        ins, cbufs = self.translate('computed', COMPUTED)
        self.assertEqual(cbufs[14]['fixed'], 0x124)
        self.assertEqual(cbufs[6]['fixed'], 0)
        self.assertIn(25, cbufs)
        names = {a[0]: name(a) for o, a in ins if o == 5}   # OpName
        table = next(i for i, n in names.items() if n.endswith('_c25'))
        consts = {a[1]: a[2] for o, a in ins if o == 43 and len(a) == 3}   # OpConstant
        chains = [a for o, a in ins if o == 65 and a[2] == table]   # OpAccessChain into the table
        # slot c[6] is the table's index 3: data[0].w
        self.assertTrue(chains and all([consts[x] for x in c[3:]] == [0, 0, 3] for c in chains), chains)
        shifts = [a for o, a in ins if o == 194 and consts.get(a[3]) == 4]   # OpShiftRightLogical size >> 4: rows of 16 bytes
        less = [a for o, a in ins if o == 176]   # OpULessThan
        self.assertEqual(len(less), 2, 'one comparison for each word of the LDC.64')
        self.assertTrue(all(any(l[3] == s[1] for s in shifts) for l in less), 'compared with the size in rows')
        selects = [a for o, a in ins if o == 169]   # OpSelect: the row index (0 when out), then the value (0 when out)
        self.assertEqual(len(selects), 4)

    def test_without_the_flag_nothing_is_bounded(self):
        ins, cbufs = self.translate('plain', COMPUTED, bounds=False)
        self.assertNotIn(25, cbufs)
        self.assertEqual(cbufs[14]['fixed'], 0)
        self.assertFalse(any(o in (169, 176) for o, _ in ins))


if __name__ == '__main__':
    unittest.main()
