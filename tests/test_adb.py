# porter/adb.py without a device: the shell's answers are made up (python -m unittest discover tests).
import sys, unittest
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


if __name__ == '__main__':
    unittest.main()
