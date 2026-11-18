"""shader_vk.py int_inputs: the vertex inputs a translated shader uses only through OpBitcast.

Only id operands count as uses. A literal word (an OpConstant value, an OpExtInst instruction
number, an OpCompositeExtract index, an image operands mask ...) that equals a load's id is not a
use, so it must not keep the input declared float (13 Sentinels: 7 of 376 vertex shaders).
"""
import ast
import collections
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'nxnative/dm/android/tools/shader_vk.py'


def load():
    # int_inputs and its constants only: importing shader_vk.py runs the command-line exporter.
    tree = ast.parse(SOURCE.read_text(encoding='utf-8'))
    names = {'OP_DECORATE', 'OP_VARIABLE', 'SPIRV_LITERALS'}

    def wanted(node):
        if isinstance(node, ast.FunctionDef): return node.name == 'int_inputs'
        if not isinstance(node, ast.Assign): return False
        targets = [e for t in node.targets for e in (t.elts if isinstance(t, ast.Tuple) else [t])]
        return any(isinstance(t, ast.Name) and t.id in names for t in targets)
    env = {'struct': struct, 'collections': collections}
    exec(compile(ast.Module(body=[n for n in tree.body if wanted(n)], type_ignores=[]), str(SOURCE), 'exec'), env)
    return env['int_inputs']


def inst(op, *words):
    return [(len(words) + 1) << 16 | op, *words]


def string(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack(f'<{len(b) // 4}I', b))


# Ids of the test shader. LOAD is the load of input 0 (location 0), cast to uint by CAST.
GLSL, MAIN, VOID, FN, FLOAT, V4F, UINT, INT, V4U, V4I, P_IN_V4F, P_IN_F = range(1, 13)
IN0, IN1, OUT, P_OUT_V4F, ZERO, LABEL, BOOL, LOAD, CAST = range(13, 22)
R = 30   # first id for the results of the instructions a test adds
LOAD1 = 50   # the load of input 1 (location 1), in tests that load it


def shader(globals_=(), body=()):
    """A vertex shader: input 0 is loaded (LOAD) and bit-cast to uint4 (CAST); then the given
    module-level instructions and function-body instructions."""
    w = [0x07230203, 0x00010000, 0, 64, 0]
    w += inst(17, 1)                                     # OpCapability Shader
    w += inst(11, GLSL, *string('GLSL.std.450'))         # OpExtInstImport
    w += inst(14, 0, 1)                                  # OpMemoryModel Logical GLSL450
    w += inst(15, 0, MAIN, *string('main'), IN0, IN1, OUT)   # OpEntryPoint Vertex
    w += inst(71, IN0, 30, 0) + inst(71, IN1, 30, 1)     # OpDecorate Location
    w += inst(71, OUT, 11, 0)                            # OpDecorate BuiltIn Position
    w += inst(19, VOID) + inst(33, FN, VOID) + inst(22, FLOAT, 32) + inst(23, V4F, FLOAT, 4)
    w += inst(21, UINT, 32, 0) + inst(21, INT, 32, 1) + inst(23, V4U, UINT, 4) + inst(23, V4I, INT, 4)
    w += inst(32, P_IN_V4F, 1, V4F) + inst(32, P_IN_F, 1, FLOAT) + inst(32, P_OUT_V4F, 3, V4F)
    w += inst(20, BOOL) + inst(43, UINT, ZERO, 0)
    w += inst(59, P_IN_V4F, IN0, 1) + inst(59, P_IN_V4F, IN1, 1) + inst(59, P_OUT_V4F, OUT, 3)
    for g in globals_: w += g
    w += inst(54, VOID, MAIN, 0, FN) + inst(248, LABEL)  # OpFunction, OpLabel
    w += inst(61, V4F, LOAD, IN0)                        # OpLoad
    w += inst(124, V4U, CAST, LOAD)                      # OpBitcast
    for b in body: w += b
    w += inst(253) + inst(56)                            # OpReturn, OpFunctionEnd
    return struct.pack(f'<{len(w)}I', *w)


class IntInputs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.int_inputs = staticmethod(load())

    def test_bitcast_only_input_is_uint(self):
        self.assertEqual(self.int_inputs(shader()), {'0': 'uint'})

    def test_bitcast_to_signed_type_is_sint(self):
        self.assertEqual(self.int_inputs(shader(body=[inst(124, V4I, R, LOAD)])), {'0': 'sint'})

    def test_other_use_keeps_input_float(self):
        self.assertEqual(self.int_inputs(shader(body=[inst(129, V4F, R, LOAD, LOAD)])), {})   # OpFAdd

    def test_component_through_access_chain(self):
        body = [inst(65, P_IN_F, R, IN1, ZERO), inst(61, FLOAT, R + 1, R),   # OpAccessChain, OpLoad
                inst(124, UINT, R + 2, R + 1)]
        self.assertEqual(self.int_inputs(shader(body=body)), {'0': 'uint', '1': 'uint'})

    def test_only_the_bitcast_input_is_reported(self):
        body = [inst(61, V4F, LOAD1, IN1), inst(62, OUT, LOAD1)]   # input 1 stored as it is
        self.assertEqual(self.int_inputs(shader(body=body)), {'0': 'uint'})

    def test_literal_equal_to_load_id_is_not_a_use(self):
        n = LOAD
        cases = {
            'OpCapability': ([inst(17, n)], []),
            'OpExecutionMode': ([inst(16, MAIN, 17, n, 1, 1)], []),
            'OpLine': ([inst(8, GLSL, n, 1)], []),
            'OpDecorate': ([inst(71, OUT, 35, n)], []),
            'OpMemberDecorate': ([inst(72, V4F, n, 35, n)], []),
            'OpTypeVector': ([inst(23, R, UINT, n)], []),
            'OpConstant': ([inst(43, UINT, R, n)], []),
            'OpSpecConstant': ([inst(50, UINT, R, n)], []),
            'OpExtInst': ([], [inst(12, FLOAT, R, GLSL, n, ZERO, ZERO)]),
            'OpCompositeExtract': ([], [inst(81, UINT, R, CAST, n)]),
            'OpCompositeInsert': ([], [inst(82, V4U, R, ZERO, CAST, n)]),
            'OpVectorShuffle': ([], [inst(79, V4U, R, CAST, CAST, n, 0, 1, 2)]),
            'OpStore memory access': ([], [inst(62, OUT, ZERO, 2, n)]),
            'OpLoad memory access': ([], [inst(61, V4F, R, OUT, 2, n)]),
            'OpImageFetch mask': ([], [inst(95, V4F, R, ZERO, ZERO, n, ZERO, ZERO, ZERO)]),
            'OpImageSampleDrefExplicitLod mask': ([], [inst(90, FLOAT, R, ZERO, ZERO, ZERO, n, ZERO)]),
            'OpImageWrite mask': ([], [inst(99, ZERO, ZERO, ZERO, n, ZERO, ZERO, ZERO)]),
            'OpGroupNonUniformIAdd operation': ([], [inst(349, UINT, R, ZERO, n, ZERO)]),
            'OpArrayLength': ([], [inst(68, UINT, R, OUT, n)]),
            'OpSelectionMerge': ([], [inst(247, LABEL, n)]),
            'OpLoopMerge': ([], [inst(246, LABEL, LABEL, n, n)]),
            'OpBranchConditional weights': ([], [inst(250, ZERO, LABEL, LABEL, n, 1)]),
            'OpSwitch literal': ([], [inst(251, ZERO, LABEL, n, LABEL)]),
        }
        for name, (globals_, body) in cases.items():
            with self.subTest(name):
                self.assertEqual(self.int_inputs(shader(globals_, body)), {'0': 'uint'})

    def test_load_in_an_id_operand_keeps_input_float(self):
        # The id operands next to the literals still count: these uses keep input 0 float.
        n = LOAD
        cases = {
            'OpStore object': [inst(62, OUT, n)],
            'OpStore object with memory access': [inst(62, OUT, n, 2, 16)],
            'OpCopyMemorySized size': [inst(64, OUT, OUT, n)],
            'OpExtInst operand': [inst(12, V4F, R, GLSL, 40, n, n)],   # FMax
            'OpExtInst last operand': [inst(12, V4F, R, GLSL, 40, ZERO, n)],
            'OpCompositeExtract composite': [inst(81, FLOAT, R, n, 0)],
            'OpCompositeInsert object': [inst(82, V4F, R, n, ZERO, 0)],
            'OpCompositeInsert composite': [inst(82, V4F, R, ZERO, n, 0)],
            'OpVectorShuffle vector 1': [inst(79, V4F, R, n, ZERO, 0, 1, 2, 3)],
            'OpVectorShuffle vector 2': [inst(79, V4F, R, ZERO, n, 4, 5, 6, 7)],
            'OpImageFetch coordinate': [inst(95, V4F, R, ZERO, n)],
            'OpImageFetch Lod after the mask': [inst(95, V4F, R, ZERO, ZERO, 2, n)],
            'OpImageSampleDrefExplicitLod dref': [inst(90, FLOAT, R, ZERO, ZERO, n, 2, ZERO)],
            'OpImageWrite texel': [inst(99, ZERO, ZERO, n)],
            'OpGroupNonUniformIAdd value': [inst(349, UINT, R, ZERO, 0, n)],
            'OpBranchConditional condition': [inst(250, n, LABEL, LABEL)],
            'OpSwitch selector': [inst(251, n, LABEL)],
            # an opcode with no layout in the table keeps every word: the old, safe answer
            'unknown layout': [inst(5000, UINT, R, ZERO, n, ZERO)],   # OpGroupIAddNonUniformAMD
        }
        for name, body in cases.items():
            with self.subTest(name):
                self.assertEqual(self.int_inputs(shader(body=body)), {})


if __name__ == '__main__': unittest.main()
