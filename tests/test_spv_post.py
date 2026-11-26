"""dm/android/tools/spv_post.py: a pixel shader's discards as demotions (game.tsv android_discard demote)."""
import struct, sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'nxnative/dm/android/tools'))
import spv_post

def ins(op, *a): return [(1 + len(a)) << 16 | op, *a]
def mod(body): return struct.pack('<%dI' % (5 + len(body)), 0x07230203, 0x10300, 0, 100, 0, *body)
def ops(spv):
    w = struct.unpack('<%dI' % (len(spv) // 4), spv); i = 5; out = []
    while i < len(w): out.append((w[i] & 0xffff, w[i + 1:i + (w[i] >> 16)])); i += w[i] >> 16
    return out
CAP_SHADER = ins(17, 1)

class DemoteDiscards(unittest.TestCase):
    def test_arm_kill_becomes_demote_and_branch_to_merge(self):
        body = CAP_SHADER + ins(248, 10) + ins(247, 13, 0) + ins(250, 5, 11, 13) + ins(248, 11) + ins(252) + ins(248, 13) + ins(253)
        out, n, left = spv_post.demote_discards(mod(body)); o = ops(out)
        self.assertEqual((n, left), (1, 0))
        self.assertEqual(o[0], (17, (1,))); self.assertEqual(o[1], (17, (5379,))); self.assertEqual(o[2][0], 10)
        self.assertEqual(struct.pack('<%dI' % len(o[2][1]), *o[2][1]).rstrip(b'\0'), b'SPV_EXT_demote_to_helper_invocation')
        self.assertNotIn(252, [x[0] for x in o])
        k = [x[0] for x in o].index(5380); self.assertEqual(o[k + 1], (249, (13,)))
        self.assertEqual(spv_post.demote_discards(out), (None, 0, 0))   # nothing left to change

    def test_kill_outside_a_selection_arm_is_left(self):
        body = CAP_SHADER + ins(248, 10) + ins(252)
        self.assertEqual(spv_post.demote_discards(mod(body)), (None, 0, 1))

    def test_both_arms_and_an_existing_capability(self):
        body = CAP_SHADER + ins(17, 5379) + ins(248, 10) + ins(247, 13, 0) + ins(250, 5, 11, 12) + ins(248, 11) + ins(252) + ins(248, 12) + ins(252) + ins(248, 13) + ins(253)
        out, n, left = spv_post.demote_discards(mod(body)); o = ops(out)
        self.assertEqual((n, left), (2, 0)); self.assertEqual([x[0] for x in o].count(17), 2); self.assertEqual([x[0] for x in o].count(10), 0)
        self.assertEqual([x for x in o if x[0] == 249], [(249, (13,)), (249, (13,))])

class TextPatch(unittest.TestCase):
    def test_missing_tools_refuse(self):
        with self.assertRaises(ValueError): spv_post.text_patch(b'', lambda t: t, None, '.', 'x')

if __name__ == '__main__': unittest.main()
