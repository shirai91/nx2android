# porter/adb.py without a device: the shell's answers are made up (python -m unittest discover tests).
import hashlib, os, shlex, shutil, subprocess, sys, tarfile, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from porter.adb import Adb, AdbError   # noqa: E402


class MissingAdb(unittest.TestCase):   # issue #109: a port without platform-tools ran a program named "None" and ended in FileNotFoundError
    def test_no_exe(self):
        with self.assertRaisesRegex(AdbError, 'adb not found'): Adb(None)

    def test_exe_gone(self):   # a configured adb that is no longer there: an AdbError at once, no retries
        with tempfile.TemporaryDirectory() as t:
            adb = Adb(Path(t) / 'adb.exe', log=lambda s: self.fail(f'retried: {s}'))
            for f in (adb.devices, adb.pick, lambda: adb.shell('true')):
                with self.assertRaisesRegex(AdbError, 'cannot run adb'): f()


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


class SameVolume(unittest.TestCase):   # /sdcard is a symlink: stat must follow it (-L), or it gives the root file system's device number
    def test_follows_links(self):
        cmds = []
        class FakeAdb(Adb):
            def shell(self, script, **kw): cmds.append(script); return '64\n64\n'
        self.assertTrue(FakeAdb('adb').same_volume('/sdcard', '/sdcard/Games/x'))
        self.assertIn('stat -L -c %d', cmds[0])


class PushTree(unittest.TestCase):
    class FakeAdb(Adb):   # small pushes recorded by their device folder and file names
        def __init__(self): super().__init__('adb'); self.pushed = []
        def run(self, *a, **kw):
            if a[0] == 'push': self.pushed += [f'{a[-1]}{Path(p).name}' for p in a[2:-1]]
            return ''
        def shell(self, script, **kw): return ''

    def test_exclude(self):   # install --romfs-at: the pack's RomFS folder left out whole, and not counted as on the device
        with tempfile.TemporaryDirectory() as t:
            for rel in ('RomFS/Data/a.bin', 'RomFS/b.bin', 'mods/main', 'env.txt'): p = Path(t) / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b'x')
            adb, said = self.FakeAdb(), []; adb.log = said.append
            self.assertEqual(adb.push_tree(t, '/sdcard/f', exclude=('RomFS',), skip=('env.txt',), have={}), 1)
            self.assertEqual(adb.pushed, ['/sdcard/f/mods/main']); self.assertIn('(1 already on the device)', said[0])   # env.txt: skipped, as before


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


@unittest.skipUnless(os.name == 'posix' and shutil.which('sh'), 'the local device shell harness needs POSIX paths and sh')
class PushFile(unittest.TestCase):
    class LocalAdb(Adb):   # actual push_file, remote_md5 and join script; only adb's transport and Android stat/md5 output are substituted
        def __init__(self, dev):
            super().__init__('adb', log=lambda s: None)
            self.dev, self.pushed, self.fail_at, self.hashes, self.fail_join = Path(dev), [], None, True, False
            self.hash_calls, self.no_printf = [], False
        def remote_sizes(self, rdir):
            return {p.name: p.stat().st_size for p in Path(rdir).iterdir() if p.is_file()}
        def run(self, *a, **kw):
            if a[:2] != ('push', '-Z'): raise AssertionError(a)
            dst = Path(a[-1])
            if dst.name == self.fail_at: raise AdbError('interrupted transfer')
            shutil.copyfile(a[-2], dst); self.pushed.append(dst.name)
            return ''
        def shell(self, script, **kw):
            if str(self.dev) not in script: raise AssertionError(script)
            words = shlex.split(script)
            if words[:3] == ['stat', '-c', '%s']: return str(Path(words[3]).stat().st_size)
            if 'md5sum' in words:
                root, start, end = Path(words[1]), words.index('md5sum') + 1, words.index('2>/dev/null;')
                self.hash_calls.append((words[start:end], kw))
                if self.hashes == 'timeout': raise AdbError('adb shell: timed out')
                if not self.hashes: return ''
                return ''.join(f'{hashlib.md5((root / name).read_bytes()).hexdigest()}  {name}\n' for name in words[start:end] if (root / name).is_file())
            if words[:2] == ['rm', '-f'] and self.fail_join: raise AdbError('interrupted join')
            if self.no_printf: script = 'printf() { return 127; }\n' + script
            r = subprocess.run(['sh', '-c', script], capture_output=True, text=True)
            if r.returncode: raise AdbError(r.stderr)
            return r.stdout

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.src = Path(tmp.name) / 'source.bin'; self.dev = Path(tmp.name) / 'device'
        self.dev.mkdir(); self.dst = str(self.dev / "a file's bytes.bin"); self.adb = self.LocalAdb(self.dev)

    def interrupt(self, data, at='00001', chunk=4):
        self.src.write_bytes(data); self.adb.fail_at = at
        with self.assertRaisesRegex(AdbError, 'interrupted transfer'): self.adb.push_file(self.src, self.dst, chunk=chunk)
        self.adb.fail_at = None; self.adb.pushed.clear()

    def test_changed_source_replaces_same_size_piece(self):
        self.interrupt(b'AAAABBBB'); self.src.write_bytes(b'CCCCDDDD')
        self.adb.push_file(self.src, self.dst, chunk=4)
        self.assertEqual(Path(self.dst).read_bytes(), b'CCCCDDDD')
        self.assertEqual(self.adb.pushed, ['00000', '00001'])

    def test_unchanged_source_keeps_verified_piece(self):
        self.interrupt(b'AAAABBBB')
        self.adb.push_file(self.src, self.dst, chunk=4)
        self.assertEqual(Path(self.dst).read_bytes(), b'AAAABBBB')
        self.assertEqual(self.adb.pushed, ['00001'])

    def test_missing_hash_replaces_piece(self):
        self.interrupt(b'AAAABBBB'); self.adb.hashes = False
        self.adb.push_file(self.src, self.dst, chunk=4)
        self.assertEqual(Path(self.dst).read_bytes(), b'AAAABBBB')
        self.assertEqual(self.adb.pushed, ['00000', '00001'])

    def test_hash_timeout_resends_unverified_pieces(self):
        self.interrupt(b'AAAABBBB')
        self.adb.hashes = 'timeout'
        self.adb.push_file(self.src, self.dst, chunk=4)
        self.assertEqual(Path(self.dst).read_bytes(), b'AAAABBBB')
        self.assertEqual(self.adb.pushed, ['00000', '00001'])
        self.assertEqual(len(self.adb.hash_calls), 1)
        self.assertEqual(self.adb.hash_calls[0][1].get('retries'), 1)

    def test_resume_limits_hash_work_per_shell_call(self):
        data = bytes(range(40))
        self.interrupt(data, at='00009')
        self.adb.push_file(self.src, self.dst, chunk=4)
        self.assertEqual(Path(self.dst).read_bytes(), data)
        self.assertEqual(self.adb.pushed, ['00009'])
        self.assertGreater(len(self.adb.hash_calls), 1)
        self.assertEqual([name for names, _ in self.adb.hash_calls for name in names],
                         [f'{i:05d}' for i in range(9)])

    def test_many_pieces_join_in_order_without_printf(self):
        data = bytes(range(256)) + b'end'
        self.adb.no_printf = True
        self.src.write_bytes(data)
        self.adb.push_file(self.src, self.dst, chunk=1)
        self.assertEqual(Path(self.dst).read_bytes(), data)
    def test_shrinking_source_ignores_old_tail(self):
        self.interrupt(b'AAAABBBBCCCC', at='00002'); self.src.write_bytes(b'DDDD')
        self.adb.push_file(self.src, self.dst, chunk=4)
        self.assertEqual(Path(self.dst).read_bytes(), b'DDDD')
        self.assertEqual(list(self.dev.glob('.dmpart-*')), [])

    def test_changed_chunk_size_ignores_old_tail(self):
        self.interrupt(b'AAAABBBBCCCCDDDD', at='00003'); self.adb.push_file(self.src, self.dst, chunk=8)
        self.assertEqual(Path(self.dst).read_bytes(), b'AAAABBBBCCCCDDDD')

    def test_truncated_source_does_not_join(self):
        self.src.write_bytes(b'AAAABBBB'); Path(self.dst).write_bytes(b'previous complete file')
        def progress(done, total, note):
            if done == 4: self.src.write_bytes(b'AAAA')
        with self.assertRaisesRegex(AdbError, 'changed|short|truncat'): self.adb.push_file(self.src, self.dst, chunk=4, progress=progress)
        self.assertEqual(Path(self.dst).read_bytes(), b'previous complete file')

    def test_short_read_does_not_upload_incomplete_piece(self):
        self.src.write_bytes(b'A' * (2 << 20))
        def progress(done, total, note):
            if done == 1 << 20:
                with self.src.open('r+b') as f: f.truncate(1 << 20)
        with self.assertRaisesRegex(AdbError, 'changed|short|truncat'): self.adb.push_file(self.src, self.dst, chunk=1 << 20, progress=progress)
        self.assertEqual(self.adb.pushed, ['00000']); self.assertFalse(Path(self.dst).exists())

    def test_empty_source_replaces_previous_pieces(self):
        self.interrupt(b'AAAABBBBCCCC', at='00002'); self.src.write_bytes(b'')
        self.adb.push_file(self.src, self.dst, chunk=4)
        self.assertEqual(Path(self.dst).read_bytes(), b'')

    def test_retry_after_join_failure_reuses_pieces(self):
        self.src.write_bytes(b'AAAABBBB'); self.adb.fail_join = True
        with self.assertRaisesRegex(AdbError, 'interrupted join'): self.adb.push_file(self.src, self.dst, chunk=4)
        self.adb.fail_join = False; self.adb.pushed.clear()
        self.adb.push_file(self.src, self.dst, chunk=4)
        self.assertEqual(Path(self.dst).read_bytes(), b'AAAABBBB'); self.assertEqual(self.adb.pushed, [])


class LaunchActivity(unittest.TestCase):
    class FakeAdb(Adb):
        def __init__(self, activity='android.app.NativeActivity', resolved=None, launch=''):
            super().__init__('adb'); self.activity, self.resolved, self.launch = activity, resolved, launch
            self.cmds = []; self.started = False
        def shell(self, script, **kw):
            self.cmds.append(script)
            if script.startswith('cmd package resolve-activity'):
                return self.resolved if self.resolved is not None else f'priority=0 preferredOrder=0 match=0x108000\norg.nx2a.test/{self.activity}\n'
            if script.startswith('am start'):
                self.started = f'org.nx2a.test/{self.activity}' in script
                return self.launch
            if script.startswith('[ -d'):
                return 'yes' if self.started and '.nx2a-old' not in script else ''
            if script.startswith('cat ') and '/result.txt' in script:
                return 'request=fixture\nresult=ok\nname=exported\n' if self.started else ''
            return ''

    def test_prepare_files_resolves_each_packaged_activity(self):
        for activity in ('android.app.NativeActivity', 'org.nx2a.GameActivity', 'org.nx2a.ExternalActivity', 'org.nx2a.KeyboardExternalActivity'):
            with self.subTest(activity=activity):
                adb = self.FakeAdb(activity)
                self.assertTrue(adb.prepare_app_files('org.nx2a.test', timeout=1))
                self.assertTrue(adb.started)
                self.assertTrue(any(c.startswith('cmd package resolve-activity') for c in adb.cmds))

    def test_save_requests_resolve_each_packaged_activity(self):
        from porter import saves
        from unittest import mock
        for activity in ('android.app.NativeActivity', 'org.nx2a.GameActivity', 'org.nx2a.ExternalActivity', 'org.nx2a.KeyboardExternalActivity'):
            with self.subTest(activity=activity), mock.patch.object(saves.secrets, 'token_hex', return_value='fixture'):
                adb = self.FakeAdb(activity)
                result = saves._request(adb, 'org.nx2a.test', 'export.txt', ['export'], timeout=0.01)
                self.assertEqual(result['name'], 'exported')
                self.assertTrue(any(c.startswith('cmd package resolve-activity') for c in adb.cmds))

    def test_missing_or_foreign_launcher_fails_without_starting(self):
        for resolved in ('No activity found\n', 'other.package/.Main\n', 'org.nx2a.test/Bad;echo bad\n'):
            with self.subTest(resolved=resolved):
                adb = self.FakeAdb(resolved=resolved)
                with self.assertRaisesRegex(AdbError, 'launcher activity'):
                    adb.start_app('org.nx2a.test')
                self.assertFalse(any(c.startswith('am start') for c in adb.cmds))

    def test_start_error_is_reported_immediately(self):
        adb = self.FakeAdb(launch='Error type 3\nError: Activity class does not exist.\n')
        with self.assertRaisesRegex(AdbError, 'Activity class does not exist'):
            adb.start_app('org.nx2a.test')

    def test_failed_save_launch_removes_pending_request(self):
        from porter import saves
        adb = self.FakeAdb(resolved='No activity found\n')
        with self.assertRaisesRegex(AdbError, 'launcher activity'):
            saves._request(adb, 'org.nx2a.test', 'export.txt', ['export'], timeout=0.01)
        self.assertTrue(any('rm -f' in c and '/export.txt' in c for c in adb.cmds))


if __name__ == '__main__':
    unittest.main()
