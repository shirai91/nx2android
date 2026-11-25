"""patch_nso.py STUBS: a profile's patches.py assembles code into the patch area and branches to it from a function's entry (Ori and the Will of the Wisps 1.2.1's
registry_lock is the first user). The rewriter runs on a synthetic module in a copy of its tools and a test profile, without game files."""
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/tools'
sys.path.insert(0, str(TOOLS))
from nso_asm import ADRP, B, BL, CBNZW, LDRX, NOP   # noqa: E402

BID = '5ab5' * 8   # the synthetic module's build ID, claimed by the test profile only
TEXT, RO, DATA = 0x0, 0x1000, 0x2000
FN, FN2, ADDR_AT, SVC_AT, SLOT = 0x40, 0x400, 0x60, 0x420, 0x2010
FIRST = 0xd10043ff   # sub sp, sp, #0x10: the function's first instruction, which the stub runs before it jumps back


def module():
    """An uncompressed NSO: text with two functions (FN: sub sp; adrp / ldr of SLOT; ret, FN2: an svc, a rewrite site), ro with MOD0 and eh_frame_hdr naming both"""
    text = bytearray(struct.pack('<I', NOP) * 0x400)
    struct.pack_into('<II', text, 0, B(0, 8), RO)   # MOD0 offset at +4
    words = {FN: FIRST, FN + 4: NOP, ADDR_AT: ADRP(8, ADDR_AT, SLOT), ADDR_AT + 4: LDRX(8, 8, (SLOT & 0xfff) // 8), ADDR_AT + 8: 0xd65f03c0,
             SVC_AT: 0xd4000001, SVC_AT + 4: 0xd65f03c0}
    for at, w in words.items(): struct.pack_into('<I', text, at, w)
    ro = bytearray(0x1000)
    eh = 0x100   # eh_frame_hdr, from the start of ro
    struct.pack_into('<4s5i', ro, 0, b'MOD0', 0, 0, 0, eh, eh)
    ro[eh:eh + 4] = bytes((1, 0x1b, 0x03, 0x3b))   # version, eh_frame_ptr pcrel sdata4, fde_count udata4, table datarel sdata4
    ehs = RO + eh
    struct.pack_into('<iI4i', ro, eh + 4, 0, 2, FN - ehs, 0, FN2 - ehs, 0)
    data = bytearray(0x1000)
    segs = (TEXT, len(text)), (RO, len(ro)), (DATA, len(data))
    hdr = bytearray(0x100); hdr[:4] = b'NSO0'
    fo = 0x100
    for i, (mo, sz) in enumerate(segs):
        struct.pack_into('<III', hdr, 0x10 + 0x10 * i, fo, mo, sz); struct.pack_into('<I', hdr, 0x60 + 4 * i, sz); fo += sz
    struct.pack_into('<I', hdr, 0x3c, 0)
    hdr[0x40:0x50] = bytes.fromhex(BID)
    return bytes(hdr + text + ro + data)


STUB = '''
from nso_asm import B, BL, CBNZW, NOP
def fix(ctx):
    ctx.site({fn:#x}, '{first}')
    slot = ctx.address({addr:#x})
    start = ctx.here()
    ctx.emit(CBNZW(0, ctx.here(), start + 8)); ctx.emit(BL(ctx.here(), {fn2:#x}))
    ctx.emit({first_word:#x}); ctx.emit(B(ctx.here(), {fn:#x} + 4))
    ctx.put({fn:#x}, [B({fn:#x}, start)], f'test stub, slot {{slot:#x}}')
STUBS = {{'{bid}': fix}}
'''


class Stubs(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.tools = self.tmp / 'dm/tools'; self.tools.mkdir(parents=True)
        for name in ('patch_nso.py', 'nso_code.py', 'nso_asm.py', 'sdk_patches.py'): shutil.copy(TOOLS / name, self.tools / name)
        self.profile = self.tmp / 'dm/games/0100000000ABC000'; self.profile.mkdir(parents=True)
        (self.tmp / 'main').write_bytes(module())

    def rewrite(self, body):
        (self.profile / 'patches.py').write_text(body, encoding='utf-8')
        out = self.tmp / 'out'; shutil.rmtree(out, ignore_errors=True); out.mkdir()
        r = subprocess.run([sys.executable, str(self.tools / 'patch_nso.py'), str(self.tmp / 'main'), str(out), 'main'], capture_output=True, text=True,
                           env={'PATH': '', 'DM_TARGET': 'android', 'PYTHONPATH': ''}, cwd=self.tmp)
        return r, out

    def stub(self, **kw):
        return STUB.format(**{'fn': FN, 'fn2': FN2, 'addr': ADDR_AT, 'first': FIRST.to_bytes(4, 'little').hex(), 'first_word': FIRST, 'bid': BID, **kw})

    def test_stub_is_assembled_and_branched_to(self):
        r, out = self.rewrite(self.stub())
        self.assertEqual(r.returncode, 0, r.stderr)
        fixups = dict(l[2:].split('\t') for l in (out / 'main.fixups.tsv').read_text().splitlines() if l.startswith('# '))
        patch_off = int(fixups['PATCH_OFF'], 16)
        img = (out / 'main.patched.bin').read_bytes()
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        site = next(md.disasm(img[FN:FN + 4], FN))
        self.assertEqual(site.mnemonic, 'b')
        start = int(site.op_str.lstrip('#'), 16)
        self.assertGreaterEqual(start, patch_off)
        ins = list(md.disasm(img[start:start + 16], start))
        self.assertEqual([(i.mnemonic, i.op_str) for i in ins],
                         [('cbnz', f'w0, #{start + 8:#x}'), ('bl', f'#{FN2:#x}'), ('sub', 'sp, sp, #0x10'), ('b', f'#{FN + 4:#x}')])
        self.assertEqual(struct.unpack_from('<I', img, FN + 4)[0], NOP)   # the rest of the function is untouched
        rows = [l.split('\t') for l in (out / 'main.cuts.tsv').read_text().splitlines() if not l.startswith('#')]
        self.assertEqual(rows, [[f'{FN:#x}', FIRST.to_bytes(4, 'little').hex(), B(FN, start).to_bytes(4, 'little').hex(), f'test stub, slot {SLOT:#x}']])
        self.assertNotIn('FPS_STATE_OFF', fixups)   # no frame step: the host's pacing is untouched
        self.assertIn("'stub': 1", r.stdout)

    def test_original_bytes_must_match(self):
        r, _ = self.rewrite(self.stub(first='ffc300d1'))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(f'stub original bytes mismatch {FN:#x}', r.stderr)

    def test_a_rewritten_site_is_refused(self):   # the svc already branches to its gate when the stubs run, so its original bytes are gone
        r, _ = self.rewrite(self.stub(fn=SVC_AT, first='010000d4', first_word=0xd4000001))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(f'stub original bytes mismatch {SVC_AT:#x}', r.stderr)

    def test_address_needs_an_adrp_pair(self):
        r, _ = self.rewrite(self.stub(addr=FN))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(f'stub: no adrp at {FN:#x}', r.stderr)

    def test_extra_patch_files_hold_no_stubs(self):
        (self.profile / 'patches.py').write_text('', encoding='utf-8')
        extra = self.tmp / 'extra.py'; extra.write_text(self.stub(), encoding='utf-8')
        out = self.tmp / 'out'; out.mkdir()
        r = subprocess.run([sys.executable, str(self.tools / 'patch_nso.py'), str(self.tmp / 'main'), str(out), 'main'], capture_output=True, text=True,
                           env={'PATH': '', 'DM_TARGET': 'android', 'DM_PATCHES_EXTRA': str(extra)}, cwd=self.tmp)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('DM_PATCHES_EXTRA files hold CUTS only', r.stderr)


class Encoders(unittest.TestCase):
    def test_bl_and_cbnzw(self):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        for frm, to in ((0x1000, 0x2000), (0x2000, 0x1000)):
            bl = next(md.disasm(struct.pack('<I', BL(frm, to)), frm))
            self.assertEqual((bl.mnemonic, bl.op_str), ('bl', f'#{to:#x}'))
            cb = next(md.disasm(struct.pack('<I', CBNZW(16, frm, to)), frm))
            self.assertEqual((cb.mnemonic, cb.op_str), ('cbnz', f'w16, #{to:#x}'))
        with self.assertRaises(AssertionError): CBNZW(0, 0, 1 << 20)


if __name__ == '__main__':
    unittest.main()
