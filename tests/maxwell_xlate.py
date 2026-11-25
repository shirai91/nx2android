"""Synthetic Maxwell programs through the shader translator (nxnative/dm/gfx/tools/dmsh_xlate), for the translator tests.

translator() builds dmsh_xlate once per test run with dotnet, as porter does (NX2A_TEST_XLATE=<dmsh_xlate.dll> takes another build instead). translate() writes one compute job, runs it, checks the SPIR-V with
spirv-val (vulkan1.1) where that tool is installed, and returns the module and its reflection. stored() lists the constants a module stores into
shared memory: with the translator's optimizer on, a program whose inputs are MOV32I literals folds to the stores of its results, which is how the
tests read an instruction's value without a GPU."""
import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PT, RZ = 7, 255
SCHED = 0x001f8000fc0007e0     # the scheduling word before each three instructions
NOP = 0x50b0000000070f00
EXIT = 0xe30000000007000f
SR_TID_X = 0x21

_build = {}


def translator():
    """dmsh_xlate.dll built from this checkout (once per process); skips the test without dotnet"""
    if not shutil.which('dotnet'): raise unittest.SkipTest('the shader translator needs .NET')
    if os.environ.get('NX2A_TEST_XLATE'): return Path(os.environ['NX2A_TEST_XLATE'])   # another build, e.g. main's, to see a test fail there
    if 'dll' not in _build:
        _build['tmp'] = tempfile.TemporaryDirectory(prefix='nx2a_maxwell_xlate_')
        out = Path(_build['tmp'].name) / 'xlate'
        r = subprocess.run(['dotnet', 'build', str(ROOT / 'nxnative/dm/gfx/tools/dmsh_xlate'), '-c', 'Release', '-o', str(out), '-nologo', '-v', 'quiet'],
                           capture_output=True, text=True)
        if r.returncode: raise AssertionError('dmsh_xlate build failed\n' + r.stdout[-4000:] + r.stderr[-4000:])
        _build['dll'] = out / 'dmsh_xlate.dll'
    return _build['dll']


def code(*insts):
    out = b''
    for k in range(0, len(insts), 3):
        out += struct.pack('<4Q', SCHED, *(tuple(insts[k:k + 3]) + (NOP, NOP))[:3])
    return out


def op(top, shift, **fields):
    bit = dict(rd=0, a=8, b=20, guard=16)
    v = top << shift | PT << 16
    for k, value in fields.items(): v |= value << bit[k]
    return v


def mov(rd, value): return 0x010000000007f000 | rd | (value & 0xffffffff) << 20             # MOV32I rd, value
def s2r_tid(rd): return op(0xf0c8, 48) | SR_TID_X << 20 | rd                                # S2R rd, SR_TID.X: a value nothing folds
def sts(rd, offset=0, a=RZ): return op(0xef5c, 48, rd=rd, a=a) | (offset & 0xffffff) << 20  # STS [a + offset], rd


def translate(insts, test=None, refused=False, stage='cs', **job):
    """(SPIR-V bytes, reflection) of a compute shader; refused: the translator must fail it (returns its output)"""
    dll = translator()
    with tempfile.TemporaryDirectory(prefix='nx2a_maxwell_') as d:
        work = Path(d)
        (work / 'k.bin').write_bytes(insts if isinstance(insts, bytes) else code(*insts))
        spec = dict(key='k', stage=stage, code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=False, reduced=True, shared=0x1000)
        spec.update(job)
        (work / 'jobs.json').write_text(json.dumps([spec]), encoding='utf-8')
        r = subprocess.run(['dotnet', str(dll), str(work / 'jobs.json'), d], capture_output=True, text=True)
        out = r.stdout + r.stderr
        if refused:
            if (work / 'k.spv').exists(): raise AssertionError('translated, expected a refusal\n' + out)
            return out
        if not (work / 'k.spv').exists(): raise AssertionError('not translated\n' + out)
        data = (work / 'k.spv').read_bytes()
        res = json.loads((work / 'k.res.json').read_text(encoding='utf-8'))
        if shutil.which('spirv-val'):
            v = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(work / 'k.spv')], capture_output=True, text=True)
            if v.returncode: raise AssertionError('spirv-val: ' + v.stdout + v.stderr)
        return data, res


def instructions(data):
    words = struct.unpack(f'<{len(data) // 4}I', data)
    out, i = [], 5
    while i < len(words):
        n, o = words[i] >> 16, words[i] & 0xffff
        if not n: raise AssertionError('zero-length SPIR-V instruction')
        out.append((o, words[i + 1:i + n])); i += n
    return out


def stored(data):
    """the constants stored into Workgroup memory, in program order (None for a store of a computed value)"""
    ins = instructions(data)
    consts = {a[1]: a[2] for o, a in ins if o == 43 and len(a) == 3}             # OpConstant (32-bit)
    shared = {a[1] for o, a in ins if o == 59 and a[2] == 4}                      # OpVariable Workgroup
    ptrs = set(shared) | {a[1] for o, a in ins if o == 65 and a[2] in shared}     # OpAccessChain into one
    for o, a in ins:
        if o == 124 and a[2] in consts: consts[a[1]] = consts[a[2]]               # OpBitcast of a constant
    return [consts.get(a[1]) for o, a in ins if o == 62 and a[0] in ptrs]


def call_args(data):
    """the arguments of each OpFunctionCall, as the constants stored into its argument variables (None where not a constant)"""
    ins = instructions(data)
    consts = {a[1]: a[2] for o, a in ins if o == 43 and len(a) == 3}
    last, calls = {}, []
    for o, a in ins:
        if o == 62: last[a[0]] = consts.get(a[1])
        elif o == 57: calls.append([last.get(p) for p in a[3:]])
    return calls


_TABLE = {}


def base(name):
    """the fixed bits of an emitter's form (FaddR) or an instruction's first form (Ldg), from the decoder's InstTable.cs"""
    import re
    if not _TABLE: _TABLE['src'] = (ROOT / 'nxnative/dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs').read_text(encoding='utf-8')
    found = (re.search(r'Add\("([01x]{64})", InstName\.\w+\s*,\s*InstEmit\.' + name + r'\s*,', _TABLE['src'])   # an emitter, e.g. FaddR
             or re.search(r'Add\("([01x]{64})", InstName\.' + name + r'\s*,', _TABLE['src']))                  # or an instruction's first form
    if not found: raise AssertionError(f'{name} is not in InstTable.cs')
    return int(found[1].replace('x', '0'), 2)


def uniform_reads(data):
    """(variable name, constant indices or None) of each access chain into a Uniform variable, e.g. ('cp_c14', [0, 2116, 2])"""
    ins = instructions(data)
    consts = {a[1]: a[2] for o, a in ins if o == 43 and len(a) == 3}
    names = {}
    for o, a in ins:
        if o == 5: names[a[0]] = struct.pack(f'<{len(a) - 1}I', *a[1:]).split(b'\0', 1)[0].decode()
    uniform = {a[1] for o, a in ins if o == 59 and a[2] == 2}
    return [(names.get(a[2], str(a[2])), [consts.get(i) for i in a[3:]]) for o, a in ins if o == 65 and a[2] in uniform]


def f32_round(x, mode='rn'):
    """the binary32 bits of the exact rational x (a Fraction) rounded by mode: 'rn' (to nearest even), 'rm', 'rp', 'rz'"""
    from fractions import Fraction
    import math
    if x == 0: return 0
    sign = 0x80000000 if x < 0 else 0
    a = abs(Fraction(x))
    e = max(a.numerator.bit_length() - a.denominator.bit_length() - 1, -126)
    while Fraction(2) ** (e + 1) <= a: e += 1
    while e > -126 and Fraction(2) ** e > a: e -= 1
    m = a / Fraction(2) ** (e - 23)                     # the significand in ulps, [2^23, 2^24) for a normal
    low = math.floor(m)
    up = {'rn': m - low > Fraction(1, 2) or (m - low == Fraction(1, 2) and low & 1), 'rz': False,
          'rm': bool(sign) and m != low, 'rp': not sign and m != low}[mode]
    n = low + int(up)
    if n >= 1 << 24: n >>= 1; e += 1
    if e > 127:   # overflow: infinity, or the largest finite value where the mode rounds toward zero
        return sign | (0x7f800000 if mode == 'rn' or (mode == 'rm' and sign) or (mode == 'rp' and not sign) else 0x7f7fffff)
    if n < 1 << 23: return sign | n                     # subnormal
    return sign | (e + 127) << 23 | (n - (1 << 23))


def f32_value(bits):
    """the exact value of finite binary32 bits, as a Fraction"""
    from fractions import Fraction
    e, m = bits >> 23 & 0xff, bits & 0x7fffff
    v = Fraction(m, 1 << 149) if e == 0 else Fraction((1 << 23) | m) * Fraction(2) ** (e - 150)
    return -v if bits >> 31 else v


_INT_OPS = {128: lambda p, q: p + q, 130: lambda p, q: p - q, 132: lambda p, q: p * q, 199: lambda p, q: p & q, 197: lambda p, q: p | q,
            198: lambda p, q: p ^ q, 196: lambda p, q: p << (q & 31), 194: lambda p, q: p >> (q & 31), 195: lambda p, q: _s32(p) >> (q & 31)}
_INT_COMPARES = {170: lambda p, q: p == q, 171: lambda p, q: p != q, 172: lambda p, q: p > q, 173: lambda p, q: _s32(p) > _s32(q),
                 174: lambda p, q: p >= q, 175: lambda p, q: _s32(p) >= _s32(q), 176: lambda p, q: p < q, 177: lambda p, q: _s32(p) < _s32(q),
                 178: lambda p, q: p <= q, 179: lambda p, q: _s32(p) <= _s32(q)}
_FLOAT_COMPARES = {180: '==', 182: '!=', 184: '<', 186: '>', 188: '<=', 190: '>='}   # OpFOrd*; the next opcode of each is its OpFUnord*


def _s32(x): return x - (1 << 32) if x & 0x80000000 else x
def _nan(x): return (x & 0x7fffffff) > 0x7f800000
def _finite(x): return (x & 0x7f800000) != 0x7f800000
def _float(x): return struct.unpack('<f', struct.pack('<I', x))[0]
def _bits(v): return 0x7fc00000 if v != v else struct.unpack('<I', struct.pack('<f', v))[0]   # an infinity or NaN: exact in a double


def run(data):
    """runs a translated compute shader of one basic block (what a program of literals folds to) on the CPU, with exact IEEE binary32
    arithmetic (round to nearest even, denormals kept); returns the words it stores into Workgroup memory, in program order. Raises on an
    instruction it does not model. An infinity or NaN operand gives an infinity or the NaN 0x7fc00000."""
    import operator
    ins = instructions(data)
    values, glsl = {}, set()
    for o, a in ins:
        if o == 43 and len(a) == 3: values[a[1]] = a[2]          # OpConstant
        elif o in (41, 42): values[a[1]] = int(o == 41)           # OpConstantTrue / False
        elif o == 11: glsl.add(a[0])                              # OpExtInstImport
    start = next(i for i, (o, _) in enumerate(ins) if o == 54)   # the first OpFunction: the entry point, the only one in such a module
    local, chains, out = {}, set(), []
    for o, a in ins[start + 1:]:
        if o in (248, 253, 56): continue                          # OpLabel, OpReturn, OpFunctionEnd
        if o == 54: break
        if o == 59: local[a[1]] = None; continue                   # OpVariable (Function)
        if o == 65: chains.add(a[1]); continue                     # OpAccessChain: into shared memory, the only kind stored to here
        if o == 62:                                                # OpStore
            if a[0] in local: local[a[0]] = values[a[1]]
            elif a[0] in chains: out.append(values[a[1]] & 0xffffffff)
            else: raise AssertionError(f'OpStore to %{a[0]}')
            continue
        r, x = a[1], [values.get(i) for i in a[2:]]
        if o == 61: values[r] = local[a[2]]                                                   # OpLoad
        elif o == 124: values[r] = x[0]                                                       # OpBitcast
        elif o == 169: values[r] = x[1] if x[0] else x[2]                                     # OpSelect
        elif o in _INT_OPS: values[r] = _INT_OPS[o](x[0], x[1]) & 0xffffffff
        elif o in _INT_COMPARES: values[r] = int(_INT_COMPARES[o](x[0], x[1]))
        elif o == 200: values[r] = ~x[0] & 0xffffffff                                         # OpNot
        elif o == 126: values[r] = -x[0] & 0xffffffff                                         # OpSNegate
        elif o == 166: values[r] = x[0] | x[1]                                                # OpLogicalOr
        elif o == 167: values[r] = x[0] & x[1]                                                # OpLogicalAnd
        elif o == 168: values[r] = 1 - x[0]                                                   # OpLogicalNot
        elif o == 164: values[r] = int(x[0] == x[1])                                          # OpLogicalEqual
        elif o == 165: values[r] = int(x[0] != x[1])                                          # OpLogicalNotEqual
        elif o == 127: values[r] = x[0] ^ 0x80000000                                          # OpFNegate
        elif o == 111: values[r] = f32_round(_s32(x[0]))                                       # OpConvertSToF (to nearest)
        elif o == 112: values[r] = f32_round(x[0])                                             # OpConvertUToF
        elif o in (109, 110):                                                                  # OpConvertFToU, OpConvertFToS: toward zero
            lo, hi = (0, 1 << 32) if o == 109 else (-(1 << 31), 1 << 31)
            v = int(f32_value(x[0])) if _finite(x[0]) else None
            values[r] = v & 0xffffffff if v is not None and lo <= v < hi else 0xdeadbeef   # out of range: undefined in SPIR-V
        elif o in (129, 131, 133):                                                            # OpFAdd, OpFSub, OpFMul
            p, q = x
            fn = {129: operator.add, 131: operator.sub, 133: operator.mul}[o]
            if not (_finite(p) and _finite(q)): values[r] = _bits(fn(_float(p), _float(q))); continue
            exact = fn(f32_value(p), f32_value(q))
            zero = {129: p & q, 131: p & ~q, 133: p ^ q}[o] & 0x80000000
            values[r] = f32_round(exact) if exact else zero
        elif 180 <= o <= 191:                                                                 # OpFOrdEqual .. OpFUnordGreaterThanEqual
            p, q = x
            if _nan(p) or _nan(q): values[r] = o & 1                                           # unordered: true for OpFUnord*
            else:
                cmp = {'==': operator.eq, '!=': operator.ne, '<': operator.lt, '>': operator.gt, '<=': operator.le, '>=': operator.ge}
                def value(t): return f32_value(t) if _finite(t) else (1 - 2 * (t >> 31)) * 2 ** 200
                values[r] = int(cmp[_FLOAT_COMPARES[o & ~1]](value(p), value(q)))
        elif o == 12 and a[2] in glsl:                                                         # OpExtInst GLSL.std.450
            n, arg = a[3], [values[i] for i in a[4:]]
            if n == 50:                                                                        # Fma
                p, q, s = arg
                if not all(map(_finite, arg)): values[r] = _bits(_float(p) * _float(q) + _float(s)); continue
                exact = f32_value(p) * f32_value(q) + f32_value(s)
                values[r] = f32_round(exact) if exact else ((p ^ q) & s & 0x80000000)
            elif n == 4: values[r] = arg[0] & 0x7fffffff                                      # FAbs
            elif n in (37, 40, 79, 80):                                                       # FMin, FMax, NMin, NMax: either zero may come back
                p, q = arg
                if _nan(p) or _nan(q): values[r] = q if _nan(p) else p
                elif f32_value(p) == f32_value(q): values[r] = q                               # a model of a device that returns the second
                else: values[r] = p if (f32_value(p) < f32_value(q)) == (n in (37, 79)) else q
            elif n in (38, 39, 41, 42):                                                       # UMin, SMin, UMax, SMax
                values[r] = (min if n in (38, 39) else max)(arg, key=(lambda t: t) if n in (38, 41) else _s32)
            else: raise AssertionError(f'GLSL.std.450 instruction {n} is not modelled')
        else: raise AssertionError(f'SPIR-V opcode {o} is not modelled')
    return out
