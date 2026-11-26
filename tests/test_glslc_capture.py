"""Runtime-shader preparation with real capture files and transport replaced at the adb boundary."""
from pathlib import Path
import shutil
import os
import shlex
import subprocess
import signal
import time
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
            dest.write_text('fixture guest compiled shaders\n', encoding='utf-8')
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
        self.map.write_text(f'x\t0\tvertex\tkey\t/glslc_capture/{NAME}#GLSLC@0\n', encoding='utf-8')
        p.g = SimpleNamespace(id='0100000000010000', table=lambda _: self.map, words=lambda _: ['main', 'subsdk0', 'sdk'], get=lambda k, d=None: d)
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


    def test_large_romfs_files_use_sparse_standins_and_profile_map_is_copied(self):
        self.port.GLSLC_SMALL = 8
        large = self.port.game / 'RomFS/large file.bin'
        large.write_bytes(b'x' * 9)
        staged = {}
        push = self.adb.push_tree
        def inspect(src, dst, **kw):
            if dst.endswith('/RomFS'):
                staged.update({p.relative_to(src).as_posix(): p.read_bytes() for p in Path(src).rglob('*') if p.is_file()})
            push(src, dst, **kw)
        with patch.object(self.adb, 'push_tree', side_effect=inspect):
            self.port.device_glslc()
        self.assertEqual(staged, {'file': b'data'})
        self.assertEqual(large.read_bytes(), b'x' * 9)
        self.assertTrue(any(c[0] == 'shell' and 'truncate -s 9' in c[1] and 'large file.bin' in c[1] for c in self.adb.calls))
        self.assertTrue(any(c[0] == 'push_file' and c[1] == self.map and c[2].endswith('/shader_map.tsv') for c in self.adb.calls))

    @unittest.skipUnless(os.name == 'posix', 'POSIX shell required')
    def test_capture_wait_stops_and_reaps_host_when_expected_files_arrive(self):
        self.port.device_glslc()
        run = next(c[1] for c in self.adb.calls if c[0] == 'shell' and 'timeout ' in c[1])
        args = shlex.split(run)
        script = args[args.index('-c') + 1].removesuffix(';')
        work = self.root / 'shell'; (work / 'capture').mkdir(parents=True)
        host = work / 'dmhost'
        host.write_text('#!/bin/sh\necho x > capture/' + NAME + '\nexec sleep 60\n', encoding='utf-8')
        host.chmod(0o755)
        result = subprocess.run(['sh', '-c', script], cwd=work, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((work / 'capture' / NAME).is_file())


    @unittest.skipUnless(os.name == 'posix', 'POSIX shell required')
    def test_capture_wait_signal_stops_and_reaps_host_without_outputs(self):
        self.port.device_glslc()
        run = next(c[1] for c in self.adb.calls if c[0] == 'shell' and 'timeout ' in c[1])
        args = shlex.split(run); script = args[args.index('-c') + 1].removesuffix(';')
        work = self.root / 'signal'; (work / 'capture').mkdir(parents=True)
        host = work / 'dmhost'
        host.write_text('#!/bin/sh\necho $$ > host.pid\nexec sleep 60\n', encoding='utf-8')
        host.chmod(0o755)
        process = subprocess.Popen(['sh', '-c', script], cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        child = None
        try:
            deadline = time.monotonic() + 5
            while not (work / 'host.pid').exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            child = int((work / 'host.pid').read_text(encoding='utf-8'))
            process.terminate()
            self.assertEqual(process.wait(timeout=5), 124)
            with self.assertRaises(ProcessLookupError): os.kill(child, 0)
        finally:
            if child:
                try: os.kill(child, signal.SIGKILL)
                except ProcessLookupError: pass
            if process.poll() is None: process.kill()
            process.wait(timeout=5)

    def test_complete_capture_reuses_without_adb_or_build(self):
        self.capture(); self.port.tools.pop('adb')
        self.port.check_graphics(); self.port.device_glslc()
        self.adb_class.assert_not_called(); self.build.assert_not_called()

    def test_reused_capture_filters_truncated_extra_without_device(self):
        cap = self.capture(); (cap / 'broken.glslc').write_bytes(b'truncated')
        (cap / 'notes.txt').write_text('keep non-shader notes', encoding='utf-8')
        self.port.tools.pop('adb')
        self.port.check_graphics(); self.port.device_glslc()
        self.assertFalse((cap / 'broken.glslc').exists())
        self.assertEqual((cap / NAME).read_bytes(), output())
        self.assertTrue((cap / 'notes.txt').exists())
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
        self.assertIn('fixture guest', (self.port.logs / 'android_glslc.log').read_text(encoding='utf-8'))
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

    def test_transport_failure_recovers_log_before_cleanup(self):
        log = self.port.logs / 'android_glslc.log'; log.write_text('previous attempt', encoding='utf-8')
        self.adb.fail = True
        with self.assertRaisesRegex(pipeline.AdbError, 'device disconnected'): self.port.device_glslc()
        self.assertEqual(log.read_text(encoding='utf-8'), 'fixture guest compiled shaders\n')
        pull_index = next(i for i, c in enumerate(self.adb.calls) if c[0] == 'pull' and c[1].endswith('/run.log'))
        cleanup_index = next(i for i, c in enumerate(self.adb.calls) if c[0] == 'shell' and c[1].startswith('rm -rf '))
        self.assertLess(pull_index, cleanup_index)
        self.assertEqual(self.adb.calls[pull_index][3]['retries'], 1)

    def test_failed_log_recovery_preserves_error_and_marks_old_log_stale(self):
        log = self.port.logs / 'android_glslc.log'; log.write_text('previous attempt', encoding='utf-8')
        self.adb.fail = True
        with patch.object(self.adb, 'run', side_effect=pipeline.AdbError('log unavailable')):
            with self.assertRaisesRegex(pipeline.AdbError, 'device disconnected'): self.port.device_glslc()
        self.assertNotIn('previous attempt', log.read_text(encoding='utf-8'))
        self.assertIn('not retrieved', log.read_text(encoding='utf-8'))

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
        self.map.write_text('', encoding='utf-8')
        self.adb.files = {}
        with self.assertRaisesRegex(pipeline.BuildError, 'captured.*run-time shaders'): self.port.device_glslc()
        self.adb.files = {NAME: output()}; self.port.device_glslc()
        self.assertEqual((self.port.game / 'glslc_capture' / NAME).read_bytes(), output())


if __name__ == '__main__': unittest.main()
