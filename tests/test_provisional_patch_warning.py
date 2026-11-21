"""External profile patches must not be silently mistaken for applied cuts."""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from porter.upstream import game, nsp

REPO = Path(__file__).resolve().parents[1] / 'nxnative'


def fixture_nso(path):
    image = bytearray(0x200)
    struct.pack_into('<II', image, 0, 0x14000004, 0x60)
    struct.pack_into('<I', image, 0x10, 0xd503201f)
    image[0x60:0x64] = b'MOD0'
    struct.pack_into('<i', image, 0x64, 0x40)
    struct.pack_into('<ii', image, 0x70, 0x20, 0x2c)
    image[0x80:0x84] = bytes((1, 3, 3, 3))
    struct.pack_into('<qQqQ', image, 0xa0, 6, 0x100, 5, 0x130)
    header = bytearray(0x100); header[:4] = b'NSO0'
    struct.pack_into('<III4xIII4xIII', header, 0x10, 0x100, 0, 0x40, 0x140, 0x40, 0x60, 0x1a0, 0xa0, 0x160)
    header[0x40:0x50] = bytes(range(16))
    struct.pack_into('<III', header, 0x60, 0x40, 0x60, 0x160)
    path.write_bytes(header + image)


class ProvisionalPatchWarning(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='nx2a_patch_warning_'); self.addCleanup(temp.cleanup)
        self.root = Path(temp.name); self.exe = self.root / 'ExeFS'; self.exe.mkdir()
        (self.root / 'RomFS').mkdir(); fixture_nso(self.exe / 'main')
        profile = self.root / 'profile/0100000000AAA000'; profile.mkdir(parents=True)
        (profile / 'game.tsv').write_text('title_id\t0100000000AAA000\nmodules\tmain\n', encoding='utf-8')
        self.g = game.Game(profile); self.local = self.g.table('patches.py')
        self.local.write_text("CUTS = {'000102030405060708090a0b0c0d0e0f': [(0x10, '1f2003d5', 'c0035fd6', 'synthetic cut')]}\n", encoding='utf-8')
        self.env = dict(os.environ, DM_TARGET='android'); self.env.pop('DM_PATCHES_EXTRA', None)

    def rewrite(self, repo=REPO):
        with patch.object(nsp.PROG, 'say') as say, patch.object(nsp.PROG, 'end') as end:
            nsp.rewrite(sys.executable, repo, self.exe, self.root / 'patched', self.env, self.root / 'logs', self.g)
        return [str(c.args[0]) for c in say.call_args_list], end

    def test_warns_on_fresh_and_reused_rewrite_without_applying_local_cuts(self):
        for reused in (False, True):
            messages, end = self.rewrite()
            self.assertTrue(any('[RED]' in s and str(self.local) in s and 'dm/games' in s for s in messages), messages)
            self.assertFalse(any('DM_PATCHES_EXTRA' in s for s in messages), messages)
            self.assertEqual((self.root / 'patched/main.patched.bin').read_bytes()[0x10:0x14], bytes.fromhex('1f2003d5'))
            cuts = (self.root / 'patched/main.cuts.tsv').read_text(encoding='utf-8').splitlines()
            self.assertFalse([s for s in cuts if s.strip() and not s.startswith('#')])
            self.assertEqual('reused' in str(end.call_args), reused)

    def test_explicit_extra_patch_is_applied_without_ignored_warning(self):
        self.env['DM_PATCHES_EXTRA'] = str(self.local)
        messages, _ = self.rewrite()
        self.assertFalse(any(str(self.local) in s for s in messages), messages)
        self.assertEqual((self.root / 'patched/main.patched.bin').read_bytes()[0x10:0x14], bytes.fromhex('c0035fd6'))

    def test_no_local_file_does_not_warn(self):
        self.local.unlink()
        messages, _ = self.rewrite()
        self.assertFalse(any('not loaded automatically' in s for s in messages), messages)

    def test_registered_profile_patch_does_not_warn(self):
        registered = next((REPO / 'dm/games').glob('*/patches.py'))
        with patch.object(self.g, 'table', return_value=registered):
            messages, _ = self.rewrite()
        self.assertFalse(any('not loaded automatically' in s for s in messages), messages)


if __name__ == '__main__': unittest.main()
