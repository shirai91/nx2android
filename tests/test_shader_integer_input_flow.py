"""Keep main's integer-input inference while the address pass fixes its inputs."""
import struct
import unittest

from test_shader_int_inputs import load

infer = load()


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
    # Preserve the pre-address-pass inference contract. In particular, do not
    # widen it to pre-existing Function temporaries or copied component graphs.
    def test_direct_signed_and_unsigned_cast(self):
        self.assertEqual(infer(module([(124, [2, 22, 21])])), {'11': 'sint'})
        self.assertEqual(infer(module([(124, [3, 22, 21])])), {'11': 'uint'})

    def test_function_temporary_is_not_newly_inferred(self):
        body = [(62, [11, 21]), (61, [1, 22, 11]), (124, [2, 23, 22])]
        self.assertEqual(infer(module(body)), {})

    def test_identity_copy_is_not_newly_inferred(self):
        self.assertEqual(infer(module([(83, [1, 22, 21]), (124, [2, 23, 22])])), {})

    def test_vector_copy_is_not_newly_inferred(self):
        body = [(80, [4, 22, 21, 9, 9, 9]), (81, [1, 23, 22, 0]), (124, [2, 24, 23])]
        self.assertEqual(infer(module(body)), {})

    def test_floating_consumer_blocks_direct_inference(self):
        body = [(124, [2, 22, 21]), (129, [1, 23, 21, 9])]
        self.assertEqual(infer(module(body)), {})

    def test_no_integer_consumer_is_not_inferred(self):
        self.assertEqual(infer(module([])), {})


if __name__ == '__main__': unittest.main()
