"""Runtime-shader preparation with real capture files and transport replaced at the adb boundary."""
from pathlib import Path
import shutil
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from porter import pipeline

NAME = '0123456789abcdef.glslc'
OTHER = 'fedcba9876543210.glslc'


def output():
    data = bytearray(0x48)
    struct.pack_into('<I', data, 0, 0x19866891)
    struct.pack_into('<I', data, 0x44, len(data))
    return bytes(data)


class Device:
    def __init__(self):
        self.calls = []
        self.files = {NAME: output()}
        self.fail = False
        self.serial = 'fixture'

    def pick(self):
        self.calls.append(('pick',))
        return self.serial

    def shell(self, script, **kw):
        self.calls.append(('shell', script, kw))
        if 'timeout ' in script:
            if self.fail: raise pipeline.AdbError('device disconnected')
            return 'NX2A_GLSLC_EXIT=124\n'
        return ''

    def push_file(self, src, dst, **kw):
        self.calls.append(('push_file', Path(src), dst))

    def push_tree(self, src, dst, **kw):
        self.calls.append(('push_tree', Path(src), dst))

    def run(self, command, remote, local, **kw):
        self.calls.append((command, remote, Path(local), kw))
        assert command == 'pull'
        dest = Path(local)
        if remote.endswith('/run.log'):
            dest.write_text('fixture guest compiled shaders\n')
        else:
            dest.mkdir(parents=True)
            for name, data in self.files.items(): (dest / name).write_bytes(data)
        return ''


class GlslcCapture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nx2a-glslc-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        p = self.port = object.__new__(pipeline.Port)
        p.work = self.root / 'build'; p.aw = p.work / 'android'; p.logs = p.work / 'logs'
        p.logs.mkdir(parents=True)
        p.versions = ['v0']; p.pkg_ver = 'v0'; p.dirs = {'v0': self.root / 'extract/v0'}; p.game = p.dirs['v0']
        (p.game / 'RomFS').mkdir(parents=True)
        (p.game / 'RomFS/file').write_bytes(b'data')
        p.patched = p.aw / 'patched_v0'; p.patched.mkdir(parents=True)
        p.o = pipeline.Options(folder=self.root); p.cfg = {}; p.env = {}; p.app_name = 'Fixture'
        p.tools = {'adb': 'fixture-adb', 'ndk': 'fixture-ndk'}; p.glslc_why = 'runtime GLSLC outputs'
        self.map = self.root / 'shader_map.tsv'
        self.map.write_text(f'x\t0\tvertex\tkey\t/glslc_capture/{NAME}#GLSLC@0\n')
        p.g = SimpleNamespace(id='0100000000010000', table=lambda _: self.map, words=lambda _: ['main', 'subsdk0', 'sdk'])
        self.src = {k: (False, '') for k in ('glslc_capture', 'oodle_packs', 'ue_decode', 'ue_legacy')}
        self.src.update(graphics=('nvn', 'NVN'), glslc_capture=(True, p.glslc_why))
        self.adb = Device()
        self.addCleanup(patch.stopall)
        self.adb_class = patch.object(pipeline, 'Adb', return_value=self.adb).start()
        patch.object(pipeline.profile_guess, 'build_sources', return_value=self.src).start()
        self.build = patch.object(pipeline.nsp, 'run').start()
        patch.object(pipeline, 'PROG').start()
        patch.object(pipeline, 'say').start()

    def capture(self, data=None):
        dest = self.port.game / 'glslc_capture'; dest.mkdir(exist_ok=True)
        (dest / NAME).write_bytes(output() if data is None else data)
        return dest

    def test_complete_capture_reuses_without_adb_or_build(self):
        self.capture(); self.port.tools.pop('adb')
        self.port.check_graphics(); self.port.device_glslc()
        self.adb_class.assert_not_called(); self.build.assert_not_called()

    def test_missing_capture_needs_adb_even_without_install(self):
        self.port.tools.pop('adb')
        with self.assertRaisesRegex(pipeline.BuildError, 'adb'):
            self.port.check_graphics()

    def test_missing_capture_uses_device_instead_of_mac_error(self):
        self.port.check_graphics()
        self.assertTrue(self.port.glslc_why)
        self.assertIn(('pick',), self.adb.calls)

    def test_missing_other_version_is_not_captured_with_wrong_modules(self):
        self.port.versions.insert(0, 'v1'); self.port.dirs['v1'] = self.root / 'extract/v1'
        with self.assertRaisesRegex(pipeline.BuildError, 'v1.*capture of its own'):
            self.port.device_glslc()
        self.adb_class.assert_not_called()

    def test_success_publishes_and_cleans_private_device_directory(self):
        old = self.capture(b'incomplete'); (old / 'stale.glslc').write_bytes(b'old')
        saved = self.port.work / 'save'; saved.mkdir(); (saved / 'keep').write_bytes(b'user save')
        self.port.device_glslc()
        self.assertEqual(list(old.iterdir()), [old / NAME])
        self.assertEqual((old / NAME).read_bytes(), output())
        self.assertEqual((saved / 'keep').read_bytes(), b'user save')
        self.assertIn('fixture guest', (self.port.logs / 'android_glslc.log').read_text())
        args = self.build.call_args.args[0]
        self.assertIn('--exe', args); self.assertEqual(args[args.index('--gfx') + 1], 'null')
        run = next(c for c in self.adb.calls if c[0] == 'shell' and 'timeout ' in c[1])
        self.assertEqual(run[2]['retries'], 1)
        self.assertIn('DM_SAVE=', run[1]); self.assertIn('DM_TEMP=', run[1])
        remote = next(c[2].rsplit('/', 1)[0] for c in self.adb.calls if c[0] == 'push_file')
        self.assertTrue(remote.startswith('/data/local/tmp/nx2a-glslc-'))
        self.assertTrue(any(c[0] == 'shell' and c[1] == f'rm -rf {remote}' for c in self.adb.calls))
        self.assertEqual(list(self.port.game.glob('.glslc-*')), [])

    def test_incomplete_or_corrupt_capture_preserves_previous_directory(self):
        for files in ({}, {NAME: b'truncated'}, {OTHER: output()}):
            with self.subTest(files=list(files)):
                old = self.capture(b'previous bytes'); self.adb.files = files
                with self.assertRaisesRegex(pipeline.BuildError, 'captured.*run-time shaders'):
                    self.port.device_glslc()
                self.assertEqual((old / NAME).read_bytes(), b'previous bytes')
                self.assertEqual(list(self.port.game.glob('.glslc-*')), [])

    def test_transport_failure_preserves_capture_and_cleans(self):
        old = self.capture(b'previous'); self.adb.fail = True
        with self.assertRaises(pipeline.AdbError): self.port.device_glslc()
        self.assertEqual((old / NAME).read_bytes(), b'previous')
        self.assertTrue(self.adb.calls[-1][1].startswith('rm -rf /data/local/tmp/nx2a-glslc-'))

    def test_concurrent_runs_get_distinct_remote_directories(self):
        self.port.device_glslc()
        shutil.rmtree(self.port.game / 'glslc_capture')
        self.port.device_glslc()
        hosts = [c[2] for c in self.adb.calls if c[0] == 'push_file' and c[2].endswith('/dmhost')]
        self.assertEqual(len(hosts), 2); self.assertNotEqual(*hosts)

    def test_failed_publication_restores_previous_capture(self):
        old = self.capture(b'previous')
        rename = Path.rename
        def fail_publish(path, dest):
            if path.name == 'capture': raise OSError('rename failed')
            return rename(path, dest)
        with patch.object(Path, 'rename', fail_publish), self.assertRaisesRegex(OSError, 'rename failed'):
            self.port.device_glslc()
        self.assertEqual((old / NAME).read_bytes(), b'previous')
        self.assertEqual(list(self.port.game.glob('.glslc-*')), [])

    def test_invalid_extra_output_is_not_published(self):
        self.adb.files['broken.glslc'] = b'truncated'
        self.port.device_glslc()
        self.assertEqual([p.name for p in (self.port.game / 'glslc_capture').iterdir()], [NAME])

    def test_capture_runs_before_shader_split(self):
        self.port.oodle_why = ''; self.port.ue_why = ''; self.port.derived = self.port.provisional = False
        self.port.aoc = None
        class SplitReached(Exception): pass
        def split(*args, **kwargs):
            self.assertEqual((self.port.game / 'glslc_capture' / NAME).read_bytes(), output())
            raise SplitReached()
        with patch.object(pipeline.nsp, 'split_shaders', side_effect=split), patch.object(self.port, 'device_oodle_end'):
            with self.assertRaises(SplitReached): self.port.shaders()

    def test_provisional_profile_requires_at_least_one_complete_output(self):
        self.map.write_text('')
        self.adb.files = {}
        with self.assertRaisesRegex(pipeline.BuildError, 'captured.*run-time shaders'): self.port.device_glslc()
        self.adb.files = {NAME: output()}; self.port.device_glslc()
        self.assertEqual((self.port.game / 'glslc_capture' / NAME).read_bytes(), output())


if __name__ == '__main__': unittest.main()
