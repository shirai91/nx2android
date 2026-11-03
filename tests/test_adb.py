# porter/adb.py without a device: the shell's answers are made up (python -m unittest discover tests).
import sys, tarfile, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from porter.adb import Adb, AdbError   # noqa: E402


class RemoteSizes(unittest.TestCase):
    class FakeAdb(Adb):
        def __init__(self, out): super().__init__('adb'); self.out, self.cmds = out, []
        def shell(self, script, **kw): self.cmds.append(script); return self.out

    def test_printf_listing(self):   # find -printf's %P: no ./ prefix; names with spaces kept whole
        adb = self.FakeAdb('12 RomFS/Data/Resources/unity default resources\n3 main\n0 RomFS/empty.bin\nrc=0\n')
        self.assertEqual(adb.remote_sizes('/sdcard/x'), {'RomFS/Data/Resources/unity default resources': 12, 'main': 3, 'RomFS/empty.bin': 0})
        self.assertIn("-printf '%s %P\\n'", adb.cmds[0])

    def test_stat_listing(self):   # Android 10's toybox has no -printf: -exec stat's ./ names
        self.assertEqual(self.FakeAdb('5 ./a/b c\n7 ./d\nrc=0\n').remote_sizes('/sdcard/x'), {'a/b c': 5, 'd': 7})

    def test_missing_folder(self):   # no folder: no output at all
        self.assertEqual(self.FakeAdb('').remote_sizes('/sdcard/x'), {})

    def test_find_error(self):   # a find that fails is an error, not a short listing
        with self.assertRaisesRegex(AdbError, 'Argument list too long'):
            self.FakeAdb('5 ./a\nfind: exec stat: Argument list too long\n7 ./d\nrc=1\n').remote_sizes('/sdcard/x')
        with self.assertRaisesRegex(AdbError, 'no exit status'):   # cut short
            self.FakeAdb('5 a\n7 d\n').remote_sizes('/sdcard/x')


class PushTars(unittest.TestCase):
    class FakeAdb(Adb):   # pushes are unpacked into a local folder standing in for the device's
        def __init__(self, dev, rc='rc=0'): super().__init__('adb'); self.dev, self.rc, self.got = Path(dev), rc, []
        def run(self, *a, **kw):
            if a[0] == 'push':
                with tarfile.open(a[2]) as t: self.got.append(sorted(t.getnames())); t.extractall(self.dev, filter='data')
            return ''
        def shell(self, script, **kw): return self.rc if 'tar -xf' in script else ''

    def test_archives(self):   # every file once, by size limits; long names kept whole
        import porter.adb as m
        with tempfile.TemporaryDirectory() as t:
            src, small = Path(t) / 'data', []
            for i in range(5):
                rel = f'RomFS/{"d" * 120}/f{i}.bin'; p = src / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(bytes([i]) * (i + 1)); small.append((rel, p, i + 1))
            adb, old = self.FakeAdb(Path(t) / 'dev'), (m.TAR_FILES, m.TAR_BYTES)
            m.TAR_FILES, m.TAR_BYTES = 2, 1 << 20
            try: self.assertEqual(adb.push_tars(src, '/sdcard/x', small), 15)
            finally: m.TAR_FILES, m.TAR_BYTES = old
            self.assertEqual([len(g) for g in adb.got], [2, 2, 1])
            for rel, p, s in small: self.assertEqual((Path(t) / 'dev' / rel).read_bytes(), p.read_bytes())
            self.assertEqual([q.name for q in Path(t).iterdir() if q.name.startswith('.nx2a_tar_')], [])   # the archives' folder is gone

    def test_unpack_error(self):   # a tar that fails on the device is an error
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'data' / 'a'; p.parent.mkdir(); p.write_bytes(b'x')
            with self.assertRaisesRegex(AdbError, 'unpacking'): self.FakeAdb(Path(t) / 'dev', 'tar: short read\nrc=1').push_tars(p.parent, '/sdcard/x', [('a', p, 1)])


if __name__ == '__main__':
    unittest.main()
