"""dm/android/tools/spv_post.py: a pixel shader's discards as demotions (game.tsv android_discard demote) and a
profile's patch of one translated shader (the text way, on its spirv-dis text, and the binary way, on its words)."""
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

class BinaryPatch(unittest.TestCase):
    """the binary way: a patch on the SPIR-V words, which needs no spirv-dis, spirv-as and spirv-val"""

    def test_a_module_survives_the_reader_and_the_writer(self):
        body = CAP_SHADER + ins(19, 1, 32, 0) + ins(59, 2, 1, 7)
        m = spv_post.Module(mod(body))
        self.assertEqual([spv_post.spv_opcode(i) for i in m.code], [17, 19, 59])
        self.assertEqual(m.op(0), 17)
        self.assertEqual([x[0] for x in ops(m.bytes())], [17, 19, 59])

    def test_a_fresh_id_grows_the_bound(self):
        m = spv_post.Module(mod(CAP_SHADER)); bound = m.header[3]
        self.assertEqual(m.ident(), bound); self.assertEqual(m.header[3], bound + 1)

    def test_the_binary_path_needs_no_external_tool(self):
        @spv_post.binary
        def patch(m):
            m.replace(0, [ins(17, 1)]); return m
        self.assertEqual(spv_post.text_patch(mod(CAP_SHADER), patch, None, '.', 'x'), mod(CAP_SHADER))

    def test_a_patch_that_is_written_for_another_module_raises(self):
        @spv_post.binary
        def patch(m): raise ValueError('not the module this patch was written for')
        with self.assertRaises(ValueError): spv_post.binary_patch(mod(CAP_SHADER), patch)

if __name__ == '__main__': unittest.main()
