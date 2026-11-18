# patch_nso.py site_window: where each way out of an exclusive window's copy lands.
# The window is copied into the patch area with x18 replaced; every way out goes through an exit stub that stores the stand-in back: the fall-through stub first, then one
# stub a target outside the window. A compare-and-swap that gives up branches to the instruction after the window (ldaxrb; cbz <store>; clrex; b <after>; stlxrb;
# cbz <claimed>): that branch must take the fall-through stub. It took the first branch-out stub, <claimed>'s: Trails in the Sky 1st Chapter's material table (main+0x2cdeb4)
# handed a slot already in use to a second material, which was then drawn with the first one's entry.
import os, re, struct, sys, unittest

HERE = os.path.dirname(os.path.abspath(__file__)); TOOLS = os.path.join(HERE, '..', 'nxnative', 'dm', 'tools')
try:
    import capstone   # noqa: F401
    HAVE = True
except ImportError:
    HAVE = False

def load():
    """patch_nso.py's encoders, helpers and site_window (the script's lines from its disassembler to retry_block), without the image it works on"""
    with open(os.path.join(TOOLS, 'patch_nso.py'), encoding='utf-8') as f: src = f.read().split('\n')
    a = next(i for i, l in enumerate(src) if l.startswith('md = Cs(')); b = next(i for i, l in enumerate(src) if l.startswith('def retry_block'))
    sys.path.insert(0, TOOLS)
    ns = {'PATCH_OFF': 0x730000, 'KEY_PAGE_OFF': 0x728000, 'KEY_INDEX': {'K18': 0}}
    exec('import struct, re, os\nfrom itertools import combinations\nfrom capstone import *\nfrom nso_asm import *\n', ns)
    os.environ['DM_TARGET'] = 'android'
    exec('\n'.join(src[a:b]), ns)
    return ns

@unittest.skipUnless(HAVE, 'capstone is not installed')
class WindowExits(unittest.TestCase):
    def window(self, words, at):
        ns = load(); seg = []
        for k, w in enumerate(words): seg += list(ns['md'].disasm(struct.pack('<I', w), at + 4 * k))
        self.assertEqual(len(seg), len(words))
        start, lo, hi = ns['site_window'](seg)
        code = list(ns['md'].disasm(bytes(ns['patch']), ns['PATCH_OFF']))
        return ns, code, lo, hi

    def follow(self, code, addr):
        """the address outside the patch area that the stub at addr branches back to"""
        for i in code:
            if i.address >= addr and i.mnemonic == 'b': return i.operands[0].imm
        self.fail(f'no branch after {addr:#x}')

    def test_give_up_branch_takes_the_fall_through_stub(self):
        at = 0x2cdeb4; claimed = 0x2cdf18
        words = [0x085ffe40,                                 # ldaxrb w0, [x18]
                 0x34000060,                                 # cbz w0, +12 (the store)
                 0xd503305f,                                 # clrex
                 0x14000003,                                 # b +12 (the instruction after the window)
                 0x0800fe4e,                                 # stlxrb w0, w14, [x18]
                 0x34000000 | ((claimed - (at + 20)) >> 2) << 5]   # cbz w0, <claimed>
        ns, code, lo, hi = self.window(words, at)
        self.assertEqual((lo, hi), (at, at + 24))
        by = {i.mnemonic: i for i in code if i.mnemonic in ('clrex',)}
        give_up = next(i for i in code if i.address == by['clrex'].address + 4); self.assertEqual(give_up.mnemonic, 'b')
        self.assertEqual(self.follow(code, give_up.operands[0].imm), hi, 'a slot found in use goes on to the next candidate')
        store = next(i for i in code if i.mnemonic == 'stlxrb'); self.assertNotIn('x18', store.op_str)
        done = next(i for i in code if i.address == store.address + 4); self.assertEqual(done.mnemonic, 'cbz')
        self.assertEqual(self.follow(code, done.operands[-1].imm), claimed, 'a store that took the slot leaves by its own stub')
        self.assertEqual(self.follow(code, done.address + 4), hi, 'a store that failed falls through')

if __name__ == '__main__':
    unittest.main()
