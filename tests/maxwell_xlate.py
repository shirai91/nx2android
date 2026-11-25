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
        res = json.loads((work / 'k.res.json').read_text())
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
    """the fixed bits of an instruction, from the decoder's InstTable.cs"""
    import re
    if not _TABLE: _TABLE['src'] = (ROOT / 'nxnative/dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs').read_text()
    found = re.search(r'Add\("([01x]{64})", InstName\.' + name + r'\s*,', _TABLE['src'])
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
