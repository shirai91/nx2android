"""A hull shader that writes its control points straight into the stage's circular buffer (DirectCbeWrites in the translator), through the shader
translator as porter builds it, checked as Vulkan SPIR-V. The hull is assembled by hand in the form NVIDIA's compiler gives every such shader: the buffer's
address from system registers 21 and 22, the count of per-patch words and the stride from ISBERD with O at 0x7e and 0x7f, output word k of lane L at
address + 0x80 * (1 + ceil(N / 4) + p) + 4 * L + 4 * S * k (p by a predicated add), a copy of an output into the buffer after the body. The output words are
the header's output map: position xyzw, then generic attribute 0's x and y. Adapted from seedds/nxnative 2a40e528b, dm/gfx/tests/test_dmsh_hull_cbe.py (MIT),
which reads the MSL; here the SPIR-V must pass spirv-val for Vulkan and write gl_out[gl_InvocationID] (read back as GLSL with spirv-cross)."""
import json, re, shutil, struct, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PT, RZ = 7, 255
SCHED = 0x001f8000fc0007e0     # the scheduling word before each three instructions
NOP = 0x50b0000000070f00
EXIT = 0xe30000000007000f
SR_LANEID, SR_INVOCATION, SR_CBE_LO, SR_CBE_HI = 0, 17, 21, 22

def s2r(rd, sr): return 0xf0c8 << 48 | PT << 16 | sr << 20 | rd
def mov32i(rd, imm): return 0x010 << 52 | imm << 20 | 0xf << 12 | PT << 16 | rd
def mov(rd, rb): return 0x5c98 << 48 | 0xf << 39 | rb << 20 | PT << 16 | rd
def isberd(rd, ra, o=True): return 0xefd0 << 48 | int(o) << 32 | PT << 16 | ra << 8 | rd               # ISBERD rd, [ra] (O: the shader's own outputs)
def iadd32i(rd, ra, imm, guard=PT): return 0x1c0 << 52 | (imm & 0xffffffff) << 20 | guard << 16 | ra << 8 | rd
def iadd(rd, ra, rb): return 0x5c10 << 48 | rb << 20 | PT << 16 | ra << 8 | rd
def iadd3i(rd, ra, imm, rc): return 0x38c0 << 48 | rc << 39 | imm << 20 | PT << 16 | ra << 8 | rd           # IADD3 rd, ra, imm, rc
def shr(rd, ra, n): return 0x3828 << 48 | n << 20 | PT << 16 | ra << 8 | rd
def shl(rd, ra, n): return 0x3848 << 48 | n << 20 | PT << 16 | ra << 8 | rd
def xmadi(rd, ra, imm, rc): return 0x3600 << 48 | rc << 39 | imm << 20 | PT << 16 | ra << 8 | rd            # XMAD rd, ra, imm16, rc
def isetp_ne(a, b): return 0x5b6 << 52 | 5 << 49 | 1 << 48 | PT << 39 | b << 20 | PT << 16 | a << 8 | 0 << 3 | PT   # ISETP.NE.AND P0, PT, a, b, PT
def ald(rd, attr, vtx): return 0xefd8 << 48 | vtx << 39 | attr << 20 | PT << 16 | RZ << 8 | rd             # ALD.32 rd, a[attr], vtx
def ast_patch(attr, rs): return 0xeff0 << 48 | RZ << 39 | 1 << 31 | attr << 20 | PT << 16 | RZ << 8 | rs   # AST.P.32 a[attr], rs
def stg(ra, rd): return 0xeedc6 << 44 | PT << 16 | ra << 8 | rd                                           # STG.E.32 [ra], rd (the high word in ra + 1)

def code(*insts):
    out = b''
    for k in range(0, len(insts), 3):
        out += struct.pack('<4Q', SCHED, *(tuple(insts[k:k + 3]) + (NOP, NOP))[:3])
    return out

def header():
    h = bytearray(0x50)
    struct.pack_into('<5I', h, 0, 1 | 3 << 5 | 2 << 10 | 1 << 16 | 3 << 17 | 1 << 26, 6 << 24, 4 << 24, 0, 0)   # a tessellation control shader: 6 per-patch words, 4 control points
    h[0x17] = 0xf0      # inputs: position xyzw
    h[0x35] = 0xf0      # outputs: position xyzw ...
    h[0x36] = 0x03      # ... and generic attribute 0's x and y
    return bytes(h)

def hull(extra_term=False):
    body = [s2r(7, SR_LANEID), mov32i(2, 0x7e), mov32i(3, 0x7f), isberd(0, 2), isberd(4, 3), s2r(8, SR_CBE_LO), s2r(5, SR_CBE_HI),
            iadd32i(0, 0, 3), shr(9, 0, 2), shl(7, 7, 2), xmadi(6, 9, 0x80, RZ), iadd(10, 8, 6),
            s2r(12, SR_INVOCATION), isetp_ne(12, RZ), iadd32i(10, 10, 0x80, guard=0),                       # the predicated row (p)
            shl(4, 4, 2), iadd3i(24, 7, 0x80, 10), isberd(21, 12, o=False)]
    body += [ald(40 + k, 0x70 + 4 * k, 21) for k in range(4)] + [ald(44, 0x80, 21), ald(45, 0x84, 21)]
    for k in range(6):
        body += [xmadi(26, 4, k, RZ), iadd(30, 24, 26), mov(31, 5)]
        if extra_term and k == 5: body.append(iadd(30, 30, 12))                                               # an address that is not base + lane + constants
        body.append(stg(30, 40 + k))
    body += [iadd32i(1, 7, 0x7f), isberd(1, 1), iadd(30, 8, 7), mov(31, 5), stg(30, 1)]                     # the header word copied into row 0: dropped
    body += [ast_patch(4 * i, 40) for i in range(6)] + [EXIT]
    return header() + code(*body)


@unittest.skipUnless(shutil.which('dotnet'), 'the shader translator needs .NET')
class HullCircularBuffer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_hull_cbe_')
        cls.out = Path(cls.tmp.name)
        src = ROOT / 'nxnative/dm/gfx/tools/dmsh_xlate'
        subprocess.run(['dotnet', 'build', str(src), '-c', 'Release', '-o', str(cls.out / 'xlate'), '-nologo', '-v', 'quiet'], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def translate(self, key, blob):
        (self.out / f'{key}.bin').write_bytes(blob)
        jobs = self.out / f'{key}.json'
        jobs.write_text(json.dumps([dict(key=key, stage='hs', code=str(self.out / f'{key}.bin'), tex={}, img={}, attr={}, fout={}, debug=False)]), encoding='utf-8')
        r = subprocess.run(['dotnet', str(self.out / 'xlate/dmsh_xlate.dll'), str(jobs), str(self.out)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        spv = self.out / f'{key}.spv'
        self.assertTrue(spv.is_file(), r.stdout + r.stderr)
        if shutil.which('spirv-val'):
            v = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(spv)], capture_output=True, text=True)
            self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        if not shutil.which('spirv-cross'): self.skipTest('requires spirv-cross on PATH to read the SPIR-V back')
        g = subprocess.run(['spirv-cross', '--version', '450', str(spv)], capture_output=True, text=True)
        self.assertEqual(g.returncode, 0, g.stderr)
        return g.stdout

    @staticmethod
    def stores(glsl):
        """(output, component, source) of each per-control-point output store indexed by the invocation; source is the input word the value was read
        from ('x'..'w' of the position, 'gx' / 'gy' of generic attribute 0) or None"""
        invocation = set(re.findall(r'(_\d+) = intBitsToFloat\(gl_InvocationID\);', glsl))
        index = r'(?:gl_InvocationID|floatBitsToInt\((_\d+)\))'
        names = dict(re.findall(r'float (_\d+) = (.+);', glsl))
        def source(rhs):
            rhs = names.get(rhs, rhs)
            m = re.fullmatch(r'(gl_in|_\d+)\[[^\]]+\]\.(?:gl_Position\.)?([xyzw])', rhs)
            return None if not m else m.group(2) if m.group(1) == 'gl_in' else 'g' + m.group(2)
        out = []
        for m in re.finditer(rf'(gl_out|_\d+)\[{index}\]\.(gl_Position\.)?([xyzw]) = (.+);', glsl):
            if m.group(2) is None or m.group(2) in invocation:
                out.append(('position' if m.group(1) == 'gl_out' else 'generic', m.group(4), source(m.group(5))))
        return out

    def test_stores_become_the_control_points_outputs(self):
        glsl = self.translate('hull', hull())
        self.assertIn('layout(vertices = 4) out;', glsl)
        stores = self.stores(glsl)
        self.assertEqual(sorted((o, c) for o, c, _ in stores), sorted([('position', c) for c in 'xyzw'] + [('generic', 'x'), ('generic', 'y')]), glsl)
        self.assertRegex(glsl, r'layout\(location = 0\) out vec4 _\d+\[4\];')   # generic attribute 0, one per control point
        self.assertNotRegex(glsl, r'\bbuffer\b(?! *\w*support)|std430', 'no store left to a storage buffer')

    def test_each_word_gets_its_own_value(self):
        glsl = self.translate('hull_values', hull())
        got = {(o, c): v for o, c, v in self.stores(glsl)}
        self.assertEqual(got, {**{('position', c): c for c in 'xyzw'}, ('generic', 'x'): 'gx', ('generic', 'y'): 'gy'}, glsl)

    def test_an_address_with_another_term_is_not_guessed(self):
        glsl = self.translate('hull_extra', hull(extra_term=True))
        self.assertEqual(len(self.stores(glsl)), 5, glsl)   # generic y's address has the invocation in it: dropped
        self.assertNotRegex(glsl, r'std430')


if __name__ == '__main__':
    unittest.main()
