# Per-build rows as profile data (dm/games/game.py BUILD_TABLES: fps_locks.tsv, ue_malloc.tsv): the header carries them only for a profile that has the file, and
# porter host accepts a port whose host was built when the rows were still in the core (porter/host.py MOVED_PREFIXES).
import hashlib, os, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / 'nxnative/dm/games'))
import game                 # noqa: E402
from porter import host     # noqa: E402

FFR = '0100606026F12000'   # a profile with both tables


class BuildTables(unittest.TestCase):
    def test_header_has_rows_only_with_the_file(self):
        h = host.profile_header(FFR).decode('utf-8')
        self.assertIn('#define DM_GAME_FPS_LOCKS { "84e182f72ee1da4f477804bd05b54c15", "Unreal t.MaxFPS", 0x8c29b00', h)
        self.assertIn('#define DM_GAME_FPS_LOCKS_N 2', h); self.assertIn('#define DM_GAME_UE_MALLOC_N 1', h)
        for gid in sorted(os.listdir(REPO / 'nxnative/dm/games')):
            d = REPO / 'nxnative/dm/games' / gid
            if not gid.startswith('0100') or (d / 'fps_locks.tsv').exists() or (d / 'ue_malloc.tsv').exists(): continue
            self.assertNotIn(b'DM_GAME_FPS_LOCKS', host.profile_header(gid)); break   # one profile without them is enough

    def test_every_row_is_for_a_build_the_profile_registers(self):
        import re
        for d in sorted((REPO / 'nxnative/dm/games').glob('0100*')):
            mains = set(re.findall(r'main=([0-9a-f]{32})', (d / 'game.tsv').read_text(encoding='utf-8'))) if (d / 'game.tsv').exists() else set()
            for fname in game.BUILD_TABLES:
                for row in game.build_table(game.load(d.name), fname):
                    self.assertIn(row[0], mains, f'{d.name}/{fname}: build {row[0]} is not a main build of the profile (game.tsv build rows)')

    def test_bad_rows_are_refused(self):
        with tempfile.TemporaryDirectory() as t:
            g = mock.Mock(); g.dir = Path(t)
            for bad in ('x' * 32 + '\tw\t0\t0\tFPS_F32\t1\t0', 'a' * 32 + '\tw\t0\t0\tFPS_F64\t1\t0', 'a' * 32 + '\tw\tzero\t0\tFPS_F32\t1\t0', 'a' * 32 + '\tw\t0\t0\tFPS_F32\t1'):
                (Path(t) / 'fps_locks.tsv').write_text(bad + '\n', encoding='utf-8')
                with self.assertRaises(SystemExit, msg=bad): game.build_table(g, 'fps_locks.tsv')

    def test_porter_host_accepts_a_host_built_before_the_move(self):
        header = host.profile_header(FFR).replace(b'\r\n', b'\n')
        before = host.without_moved(header)
        self.assertIsNotNone(before)
        manifest = {'version': next(iter(game.load(FFR).builds())), 'profile_h': hashlib.sha256(before).hexdigest()}
        host.check_profile('org.example.ffr', manifest, FFR)   # no BuildError
        with self.assertRaises(host.BuildError):
            host.check_profile('org.example.ffr', dict(manifest, profile_h='0' * 64), FFR)


if __name__ == '__main__':
    unittest.main()
