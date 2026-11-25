"""SHFL uses only b[4:0] as its lane or offset, in all four modes (adapted from seedds/nxnative's test_dmsh_shfl.py, MIT, whose runs need Metal).

The translator lowers SHFL to a helper function per mode (HelperFunctionManager); its second argument is the register lane or offset, which
must be masked to five bits before anything uses it. Before the fix high bits in the register changed the source lane and the predicate."""
import unittest

from maxwell_xlate import EXIT, instructions, s2r_tid, sts, translate

OP_FUNCTION, OP_PARAMETER, OP_FUNCTION_END, OP_LOAD, OP_BITWISE_AND = 54, 55, 56, 61, 199


def shfl(mode, immediate=False):
    # SHFL.<mode> R3|P0, R9, R1 (or 1), R2
    return 0xef10000000070000 | 3 | 9 << 8 | 1 << 20 | 2 << 39 | mode << 30 | int(immediate) << 28


class ShuffleLane(unittest.TestCase):
    def helper_index_uses(self, data):
        """for the helper function: every use of a load of its second parameter, as (opcode, operands)"""
        ins = instructions(data)
        consts = {a[1]: a[2] for o, a in ins if o == 43 and len(a) == 3}
        functions, current = [], None
        for o, a in ins:
            if o == OP_FUNCTION: current = []; functions.append(current)
            elif o == OP_FUNCTION_END: current = None
            elif current is not None: current.append((o, a))
        helpers = [f for f in functions if sum(o == OP_PARAMETER for o, _ in f) == 4]
        self.assertEqual(len(helpers), 1, 'one shuffle helper expected')
        index = [a[1] for o, a in helpers[0] if o == OP_PARAMETER][1]
        loads = {a[1] for o, a in helpers[0] if o == OP_LOAD and a[2] == index}
        self.assertTrue(loads)
        uses = [(o, a) for o, a in helpers[0] if o != OP_LOAD and any(x in loads for x in a[2:])]
        return [(o, sorted(consts.get(x) for x in a[2:] if x not in loads)) for o, a in uses]

    def test_register_lane_is_masked_in_every_mode(self):
        for mode, name in enumerate(('IDX', 'UP', 'DOWN', 'BFLY')):
            with self.subTest(mode=name):
                data, _ = translate([s2r_tid(9), s2r_tid(1), s2r_tid(2), shfl(mode), sts(3), EXIT])
                self.assertEqual(self.helper_index_uses(data), [(OP_BITWISE_AND, [31])], 'the lane must be b & 0x1f before any use')

    def test_immediate_lane_translates(self):
        for mode in range(4):
            with self.subTest(mode=mode):
                translate([s2r_tid(9), s2r_tid(1), s2r_tid(2), shfl(mode, immediate=True), sts(3), EXIT])


if __name__ == '__main__':
    unittest.main()
