# porter/install.py without a device: a RomFS read from a folder outside the app's files folder (--romfs-at). The device is made up: a dict of its files and the shell's
# answers (python -m unittest discover tests).
import hashlib, json, os, shlex, shutil, struct, subprocess, sys, tempfile, unittest, zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
os.environ.setdefault('NX2A_HOME', tempfile.mkdtemp(prefix='nx2a_test_home_'))
from porter import cli, datapack, install   # noqa: E402
from porter.adb import Adb                  # noqa: E402
from porter.upstream import BuildError, PROG   # noqa: E402

PKG = 'org.nx2a.test'
FILES = datapack.files_folder(PKG)
DUMP = '/sdcard/Games/Test/romfs'
ROM = {'Data/a.bin': 20, 'Data/sub/b.bin': 7, 'c.bin': 5}


class Device(Adb):
    """A made-up device: files {absolute path: size}, the text of the few files the installer reads, All files access as the appop says, and every step in ops.
    A file's content is the pack's (size times b'x') unless md5 {absolute path: MD5} says otherwise; md5=False: md5sum gives nothing. installed: the app is on the
    device already; requests: its manifest asks for the access"""
    def __init__(self, files=(), text=None, sdk=34, access='default', grants=True, free=1 << 40, md5=None, installed=False, requests=True):
        super().__init__('adb'); self.serial = 'dev'
        self.fs, self.text, self.sdk_, self.access, self.grants, self.free, self.ops = dict(files), dict(text or {}), sdk, access, grants, free, []
        self.md5, self.installed_, self.requests = md5 if md5 is not None else {}, installed, requests
    def pick(self): return 'dev'
    def hold(self, folder): return None
    def prop(self, name): return str(self.sdk_) if name == 'ro.build.version.sdk' else 'Test'
    def gpu(self): return 'Qualcomm, Adreno (TM) 830'
    def free_bytes(self, path='/sdcard'): return self.free
    def same_volume(self, a, b): return True
    def read_text(self, path): return self.text.get(path, '')
    def remote_sizes(self, rdir): p = rdir.rstrip('/') + '/'; return {k[len(p):]: v for k, v in self.fs.items() if k.startswith(p)}
    def install(self, apk, package, replace_signer=False): self.ops.append(('install', package)); self.installed_ = True
    def installed(self, package): return self.installed_
    def requests_permission(self, package, perm): return self.requests
    def remote_md5(self, rdir, rels):
        self.ops.append(('md5', rdir, len(rels)))
        if self.md5 is False: return {}
        return {r: self.md5.get(f'{rdir}/{r}') or hashlib.md5(b'x' * self.fs[f'{rdir}/{r}']).hexdigest() for r in rels if f'{rdir}/{r}' in self.fs}
    def prepare_app_files(self, package): return False
    def push_tree(self, local, remote, skip=(), exclude=(), have=None, **kw):   # what the real one pushes: every file but skip's and exclude's folders
        local, n = Path(local), 0
        for p in sorted(local.rglob('*')):
            rel = p.relative_to(local).as_posix()
            if p.is_file() and rel not in skip and rel.split('/')[0] not in exclude:
                self.fs[f'{remote}/{rel}'] = p.stat().st_size; n += 1
                if self.md5: self.md5.pop(f'{remote}/{rel}', None)   # the pack's content now
                if rel.endswith('.txt'): self.text[f'{remote}/{rel}'] = p.read_text(encoding='utf-8')
        self.ops.append(('push_tree', remote, n))
    def push_file(self, local, dst, **kw):
        self.fs[dst] = Path(local).stat().st_size; self.ops.append(('push_file', dst))
        if dst.endswith('.txt'): self.text[dst] = Path(local).read_text(encoding='utf-8')
    def shell(self, script, **kw):
        self.ops.append(('shell', script))
        if script.startswith('appops get'): return 'No operations.\nDefault mode: default\n' if self.access == 'default' else f'Uid mode: MANAGE_EXTERNAL_STORAGE: {self.access}\n'
        if script.startswith('appops set') and self.grants: self.access = 'allow'
        if script.startswith('pm grant') and self.grants: self.access = 'granted'
        if script.startswith('dumpsys package'): return '      android.permission.READ_EXTERNAL_STORAGE: granted=true\n' if self.access == 'granted' else ''
        if script.startswith('rm -rf '): d = shlex.split(script)[2] + '/'; self.fs = {k: v for k, v in self.fs.items() if not k.startswith(d)}
        return ''
    def step(self, kind, what=''):   # the index of the first op of that kind whose text holds what (-1: none)
        return next((i for i, o in enumerate(self.ops) if o[0] == kind and what in ' '.join(map(str, o[1:]))), -1)


def apk_file(path, access=True):   # an APK whose manifest's string pool holds the permission (as mkapk.py writes it, UTF-16) or not
    with zipfile.ZipFile(path, 'w') as z: z.writestr('AndroidManifest.xml', b'\x03\x00\x08\x00' + (install.ACCESS.encode('utf-16-le') if access else b'android.permission.VIBRATE'))
    return path


def axml_elements(b):   # [(tag, {attribute: value})] of a binary AndroidManifest.xml, in document order (strings and ints; booleans as True / False)
    pos, strs, out = 8, [], []
    while pos < len(b):
        t, _, size = struct.unpack_from('<HHI', b, pos)
        if t == 0x0001:   # the string pool, UTF-16
            n, _, _, start, _ = struct.unpack_from('<IIIII', b, pos + 8)
            for o in struct.unpack_from(f'<{n}I', b, pos + 28):
                p = pos + start + o; ln = struct.unpack_from('<H', b, p)[0]; strs.append(b[p + 2:p + 2 + 2 * ln].decode('utf-16-le'))
        elif t == 0x0102:   # a start element: its attributes after the 20-byte extension
            name, cnt = struct.unpack_from('<I', b, pos + 20)[0], struct.unpack_from('<H', b, pos + 28)[0]; attrs = {}
            for k in range(cnt):
                _, an, _, _, _, dt, data = struct.unpack_from('<IIIHBBI', b, pos + 36 + 20 * k)
                attrs[strs[an]] = strs[data] if dt == 0x03 else bool(data) if dt == 0x12 else data
            out.append((strs[name], attrs))
        pos += size
    return out


class RomfsAt(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='nx2a_test_install_')); self.real, PROG.mode = install.Adb, 'off'
        d = self.data = self.t / 'Test-data'
        for rel, n in [*((f'RomFS/{r}', n) for r, n in ROM.items()), ('mods/main', 10), ('shaders.dmsv', 4)]: (d / rel).parent.mkdir(parents=True, exist_ok=True); (d / rel).write_bytes(b'x' * n)
        (d / 'env.txt').write_text('# porter\nDM_LANG=en-US\n', encoding='utf-8')
        self.bytes = sum(p.stat().st_size for p in d.rglob('*') if p.is_file())
        (d / datapack.MANIFEST).write_text(json.dumps(dict(title_id='0100000000AAA000', name='Test Game', package=PKG, version='v65536', display='1.1.0', requires=[], bytes=self.bytes)))
        self.apk = apk_file(self.t / 'Test.apk')

    def tearDown(self): install.Adb = self.real; shutil.rmtree(self.t, ignore_errors=True)

    def run_on(self, dev, **kw):
        install.Adb = lambda exe, serial=None, log=print: dev
        install.install(self.apk, self.data, 'adb', **kw)
        return dev

    def test_target(self):   # absolute folders in shared storage; app (or the files folder's own RomFS) for the files folder; folders the app cannot read refused at once
        self.assertEqual(install.romfs_target('/sdcard/Games/Pokopia/romfs/'), '/sdcard/Games/Pokopia/romfs')
        self.assertEqual(install.romfs_target('/storage/1234-ABCD/nx2a/x/../RomFS'), '/storage/1234-ABCD/nx2a/RomFS')
        for none in (None, 'app', f'{FILES}/RomFS', f'{FILES}/RomFS/'): self.assertIsNone(install.romfs_target(none, FILES))
        for bad in ('Games/romfs', '/data/local/tmp/romfs', ' /sdcard/x', '/sdcard/x\n', '/sdcard/Android/data/dev.eden.eden_emulator/files/romfs',
                    '/storage/emulated/0/android/OBB/x', '/sdcard/' + 'x' * 600):
            with self.assertRaises(BuildError, msg=bad): install.romfs_target(bad, FILES)
        with self.assertRaisesRegex(BuildError, 'installer\'s own'): install.romfs_target(f'{FILES}/romfs2', FILES)

    def test_env_row(self):   # the installer's row, last, read back from the device's env.txt; a hand-written DM_ROMFS is not the installer's
        env = install.env_with_romfs('# porter\nDM_LANG=en-US\n', DUMP)
        self.assertTrue(env.endswith(f'\nDM_ROMFS={DUMP}\n')); self.assertEqual(env.count('DM_ROMFS='), 1)
        self.assertEqual(install.device_romfs(env), DUMP)
        self.assertIsNone(install.device_romfs('DM_LANG=en-US\nDM_ROMFS=/sdcard/copy\n'))
        self.assertEqual(install.device_romfs(install.env_with_romfs(env, '/sdcard/b')), '/sdcard/b')   # the last one, as the app applies them

    def test_mismatch(self):   # the first differences of each kind, with sizes; nothing for the same listing
        self.assertEqual(install.romfs_mismatch(DUMP, ROM, dict(ROM)), '')
        msg = install.romfs_mismatch(DUMP, ROM, {'Data/a.bin': 21, 'c.bin': 5, 'x.bin': 1, 'y.bin': 1}, n=1)
        self.assertIn('1 of the game\'s files are not there (Data/sub/b.bin)', msg); self.assertIn('Data/a.bin: 21 bytes, the pack has 20', msg)
        self.assertIn('2 are not the game\'s (x.bin, …)', msg)
        self.assertIn('holds no files', install.romfs_mismatch(DUMP, ROM, {}))

    def test_in_place(self):   # the folder matches: no RomFS pushed, access granted, env.txt with the row, the earlier copy in the files folder removed only after that
        dev = self.run_on(Device({**{f'{DUMP}/{r}': n for r, n in ROM.items()}, f'{FILES}/RomFS/Data/a.bin': 20, f'{FILES}/RomFS/old.bin': 3, f'{FILES}/log.txt': 9}), romfs_at=DUMP)
        self.assertFalse([k for k in dev.fs if k.startswith(f'{FILES}/RomFS/')]); self.assertTrue(dev.fs[f'{FILES}/mods/main'])
        self.assertEqual(install.device_romfs(dev.text[f'{FILES}/env.txt']), DUMP); self.assertIn('DM_LANG=en-US', dev.text[f'{FILES}/env.txt'])
        self.assertEqual(dev.access, 'allow')
        order = [dev.step('install'), dev.step('shell', 'appops set'), dev.step('push_tree', FILES), dev.step('push_file', 'env.txt'), dev.step('shell', 'rm -rf'), dev.step('push_file', datapack.MANIFEST)]
        self.assertEqual(order, sorted(order)); self.assertNotIn(-1, order)
        self.assertEqual(dev.ops[dev.step('push_tree', FILES)][2], 2)   # mods/main and shaders.dmsv: no RomFS; env.txt (with the row) and the manifest go on their own
        dev.ops.clear(); self.run_on(dev)   # a later install without --romfs-at keeps the folder
        self.assertEqual(install.device_romfs(dev.text[f'{FILES}/env.txt']), DUMP); self.assertFalse([k for k in dev.fs if k.startswith(f'{FILES}/RomFS/')])
        self.assertEqual(dev.step('shell', 'appops set'), -1)   # already granted: nothing to set

    def test_refused_before_anything(self):   # a folder that is not the pack's RomFS: refused before the APK goes in, with the first differences and what to do
        dev = Device({f'{DUMP}/Data/a.bin': 20, f'{DUMP}/c.bin': 6})
        with self.assertRaises(BuildError) as e: self.run_on(dev, romfs_at=DUMP)
        for s in ('Data/sub/b.bin', 'c.bin: 6 bytes, the pack has 5', 'Test Game 1.1.0', '--romfs-push', '--romfs-at app'): self.assertIn(s, str(e.exception))
        self.assertEqual([o for o in dev.ops if o[0] != 'shell'], [])
        dev = Device({**{f'{DUMP}/{r}': n for r, n in ROM.items()}, f'{DUMP}/other.bin': 1})   # files that are not the game's: refused with --romfs-push too, nothing deleted
        with self.assertRaisesRegex(BuildError, 'never deletes'): self.run_on(dev, romfs_at=DUMP, romfs_push=True)
        self.assertIn(f'{DUMP}/other.bin', dev.fs); self.assertEqual(dev.step('install'), -1)
        with self.assertRaisesRegex(BuildError, 'earlier nx2android'): apk_file(self.apk, access=False); self.run_on(Device({f'{DUMP}/{r}': n for r, n in ROM.items()}), romfs_at=DUMP)

    def test_access_refused(self):   # the device does not grant it: a clear error, and no data pushed
        dev = Device({f'{DUMP}/{r}': n for r, n in ROM.items()}, grants=False)
        with self.assertRaises(BuildError) as e: self.run_on(dev, romfs_at=DUMP)
        self.assertIn('MANAGE_EXTERNAL_STORAGE): default', str(e.exception)); self.assertIn('Settings > Apps > Test Game > All files access', str(e.exception))
        self.assertEqual((dev.step('push_tree'), dev.step('push_file')), (-1, -1))

    def test_android_10(self):   # no All files access: READ_EXTERNAL_STORAGE through pm grant
        dev = self.run_on(Device({f'{DUMP}/{r}': n for r, n in ROM.items()}, sdk=29), romfs_at=DUMP)
        self.assertNotEqual(dev.step('shell', 'pm grant org.nx2a.test android.permission.READ_EXTERNAL_STORAGE'), -1); self.assertEqual(dev.step('shell', 'appops'), -1)

    def test_carried_over_and_back(self):   # a later install checks the kept folder again; app moves the RomFS back into the files folder and drops the row
        env = {f'{FILES}/env.txt': install.env_with_romfs('DM_LANG=en-US\n', DUMP)}
        with self.assertRaisesRegex(BuildError, 'earlier install\'s --romfs-at'): self.run_on(Device({f'{DUMP}/c.bin': 5}, text=env, access='allow'))
        dev = self.run_on(Device({f'{DUMP}/c.bin': 5}, text=env, access='allow'), romfs_at='app')
        self.assertEqual({k[len(FILES) + 7:] for k in dev.fs if k.startswith(f'{FILES}/RomFS/')}, set(ROM)); self.assertIsNone(install.device_romfs(dev.text[f'{FILES}/env.txt']))
        self.assertEqual(dev.step('push_file', 'env.txt'), -1); self.assertIn(f'{DUMP}/c.bin', dev.fs)   # the folder is left as it is

    def test_romfs_push(self):   # --romfs-push: the missing files and those of another size go into the folder, then it is checked again
        dev = self.run_on(Device({f'{DUMP}/c.bin': 4}), romfs_at=DUMP, romfs_push=True)
        self.assertEqual(dev.remote_sizes(DUMP), ROM); self.assertNotEqual(dev.step('push_tree', DUMP), -1)
        with self.assertRaisesRegex(BuildError, '--romfs-push needs --romfs-at'): self.run_on(Device(), romfs_push=True)

    def test_content(self):   # a folder of the right names and sizes but other bytes (another game version): refused, or pushed again with --romfs-push
        rom = {f'{DUMP}/{r}': n for r, n in ROM.items()}
        dev = Device(rom, md5={f'{DUMP}/c.bin': '0' * 32})
        with self.assertRaisesRegex(BuildError, r'1 have the right size but other content \(MD5: c.bin\)'): self.run_on(dev, romfs_at=DUMP)
        self.assertEqual(dev.step('install'), -1); self.assertNotEqual(dev.step('md5', DUMP), -1)
        dev = self.run_on(Device(rom, md5={f'{DUMP}/c.bin': '0' * 32}), romfs_at=DUMP, romfs_push=True)
        self.assertNotEqual(dev.step('push_tree', DUMP), -1); self.assertFalse(dev.md5)
        dev = self.run_on(Device(rom, md5=False), romfs_at=DUMP)   # no MD5 from the device: names and sizes only, as before
        self.assertNotEqual(dev.step('push_file', 'env.txt'), -1)
        dev = Device({f'{DUMP}/c.bin': 6})   # a listing that is refused anyway: no MD5 asked for
        with self.assertRaises(BuildError): self.run_on(dev, romfs_at=DUMP)
        self.assertEqual(dev.step('md5'), -1)

    def test_sample(self):   # files of at most SAMPLE_MAX bytes, at most SAMPLE_FILES of them, spread over the sorted paths
        rom = {f'f{i:04d}': 10 for i in range(1000)} | {'big': install.SAMPLE_MAX + 1}
        s = install.romfs_sample(rom)
        self.assertEqual(len(s), install.SAMPLE_FILES); self.assertNotIn('big', s); self.assertEqual((s[0], s[-1] > 'f0990'), ('f0000', True)); self.assertEqual(s, sorted(set(s)))
        self.assertEqual(install.romfs_sample(ROM), sorted(ROM))

    def test_update_grant_first(self):   # an update whose app asks for the access: a refused grant stops the install before the APK goes in
        rom = {f'{DUMP}/{r}': n for r, n in ROM.items()}
        dev = Device(rom, grants=False, installed=True)
        with self.assertRaisesRegex(BuildError, 'did not grant it'): self.run_on(dev, romfs_at=DUMP)
        self.assertEqual(dev.step('install'), -1)
        dev = Device(rom, grants=False, installed=True, requests=False)   # an older APK that did not ask: refused after the APK, before any data
        with self.assertRaisesRegex(BuildError, 'did not grant it'): self.run_on(dev, romfs_at=DUMP)
        self.assertNotEqual(dev.step('install'), -1); self.assertEqual((dev.step('push_tree'), dev.step('push_file')), (-1, -1))
        dev = self.run_on(Device(rom, installed=True), romfs_at=DUMP)   # granted before the APK
        self.assertLess(dev.step('shell', 'appops set'), dev.step('install'))

    def test_free_space(self):   # the RomFS in another folder needs no room in the files folder
        rom = sum(ROM.values())
        self.run_on(Device({f'{DUMP}/{r}': n for r, n in ROM.items()}, free=self.bytes - rom), romfs_at=DUMP)
        with self.assertRaisesRegex(BuildError, 'GB more'): self.run_on(Device(free=self.bytes - rom))

    def test_options(self):   # install and port --install take --romfs-at / --romfs-push; port without --install, or a folder the app cannot read, fails before any stage
        a = cli._parser().parse_args(['install', 'x-data', '--romfs-at', DUMP, '--romfs-push'])
        self.assertEqual((a.romfs_at, a.romfs_push), (DUMP, True)); self.assertIsNone(cli._parser().parse_args(['install', 'x-data']).romfs_at)
        self.assertEqual(cli._parser().parse_args(['port', 'x', '--install', '--romfs-at', 'app']).romfs_at, 'app')
        self.assertEqual(cli.main(['port', str(self.t), '--romfs-at', DUMP]), 2); self.assertEqual(cli.main(['port', str(self.t), '--install', '--romfs-at', 'romfs']), 2)

    def test_apk_asks_for_access(self):   # mkapk.py's manifest: All files access, READ_EXTERNAL_STORAGE up to Android 10, the legacy storage view there; VIBRATE as before
        so = self.t / 'libdmhost.so'; so.write_bytes(b'\x7fELF' + b'\0' * 60)
        subprocess.run([sys.executable, str(REPO / 'nxnative/dm/android/apk/mkapk.py'), str(so), str(self.t / 'a.apk'), str(self.t / 'k.json'), '--lib', 'dmhost', '--release'],
                       check=True, capture_output=True)
        self.assertTrue(install.apk_asks_access(self.t / 'a.apk')); self.assertFalse(install.apk_asks_access(apk_file(self.t / 'old.apk', access=False)))
        with zipfile.ZipFile(self.t / 'a.apk') as z: els = axml_elements(z.read('AndroidManifest.xml'))
        perms = {a['name']: a.get('maxSdkVersion') for tag, a in els if tag == 'uses-permission'}
        self.assertEqual(perms, {'android.permission.VIBRATE': None, install.ACCESS: None, 'android.permission.READ_EXTERNAL_STORAGE': 29,
                                 # local play over the LAN (DM_LAN): sockets, the Wi-Fi address, the multicast lock
                                 'android.permission.INTERNET': None, 'android.permission.ACCESS_NETWORK_STATE': None,
                                 'android.permission.ACCESS_WIFI_STATE': None, 'android.permission.CHANGE_WIFI_MULTICAST_STATE': None})
        app = next(a for tag, a in els if tag == 'application')
        self.assertIs(app['requestLegacyExternalStorage'], True); self.assertIs(app['debuggable'], False)


if __name__ == '__main__':
    unittest.main()
