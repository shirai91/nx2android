# porter.paths.engine_copy: an AppImage's engine runs from one writable copy in the user folder, made again only for another AppImage.
import os, tempfile, unittest
from pathlib import Path
from unittest import mock

from porter import paths


class EngineCopy(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory(); self.addCleanup(t.cleanup); self.t = Path(t.name)
        self.bundle = self.t / 'mount'; (self.bundle / 'nxnative/dm').mkdir(parents=True)
        (self.bundle / 'nxnative/dm/tool.py').write_text('v1'); (self.bundle / 'VERSION').write_text('v0.9.0\n')
        self.image = self.t / 'nx2android.AppImage'; self.image.write_bytes(b'x' * 10)
        p = mock.patch.dict(os.environ, {'NX2A_HOME': str(self.t / 'home')}); p.start(); self.addCleanup(p.stop)

    def test_copied_once_and_kept(self):
        nx = paths.engine_copy(self.bundle, self.image)
        self.assertEqual(nx, self.t / 'home/engine/nxnative')
        self.assertEqual((nx / 'dm/tool.py').read_text(), 'v1')
        (nx / 'dm/obj').mkdir()   # a build's output in the copy stays for the next launch
        self.assertEqual(paths.engine_copy(self.bundle, self.image), nx)
        self.assertTrue((nx / 'dm/obj').is_dir())

    def test_another_image_replaces_the_copy(self):
        nx = paths.engine_copy(self.bundle, self.image); (nx / 'dm/obj').mkdir()
        (self.bundle / 'nxnative/dm/tool.py').write_text('v2'); (self.bundle / 'VERSION').write_text('v0.9.1\n'); self.image.write_bytes(b'x' * 11)
        nx = paths.engine_copy(self.bundle, self.image)
        self.assertEqual((nx / 'dm/tool.py').read_text(), 'v2')
        self.assertFalse((nx / 'dm/obj').exists())
        self.assertEqual([p.name for p in (self.t / 'home').iterdir()], ['engine'])   # no partial copy left

    def test_copy_is_newer_than_old_objects(self):   # an AppImage built before the last port's objects: its copy still makes the host build compile again
        tool = self.bundle / 'nxnative/dm/tool.py'; os.utime(tool, (1_000_000_000, 1_000_000_000))
        nx = paths.engine_copy(self.bundle, self.image)
        self.assertGreater((nx / 'dm/tool.py').stat().st_mtime, 1_000_000_000 + 86400)


if __name__ == '__main__':
    unittest.main()
