"""shader_vk.py int_inputs: which vertex inputs a translated vertex shader uses only as integers (the pipeline then feeds the integer format of the
attribute's layout, gfx_vk_exec.c vertex_format). A cast whose integer only goes back to float, directly or through the translator's register
variables, is a raw move: Dead or Alive Xtreme 3 Scarlet's ocean vertex shader 6548b85b2c1cb7cf copies its RGBA8 UNORM attribute that way, and
declared sint it was fed R8G8B8A8_SINT, whose bytes >= 0x80 came out as NaN bit patterns."""
import ast
import collections
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'nxnative/dm/android/tools'

# SPIR-V opcodes and enums the modules below use
TYPE_INT, TYPE_FLOAT, TYPE_VECTOR, TYPE_POINTER, VARIABLE = 21, 22, 23, 32, 59
LOAD, STORE, ACCESS_CHAIN, DECORATE, BITCAST, IADD, CONSTANT = 61, 62, 65, 71, 124, 128, 43
INPUT, OUTPUT, FUNCTION, LOCATION = 1, 3, 7, 30


def int_inputs_fn():
    source = TOOLS / 'shader_vk.py'
    tree = ast.parse(source.read_text(encoding='utf-8'))
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'int_inputs' or
             isinstance(n, ast.Assign) and any(isinstance(t, ast.Tuple) and any(isinstance(v, ast.Name) and v.id == 'OP_DECORATE' for v in t.elts)
                                               for t in n.targets)]
    env = {'struct': struct, 'collections': collections}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), env)
    return env['int_inputs']


def module(body):
    """A vertex shader's words: the header, the types and variables every case shares, then body (a list of (opcode, operands))."""
    ins = [(TYPE_INT, [1, 32, 1]), (TYPE_FLOAT, [2, 32]), (TYPE_VECTOR, [3, 2, 4]),              # %1 int, %2 float, %3 vec4
           (TYPE_POINTER, [4, INPUT, 3]), (TYPE_POINTER, [5, INPUT, 2]), (TYPE_POINTER, [6, FUNCTION, 1]),
           (TYPE_POINTER, [7, OUTPUT, 2]), (CONSTANT, [8, 1, 0]),                               # %8 int 0
           (DECORATE, [10, LOCATION, 6]), (VARIABLE, [4, 10, INPUT]),                            # %10: vec4 input, location 6
           (VARIABLE, [6, 11, FUNCTION]),                                                        # %11: a register
           (DECORATE, [12, LOCATION, 0]), (VARIABLE, [7, 12, OUTPUT])] + body                    # %12: an output
    words = [0x07230203, 0x10300, 0, 100, 0]
    for op, a in ins:
        words += [(len(a) + 1) << 16 | op] + a
    return struct.pack(f'<{len(words)}I', *words)


# the input's component 0, loaded and cast to int: %21
LOAD_AS_INT = [(ACCESS_CHAIN, [5, 20, 10, 8]), (LOAD, [2, 30, 20]), (BITCAST, [1, 21, 30])]


class IntInputs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.int_inputs = staticmethod(int_inputs_fn())

    def test_raw_move_through_a_register_stays_float(self):
        # input -> int -> register -> int -> float -> output: the bits only move
        spv = module(LOAD_AS_INT + [(STORE, [11, 21]), (LOAD, [1, 22, 11]), (BITCAST, [2, 23, 22]), (STORE, [12, 23])])
        self.assertEqual(self.int_inputs(spv), {})

    def test_cast_straight_back_to_float_stays_float(self):
        spv = module(LOAD_AS_INT + [(BITCAST, [2, 23, 21]), (STORE, [12, 23])])
        self.assertEqual(self.int_inputs(spv), {})

    def test_integer_arithmetic_makes_an_integer_input(self):
        spv = module(LOAD_AS_INT + [(IADD, [1, 24, 21, 8])])
        self.assertEqual(self.int_inputs(spv), {'6': 'sint'})

    def test_integer_use_of_the_register_makes_an_integer_input(self):
        # the register's value is also added as an integer: not a raw move
        spv = module(LOAD_AS_INT + [(STORE, [11, 21]), (LOAD, [1, 22, 11]), (BITCAST, [2, 23, 22]), (STORE, [12, 23]),
                                    (LOAD, [1, 25, 11]), (IADD, [1, 26, 25, 8])])
        self.assertEqual(self.int_inputs(spv), {'6': 'sint'})

    def test_register_reaching_an_access_chain_keeps_the_input_integer(self):
        # the register's pointer also goes into an access chain, where an integer use of it would not show as an OpLoad: not provably a raw move
        spv = module(LOAD_AS_INT + [(STORE, [11, 21]), (LOAD, [1, 22, 11]), (BITCAST, [2, 23, 22]), (STORE, [12, 23]),
                                    (ACCESS_CHAIN, [6, 27, 11, 8])])
        self.assertEqual(self.int_inputs(spv), {'6': 'sint'})

    def test_float_use_keeps_a_float_input(self):
        spv = module([(ACCESS_CHAIN, [5, 20, 10, 8]), (LOAD, [2, 30, 20]), (STORE, [12, 30])])
        self.assertEqual(self.int_inputs(spv), {})


if __name__ == '__main__':
    unittest.main()
