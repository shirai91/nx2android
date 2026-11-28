"""Where a Game Freak Oodle title's decoder runs (pipeline.check_graphics): the Mac host on macOS, which runs the game's own arm64 OodleLZ_Decompress natively and needs no
device, and the Android device everywhere else. The split's own Mac-host path (pipeline.mac_oodle) is exercised by a port; this covers the choice and its adb requirement."""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from porter import pipeline


class Profile(dict):
    """The profile object check_graphics reads: dict-like rows, an id and the modules list."""
    id = '0100F43008C44000'
    def words(self, key): return ['main', 'sdk']


class OodleRoute(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nx2a-oodle-route-')
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        p = self.port = object.__new__(pipeline.Port)
        p.work = root / 'build'; p.aw = p.work / 'android'; p.logs = p.work / 'logs'; p.logs.mkdir(parents=True)
        p.versions = ['v0']; p.pkg_ver = 'v0'; p.dirs = {'v0': root / 'extract/v0'}; p.game = p.dirs['v0']
        (p.game / 'RomFS').mkdir(parents=True)
        p.o = pipeline.Options(folder=root); p.cfg = {}; p.env = {}; p.app_name = 'Fixture'
        p.tools = {}   # no adb: a port that does not install may leave it out, but this game's shaders need it off a Mac
        p.g = Profile()
        self.src = {k: (False, '') for k in ('glslc_capture', 'oodle_packs', 'ue_decode', 'ue_legacy')}
        self.src.update(graphics=('nvn', 'NVN'), oodle_packs=(True, "1 Trinity pack file(s) (ONEPACK, arc/data.trpfs): Oodle-compressed"))
        for target, value in ((pipeline.profile_guess, 'build_sources'), (pipeline, 'say'), (pipeline, 'PROG')):
            patch.object(target, value, return_value=self.src if value == 'build_sources' else None).start()
        self.addCleanup(patch.stopall)

    def test_macos_decodes_in_the_mac_host_and_needs_no_device(self):
        with patch.object(pipeline.sys, 'platform', 'darwin'):
            self.port.check_graphics()
        self.assertTrue(self.port.oodle_mac)

    def test_other_platforms_need_the_device(self):
        with patch.object(pipeline.sys, 'platform', 'win32'):
            with self.assertRaisesRegex(pipeline.BuildError, 'needs adb'):
                self.port.check_graphics()
        self.assertFalse(self.port.oodle_mac)


if __name__ == '__main__':
    unittest.main()