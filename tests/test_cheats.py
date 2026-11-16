"""porter cheats: the build ID of an installed port, the files listed, and a pushed file named for the build."""
import tempfile
import unittest
from pathlib import Path

from porter import cheats, datapack
from porter.upstream import BuildError

BUILD = '0123456789ABCDEF'
OTHER = 'FEDCBA9876543210'
PKG = 'org.nx2a.test'
FILES = datapack.files_folder(PKG)
ROOT = f'{FILES}/cheats'


class FakeAdb:
    def __init__(self, files):
        self.files = dict(files)   # device path -> bytes
        self.said = []

    def read_text(self, path):
        v = self.files.get(path, b'')
        return v.decode('utf-8', 'replace') if isinstance(v, bytes) else v

    def remote_sizes(self, rdir):
        pre = rdir.rstrip('/') + '/'
        return {p[len(pre):]: len(v) for p, v in self.files.items() if p.startswith(pre)}

    def shell(self, script, **kw):
        self.said.append(script)
        return ''

    def push_file(self, src, dst):
        self.said.append(f'push {dst}')
        self.files[dst] = Path(src).read_bytes()


def adb_with(*names):
    files = {f'{FILES}/mods/main.fixups.tsv': f'# BUILD_ID\t{BUILD}\n'.encode()}
    for p, n in names: files[f'{ROOT}/{p}'] = b'x' * n
    return FakeAdb(files)


class Cheats(unittest.TestCase):
    def test_build_id_comes_from_the_installed_main(self):
        self.assertEqual(cheats.build_id(adb_with(), PKG), BUILD)
        with self.assertRaisesRegex(BuildError, 'no build ID'):
            cheats.build_id(FakeAdb({}), PKG)

    def test_build_id_takes_the_first_16_of_the_full_nso_id(self):
        adb = adb_with()
        adb.files[f'{FILES}/mods/main.fixups.tsv'] = f'# BUILD_ID\t{BUILD}0123456789ABCDEF\n'.encode()
        self.assertEqual(cheats.build_id(adb, PKG), BUILD)

    def test_listing_marks_this_build_and_counts_the_enabled_names(self):
        adb = adb_with((f'{BUILD}.txt', 100), (f'{OTHER}.txt', 50), (f'{BUILD}.enabled', 13))
        adb.files[f'{ROOT}/{BUILD}.enabled'] = b'Infinite money\nMax items\n'
        out = cheats.listing(adb, PKG)
        self.assertEqual(out['build'], BUILD); self.assertEqual(out['folder'], ROOT)
        rows = {r['build']: r for r in out['files']}
        self.assertTrue(rows[BUILD]['current']); self.assertEqual(rows[BUILD]['enabled'], 2); self.assertEqual(rows[BUILD]['bytes'], 100)
        self.assertFalse(rows[OTHER]['current']); self.assertEqual(rows[OTHER]['enabled'], 0)

    def test_install_names_the_file_for_the_build_and_keeps_the_old_one(self):
        adb = adb_with((f'{BUILD}.txt', 3))
        with tempfile.TemporaryDirectory() as t:
            src = Path(t) / 'my cheats.txt'; src.write_text('04000000 00000000 00000001\n')
            out = cheats.install(adb, PKG, src)
            self.assertEqual(out['build'], BUILD); self.assertEqual(out['file'], f'{BUILD}.txt'); self.assertTrue(out['backup'])
            self.assertEqual(adb.files[f'{ROOT}/{BUILD}.txt'], src.read_bytes())
            self.assertTrue(any(f'mv -f {ROOT}/{BUILD}.txt {ROOT}/{BUILD}.txt.bak' in s for s in adb.said))

    def test_a_file_named_for_another_build_is_refused_unless_asked(self):
        adb = adb_with()
        with tempfile.TemporaryDirectory() as t:
            src = Path(t) / f'{OTHER}.txt'; src.write_text('04000000 00000000 00000001\n')
            with self.assertRaisesRegex(BuildError, f'for build {OTHER}.*runs {BUILD}'):
                cheats.install(adb, PKG, src)
            out = cheats.install(adb, PKG, src, any_build=True)
            self.assertEqual(out['build'], OTHER)
            self.assertIn(f'{ROOT}/{OTHER}.txt', adb.files)

    def test_an_anchored_toml_is_named_as_not_compiled(self):
        with tempfile.TemporaryDirectory() as t:
            src = Path(t) / 'game.cheats.toml'; src.write_text('')
            with self.assertRaisesRegex(BuildError, 'cheat_anchor.py'):
                cheats.install(adb_with(), PKG, src)

    def test_only_16_hex_files_are_cheats(self):
        adb = adb_with(('readme.txt', 3), ('notes.md', 3), (f'{BUILD}.txt', 3))
        out = cheats.listing(adb, PKG)
        self.assertEqual([r['name'] for r in out['files']], [f'{BUILD}.txt'])


if __name__ == '__main__':
    unittest.main()