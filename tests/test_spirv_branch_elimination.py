"""Regression tests for Ryujinx BranchElimination against PhiNode merge targets.

With DebugMode off (Android's shader build), Optimizer.PropagateCopy folds a
single-copy fallthrough block (for example the predicated-SYNC tail in
NieR:Automata's Forward+ fragment shaders) into the merge block's PhiNode,
leaving only an unconditional Branch. BranchElimination must not drop the
conditional branch to that merge block while it carries a PhiNode, or execution
falls straight into the fallthrough block's phi copy and unconditionally
clobbers the branch path's value.
"""
import json, os, shutil, struct, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


# Maxwell instruction encoders (same ISA bits as Ryujinx InstTable / InstDecoders).
def _mov_c(dest, slot, word_off):
    return (0x4c98 << 48) | (0xf << 39) | (slot << 34) | (word_off << 20) | (7 << 16) | dest


def _mov_r(dest, src):
    return (0x5c98 << 48) | (0xf << 39) | (src << 20) | (7 << 16) | dest


def _isetp_eq_c(dest_pred, src_reg, slot, word_off):
    # ISETP.EQ.AND Pd, PT, Ra, c[slot][word_off*4], PT
    return (0x4b65 << 48) | (7 << 39) | (slot << 34) | (word_off << 20) | (7 << 16) | (src_reg << 8) | (dest_pred << 3) | 7


def _bra(pc, target, pred=7, inv=False):
    rel = (target - (pc + 8)) & 0xffffff
    return (0xe240 << 48) | (rel << 20) | ((1 if inv else 0) << 19) | (pred << 16) | 0xf


def _ssy(pc, target):
    rel = (target - (pc + 8)) & 0xffffff
    return (0xe290 << 48) | (rel << 20)


def _sync(pred=7, inv=False):
    return (0xf0f8 << 48) | ((1 if inv else 0) << 19) | (pred << 16) | 0xf


_EXIT = 0xe30000000007000f


def _build_frag(ops):
    """Pack Maxwell instructions after a 0x50-byte fragment SPH (RT0 RGBA output)."""
    # Insert a zero scheduling word at every 32-byte boundary (pc & 0x1f == 0).
    words = []
    for op in ops:
        if (len(words) * 8) & 0x1f == 0:
            words.append(0)
        words.append(op)
    code = bytearray(0x50 + len(words) * 8)
    # SPH: fragment stage (type 1, stage 5 -> 0x04065462), RT0 mask 0xf at +0x48.
    struct.pack_into('<I', code, 0x00, 0x04065462)
    struct.pack_into('<I', code, 0x48, 0x0000000f)
    for i, w in enumerate(words):
        struct.pack_into('<Q', code, 0x50 + i * 8, w)
    return bytes(code)


def _phi_sync_shader():
    # Exact control-flow diamond from NieR:Automata's Forward+ spotlight shadow tail:
    #   0x08: MOV R0, c[0][0x0]        ; path A value
    #   0x10: MOV R1, c[0][0x4]        ; path B value (fallthrough copy)
    #   0x18: MOV R2, c[0][0x8]        ; selector
    #   0x28: MOV R3, c[0][0xc]        ; else-branch value
    #   0x30: ISETP.EQ P1, R2, c[0][0x10]
    #   0x38: SSY 0x88                 ; merge block (carries Phi for R0)
    #   0x48: @!P1 BRA 0x70            ; jump to else block so SYNC at 0x68 is not +8 fallthrough
    #   0x50: ISETP.EQ P0, R2, c[0][0x14]
    #   0x58: @!P0 SYNC                ; if !P0, merge at 0x88 keeping R0
    #   0x68: MOV R0, R1               ; single-copy fallthrough (folded into Phi at 0x88)
    #   0x70: SYNC                     ; unconditional Branch(0x88) — wait: align else at 0x78!
    #   0x78: MOV R0, R3               ; else block
    #   0x88: EXIT                     ; merge block (outputs R0..R3)
    return _build_frag([
        _mov_c(0, 0, 0),            # 0x08
        _mov_c(1, 0, 1),            # 0x10
        _mov_c(2, 0, 2),            # 0x18
        _mov_c(3, 0, 3),            # 0x28
        _isetp_eq_c(1, 2, 0, 4),    # 0x30
        _ssy(0x38, 0x88),           # 0x38
        _bra(0x48, 0x78, pred=1, inv=True),  # 0x48 -> else at 0x78
        _isetp_eq_c(0, 2, 0, 5),    # 0x50
        _sync(pred=0, inv=True),    # 0x58 -> merge at 0x88 if !P0
        _mov_r(0, 1),               # 0x68 (fallthrough block: copy-folded into Phi)
        _sync(),                    # 0x70 -> Branch(0x88)
        _mov_r(0, 3),               # 0x78 (else block)
        _EXIT,                      # 0x88 (merge block: carries Phi for R0)
    ])


def _nophi_sync_shader():
    # Same diamond, except the fallthrough block leaves R0 untouched and the
    # else branch is absent from the Phi (R0 is identical on all paths), so the
    # merge block carries no PhiNode and the conditional SYNC is genuinely redundant.
    return _build_frag([
        _mov_c(0, 0, 0),            # 0x08
        _mov_c(2, 0, 2),            # 0x10
        _isetp_eq_c(1, 2, 0, 4),    # 0x18
        _ssy(0x28, 0x68),           # 0x28
        _bra(0x30, 0x58, pred=1, inv=True),  # 0x30 -> else at 0x58
        _isetp_eq_c(0, 2, 0, 5),    # 0x38
        _sync(pred=0, inv=True),    # 0x48 -> merge at 0x68
        _sync(),                    # 0x50 -> Branch(0x68), no Phi at 0x68
        _sync(),                    # 0x58 -> Branch(0x68), no Phi at 0x68
        _EXIT,                      # 0x68 (merge block: R0 unchanged on all paths)
    ])


@unittest.skipUnless(shutil.which('dotnet'), 'the shader translator needs .NET')
class BranchEliminationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_braelim_')
        cls.out = Path(cls.tmp.name)
        src = ROOT / 'nxnative/dm/gfx/tools/dmsh_xlate'
        cls.env = os.environ.copy()
        cls.env.setdefault('DOTNET_ROLL_FORWARD', 'Major')
        subprocess.run(
            ['dotnet', 'build', str(src), '-c', 'Release', '-o', str(cls.out / 'xlate'), '-nologo', '-v', 'quiet'],
            check=True, capture_output=True, env=cls.env,
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def _translate(self, key, code_bytes):
        binary = self.out / f'{key}.bin'
        binary.write_bytes(code_bytes)
        job = dict(key=key, stage='ps', code=str(binary), tex={}, img={}, attr={}, fout={}, debug=False, reduced=True)
        jobs = self.out / f'{key}.json'
        jobs.write_text(json.dumps([job]), encoding='utf-8')
        subprocess.run(
            ['dotnet', str(self.out / 'xlate/dmsh_xlate.dll'), str(jobs), str(self.out)],
            check=True, capture_output=True, env=self.env,
        )
        data = (self.out / f'{key}.spv').read_bytes()
        words = struct.unpack(f'<{len(data) // 4}I', data)
        insts = []
        i = 5
        while i < len(words):
            n, op = words[i] >> 16, words[i] & 0xffff
            self.assertGreater(n, 0)
            insts.append((op, words[i + 1:i + n]))
            i += n
        return insts

    def test_predicated_sync_into_phi_target_is_preserved(self):
        insts = self._translate('phi_sync', _phi_sync_shader())
        # Both comparisons (P1 for the outer if, P0 for @!P0 SYNC) and both
        # conditional branches must survive. Without the BranchElimination Phi
        # guard, the inner comparison and branch are dropped and the phi var is
        # stored twice in the same basic block (unconditionally clobbered by R1).
        iequals = [a for op, a in insts if op == 170]       # OpIEqual
        branches = [a for op, a in insts if op == 250]      # OpBranchConditional
        self.assertGreaterEqual(len(iequals), 2, f'inner predicate comparison dropped: {len(iequals)} OpIEqual')
        self.assertGreaterEqual(len(branches), 2, f'predicated SYNC branch eliminated: {len(branches)} OpBranchConditional')
        stores_by_var = {}
        block = 0
        for op, args in insts:
            if op == 248:  # OpLabel
                block = args[0]
            elif op == 62:  # OpStore ptr val
                stores_by_var.setdefault(args[0], []).append(block)
        for var, blocks in stores_by_var.items():
            self.assertEqual(len(blocks), len(set(blocks)),
                             f'variable %{var} overwritten twice in the same block: {blocks}')

    def test_redundant_branch_without_phi_is_still_eliminated(self):
        # The inner @!P0 SYNC -> 0x68 has fallthrough SYNC -> 0x68 with no Phi at
        # 0x68, so BranchElimination must still drop the inner branch and DCE P0
        # (leaving only the outer @!P1 BRA 0x58: 1 OpIEqual, 1 OpBranchConditional).
        insts = self._translate('nophi_sync', _nophi_sync_shader())
        iequals = [a for op, a in insts if op == 170]
        branches = [a for op, a in insts if op == 250]
        self.assertEqual(len(iequals), 1, f'inner no-Phi predicate was not eliminated: {len(iequals)} OpIEqual')
        self.assertEqual(len(branches), 1, f'inner no-Phi branch was not eliminated: {len(branches)} OpBranchConditional')


if __name__ == '__main__':
    unittest.main()
