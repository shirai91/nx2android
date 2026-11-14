"""Integer-input inference follows identity temporaries without accepting FP uses."""
import ast
import collections
from pathlib import Path
import struct
import unittest

SOURCE = Path(__file__).resolve().parents[1]/'nxnative/dm/android/tools/shader_vk.py'
tree = ast.parse(SOURCE.read_text(encoding='utf-8'))
function = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'int_inputs')
namespace = dict(struct=struct, collections=collections, OP_DECORATE=71, OP_VARIABLE=59)
exec(compile(ast.Module(body=[function], type_ignores=[]), str(SOURCE), 'exec'), namespace)
infer = namespace['int_inputs']


def module(body, local_storage=7):
    # Scalar component of location11, a floating Function cell and scalar int types.
    rows = [(22, [1, 32]), (21, [2, 32, 1]), (21, [3, 32, 0]),
            (23, [4, 1, 4]), (32, [5, 1, 4]), (32, [6, 1, 1]),
            (32, [7, local_storage, 1]), (43, [2, 8, 0]), (43, [1, 9, 0]),
            (19, [12]), (33, [13, 12]), (71, [10, 30, 11]), (59, [5, 10, 1]),
            (54, [12, 14, 0, 13]), (248, [15]), (59, [7, 11, local_storage]),
            (65, [6, 20, 10, 8]), (61, [1, 21, 20]), *body, (253, []), (56, [])]
    words = [0x07230203, 0x00010300, 0, 128, 0]
    for op, args in rows:
        words.extend([((len(args) + 1) << 16) | op, *args])
    return struct.pack(f'<{len(words)}I', *words)


class IntegerInputFlowTests(unittest.TestCase):
    def test_direct_signed_and_unsigned_cast(self):
        self.assertEqual(infer(module([(124, [2, 22, 21])])), {'11': 'sint'})
        self.assertEqual(infer(module([(124, [3, 22, 21])])), {'11': 'uint'})

    def test_raw_function_temporary_and_copy(self):
        body = [(62, [11, 21]), (61, [1, 22, 11]), (83, [1, 23, 22]), (124, [2, 24, 23])]
        self.assertEqual(infer(module(body)), {'11': 'sint'})

    def test_global_literal_is_not_a_pointer_use(self):
        data = module([(62, [11, 21]), (61, [1, 22, 11]), (124, [2, 23, 22])])
        # Capability11 is a literal that happens to equal the Function cell's id.
        capability = struct.pack('<2I', 2 << 16 | 17, 11)
        self.assertEqual(infer(data[:20] + capability + data[20:]), {'11': 'sint'})

    def test_extinst_number_is_not_a_pointer_use(self):
        body = [(62, [11, 21]), (61, [1, 22, 11]), (124, [2, 23, 22]), (12, [1, 24, 60, 11, 9])]
        self.assertEqual(infer(module(body)), {'11': 'sint'})

    def test_multiple_integer_reads(self):
        body = [(62, [11, 21]), (61, [1, 22, 11]), (124, [3, 23, 22]),
                (61, [1, 24, 11]), (124, [2, 25, 24])]
        self.assertEqual(infer(module(body)), {'11': 'sint'})

    def test_construct_and_extract_preserve_raw_bits(self):
        body = [(80, [4, 22, 21, 9, 9, 9]), (81, [1, 23, 22, 0]), (124, [2, 24, 23])]
        self.assertEqual(infer(module(body)), {'11': 'sint'})

    def test_floating_use_of_constructed_vector_blocks_inference(self):
        body = [(80, [4, 22, 21, 9, 9, 9]), (81, [1, 23, 22, 0]),
                (124, [2, 24, 23]), (129, [1, 25, 23, 9])]
        self.assertEqual(infer(module(body)), {})

    def test_floating_use_after_identity_copy_blocks_inference(self):
        body = [(62, [11, 21]), (61, [1, 22, 11]), (124, [2, 23, 22]),
                (83, [1, 24, 22]), (129, [1, 25, 24, 9])]
        self.assertEqual(infer(module(body)), {})

    def test_function_pointer_escape_blocks_inference(self):
        body = [(62, [11, 21]), (61, [1, 22, 11]), (124, [2, 23, 22]), (57, [1, 24, 60, 11])]
        self.assertEqual(infer(module(body)), {})

    def test_non_function_store_is_not_an_identity_cell(self):
        body = [(62, [11, 21]), (61, [1, 22, 11]), (124, [2, 23, 22])]
        self.assertEqual(infer(module(body, local_storage=12)), {})

    def test_no_integer_consumer_is_not_inferred(self):
        self.assertEqual(infer(module([(62, [11, 21]), (61, [1, 22, 11])])), {})

    def test_mixed_stores_still_block_any_floating_consumer(self):
        body = [(62, [11, 21]), (62, [11, 9]), (61, [1, 22, 11]),
                (124, [2, 23, 22]), (129, [1, 24, 22, 9])]
        self.assertEqual(infer(module(body)), {})
