"""Execute emitted integer SPIR-V against an independent IEEE binary16 oracle."""
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'nxnative/dm/android/tools'))
import spv_half
from spv_module import decode, opcode

SOURCE = '''OpCapability Shader
%glsl = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%pair = OpTypeVector %float 2
%ft = OpTypeFunction %void
%zero = OpConstant %float 0
%v = OpConstantComposite %pair %zero %zero
%main = OpFunction %void None %ft
%entry = OpLabel
%result = OpExtInst %uint %glsl PackHalf2x16 %v
OpReturn
OpFunctionEnd
'''


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def oracle(value):
    number = struct.unpack('<f', struct.pack('<I', value))[0]
    try:
        return int.from_bytes(struct.pack('<e', number), 'little')
    except OverflowError:
        return (value >> 16 & 0x8000) | 0x7c00


def finite_inputs():
    # Every positive finite half, every midpoint and the binary32 values
    # on either side, both signs; plus all exponent classes via random bits.
    inputs = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7f7fffff, 0xff7fffff}
    for value in range(0x7c00):
        a = struct.unpack('<e', struct.pack('<H', value))[0]
        b = struct.unpack('<e', struct.pack('<H', value + 1))[0] if value < 0x7bff else 65536.
        midpoint = bits((a+b)/2)
        for point in (bits(a), midpoint-1, midpoint, midpoint+1):
            inputs.update((point, point | 0x80000000))
    rng = random.Random(0x16)
    inputs.update(value for _ in range(4096) if (value := rng.getrandbits(32)) & 0x7fffffff <= 0x7f800000)
    ordered = sorted(inputs)
    if len(ordered) % 2: ordered.append(0)
    return ordered


def interpreter(data):
    """Minimal integer SPIR-V interpreter, independent of the conversion math."""
    _, instructions = decode(data)
    constants = {x[2]: x[3] for x in instructions if opcode(x) == 43}
    start = max(i for i, x in enumerate(instructions) if opcode(x) == 54)
    body = [(opcode(x), x) for x in instructions[start + 1:]]

    def run(pair):
        values = dict(constants)
        for op, x in body:
            if op == 55: values[x[2]] = pair
            elif op in (248, 56): continue
            elif op == 254: return values[x[1]]
            else:
                a = values[x[3]]
                if op == 81: result = a[x[4]]
                elif op == 124: result = a  # floats represented by raw bits
                else:
                    b = values[x[4]]
                    if op == 128: result = a + b
                    elif op == 130: result = a - b
                    elif op == 169: result = b if a else values[x[5]]
                    elif op == 172: result = a > b
                    elif op == 176: result = a < b
                    elif op == 194:
                        assert b < 32, b
                        result = a >> b
                    elif op == 196:
                        assert b < 32, b
                        result = a << b
                    elif op == 197: result = a | b
                    elif op == 199: result = a & b
                    else: raise AssertionError(op)
                values[x[2]] = result & 0xffffffff
        raise AssertionError('missing return')
    return run


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'requires SPIR-V tools')
class HalfPack(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name)
        (self.path/'input.spvasm').write_text(SOURCE)
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', self.path/'input.spvasm', '-o', self.path/'input.spv'], check=True)
        self.raw = (self.path/'input.spv').read_bytes()
        self.lowered = spv_half.lower(self.raw)
        (self.path/'lowered.spv').write_bytes(self.lowered)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', self.path/'lowered.spv'], check=True)

    def test_all_finite_half_boundaries_and_float_classes(self):
        ordered = finite_inputs()
        execute = interpreter(self.lowered)
        for i in range(0, len(ordered), 2):
            pair = ordered[i:i+2]
            expected = oracle(pair[0]) | oracle(pair[1]) << 16
            actual = execute(pair)
            if actual != expected:
                self.fail(f'{pair}: {actual:08x} != {expected:08x}')

    def test_apple_nan_canonicalization_and_no_new_features(self):
        execute = interpreter(self.lowered)
        for low in (0x7f800001, 0x7fa00000, 0x7fc12345, 0x7fffffff):
            self.assertEqual(execute((low, low | 0x80000000)), 0x7e007e00)
        self.assertEqual(spv_half.lower(self.lowered), self.lowered)
        _, code = decode(self.lowered)
        self.assertEqual([x[1] for x in code if opcode(x) == 17], [1])
        self.assertFalse(any(opcode(x) == 22 and x[2] != 32 for x in code))
        self.assertFalse(any(opcode(x) == 21 and x[2] != 32 for x in code))
        self.assertFalse(any(opcode(x) == 12 and x[4] == 58 for x in code))


if __name__ == '__main__': unittest.main()
