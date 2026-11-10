# porter driver without a device: another Vulkan driver library in an installed port's APK (nxnative/dm/android/apk/apkdriver.py) and env.txt's DM_VK_DRIVER row
# (porter/driver.py). The device is made up: pull hands over an APK built by mkapk.py, install and push keep what they were given (python -m unittest discover tests).
import json, os, shutil, subprocess, sys, tempfile, unittest, zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
os.environ.setdefault('NX2A_HOME', tempfile.mkdtemp(prefix='nx2a_test_home_'))
from porter import datapack, driver, paths   # noqa: E402
from porter.adb import Adb                     # noqa: E402
from porter.upstream import BuildError         # noqa: E402

APK = REPO / 'nxnative/dm/android/apk'
PKG = 'org.nx2a.test'
HOOKS = ('libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so', 'libfile_redirect_hook.so', 'libgsl_alloc_hook.so')


def libs(apk):   # the APK's libraries {name: bytes}, after checking every entry is stored and every library's data starts on a 16 KB page (as mkapk.py writes them)
    out = {}
    with zipfile.ZipFile(apk) as z:
        raw = Path(apk).read_bytes()
        for i in z.infolist():
            assert i.compress_type == zipfile.ZIP_STORED, i.filename
            if i.filename.startswith('lib/arm64-v8a/'):
                start = i.header_offset + 30 + int.from_bytes(raw[i.header_offset + 26:i.header_offset + 28], 'little') + int.from_bytes(raw[i.header_offset + 28:i.header_offset + 30], 'little')
                assert start % 16384 == 0, (i.filename, start)
                out[i.filename.split('/')[-1]] = z.read(i)
    assert b'APK Sig Block 42' in raw   # signed again (APK Signature Scheme v2)
    return out


class Device(Adb):
    def __init__(self, apk, env):
        super().__init__('adb'); self.serial = 'dev'; self.apk, self.env_text, self.ops, self.installed_apk = apk, env, [], None
    def pick(self): return 'dev'
    def run(self, *a, timeout=None, check=True, retries=3):
        a = [str(x) for x in a]; self.ops.append(a[0])
        if a[0] == 'pull': shutil.copyfile(self.apk, a[2]); return ''
        if a[0] == 'push': self.env_text = Path(a[1]).read_text(encoding='utf-8'); return ''
        if a[0] == 'install': self.installed_apk = Path(a[-1]).read_bytes(); return 'Success'
        if a[0] == 'shell' and a[1].startswith('pm path'): return f'package:/data/app/{PKG}/base.apk\n'
        if a[0] == 'shell' and a[1].startswith('cat '): return self.env_text
        return ''


class DriverTest(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='nx2a_driver_test_'))
        self.addCleanup(shutil.rmtree, self.t, ignore_errors=True)

    def apk(self, with_driver=True):   # a port's APK as porter makes it: the host, the old driver (Mr Purple's T30) and adrenotools' libraries
        extra = []
        for n in (('libvulkan.purple.so',) + HOOKS if with_driver else ()):
            (self.t / n).write_bytes(n.encode() * 50); extra += ['--extra-so', str(self.t / n)]
        so = self.t / 'libdmhost.so'; so.write_bytes(b'\x7fELF' + b'\0' * 60)
        subprocess.run([sys.executable, str(APK / 'mkapk.py'), str(so), str(self.t / 'a.apk'), str(paths.signing_key()), '--lib', 'dmhost', '--release'] + extra, check=True, capture_output=True)
        return self.t / 'a.apk'

    def tool(self, *a):
        return subprocess.run([sys.executable, str(APK / 'apkdriver.py'), *map(str, a)], capture_output=True, text=True)

    def test_env_row(self):
        self.assertEqual(driver.env_with_driver('DM_LANG=en-US\nDM_VK_DRIVER=libvulkan.purple.so\nDM_CTL=x\n', 'libvulkan_freedreno.so'),
                         'DM_LANG=en-US\nDM_VK_DRIVER=libvulkan_freedreno.so\nDM_CTL=x\n')   # in place
        self.assertEqual(driver.env_with_driver('DM_LANG=en-US\n', 'libx.so'), 'DM_LANG=en-US\nDM_VK_DRIVER=libx.so\n')   # appended
        self.assertEqual(driver.env_with_driver('A=1\nDM_VK_DRIVER=liba.so\nDM_VK_DRIVER=libb.so\n', ''), 'A=1\n')   # the device's driver: no row

    def test_swap_and_system(self):
        new = self.t / 'vulkan.freedreno.so'; new.write_bytes(b'turnip' * 100)
        r = self.tool(self.apk(), self.t / 'b.apk', paths.signing_key(), '--driver', new)
        self.assertEqual(r.returncode, 0, r.stderr); self.assertEqual(json.loads(r.stdout), {'driver': 'libvulkan.freedreno.so', 'dropped': ['libvulkan.purple.so']})
        got = libs(self.t / 'b.apk')
        self.assertEqual(set(got), {'libdmhost.so', 'libvulkan.freedreno.so', *HOOKS}); self.assertEqual(got['libvulkan.freedreno.so'], new.read_bytes())
        r = self.tool(self.t / 'b.apk', self.t / 'c.apk', paths.signing_key(), '--system')
        self.assertEqual(json.loads(r.stdout), {'driver': '', 'dropped': ['libvulkan.freedreno.so']}); self.assertEqual(set(libs(self.t / 'c.apk')), {'libdmhost.so', *HOOKS})

    def test_needs_adrenotools(self):   # ported without --vulkan-driver: no libadrenotools to load a driver with
        new = self.t / 'libvulkan_freedreno.so'; new.write_bytes(b'x')
        r = self.tool(self.apk(with_driver=False), self.t / 'b.apk', paths.signing_key(), '--driver', new)
        self.assertNotEqual(r.returncode, 0); self.assertIn('--vulkan-driver', r.stderr); self.assertFalse((self.t / 'b.apk').exists())

    def test_porter_driver(self):   # porter driver on a made-up device: the APK reinstalled with the library, env.txt naming it
        new = self.t / 'libvulkan_freedreno.so'; new.write_bytes(b'turnip')
        dev = Device(self.apk(), 'DM_LANG=en-US\nDM_VK_DRIVER=libvulkan.purple.so\n')
        out = driver.swap(dev, PKG, new)
        self.assertEqual(out, {'driver': 'libvulkan_freedreno.so', 'dropped': ['libvulkan.purple.so'], 'package': PKG})
        self.assertEqual(dev.env_text, 'DM_LANG=en-US\nDM_VK_DRIVER=libvulkan_freedreno.so\n')
        apk = self.t / 'installed.apk'; apk.write_bytes(dev.installed_apk); self.assertIn('libvulkan_freedreno.so', libs(apk))
        self.assertEqual(dev.ops, ['shell', 'pull', 'shell', 'install', 'shell', 'push'])   # pm path, pull, force-stop, install, cat env.txt, push it
        with self.assertRaises(BuildError): driver.swap(dev, PKG, self.t / 'missing.so')

    def test_host_swap(self):   # apkdriver.py --host: libdmhost.so replaced, the driver library kept
        so = self.t / 'new_host.so'; so.write_bytes(b'\x7fELF new host' * 10)
        r = self.tool(self.apk(), self.t / 'b.apk', paths.signing_key(), '--host', so)
        self.assertEqual(r.returncode, 0, r.stderr); self.assertEqual(json.loads(r.stdout), {'driver': 'libvulkan.purple.so', 'dropped': []})
        got = libs(self.t / 'b.apk'); self.assertEqual(got['libdmhost.so'], so.read_bytes()); self.assertEqual(got['libvulkan.purple.so'], b'libvulkan.purple.so' * 50)
        self.assertNotEqual(self.tool(self.apk(), self.t / 'c.apk', paths.signing_key()).returncode, 0)   # nothing to change

    def test_stale_sources(self):   # porter host: the rewriter's sources the device's modules were made with, against this checkout's
        import hashlib
        from porter import host
        tools = REPO / 'nxnative/dm/tools'
        def row(rel, ok=True): return (hashlib.sha256((tools / rel).read_bytes()).hexdigest() if ok else '0' * 64) + '  ' + rel
        fft, other = '010038B015560000', '01004AB00A260000'   # two games with patches.py
        good = [row('patch_nso.py'), row('sdk_patches.py'), row(f'../games/{fft}/patches.py'), row(f'../games/{other}/patches.py', ok=False), '0' * 64 + '  ../../../../build/fpslock/patches.py']
        self.assertEqual(host.stale_sources('\n'.join(good), fft), [])   # another game's patches and a file outside the repository do not count
        self.assertEqual(host.stale_sources('\n'.join([row('patch_nso.py', ok=False)] + good[1:]), fft), ['patch_nso.py'])
        self.assertEqual(host.stale_sources('\n'.join(good[:2] + [row(f'../games/{fft}/patches.py', ok=False)]), fft), [f'../games/{fft}/patches.py'])
        self.assertEqual(host.stale_sources('\n'.join(good[:2]), fft), [f'../games/{fft}/patches.py'])   # patches registered for the game since the port

    def test_stale_sdk_patches(self):   # porter host: a changed sdk_patches.py counts only when it changes what the port's modules got
        import hashlib
        from porter import host
        tools = REPO / 'nxnative/dm/tools'
        def row(rel, ok=True): return (hashlib.sha256((tools / rel).read_bytes()).hexdigest() if ok else '0' * 64) + '  ' + rel
        bdsp, sdk = '0100000011D90000', '2d369d9999e3e10e665106f840ffff9b'   # Brilliant Diamond (no patches.py) and its SDK 11.4.3 build, which sdk_patches.py cuts
        cuts = host._tables(tools / 'sdk_patches.py')['CUTS'][sdk]
        def tsv(rows): return f'# BUILD_ID\t{sdk}\n' + ''.join(f'{o:#x}\t{ob}\t{nb}\t{why}\n' for o, ob, nb, why in rows)
        main = host.module_tables('# BUILD_ID\t94ceae325c205c4b9d6f7235552f28fd\n', '# BUILD_ID\t94ceae325c205c4b9d6f7235552f28fd\n')
        record = '\n'.join([row('patch_nso.py'), row('sdk_patches.py', ok=False)])
        self.assertEqual(host.module_tables(tsv(cuts), '')['cuts'][0], (cuts[0][0], cuts[0][1], cuts[0][2]))
        self.assertEqual(host.stale_sources(record, bdsp, modules=[main, host.module_tables(tsv(cuts), f'# BUILD_ID\t{sdk}\n')]), [])   # changed for other builds only
        self.assertEqual(host.stale_sources(record, bdsp, modules=[main, host.module_tables(tsv(cuts[:-1]), '')]), ['sdk_patches.py'])   # a cut of its build added since
        self.assertEqual(host.stale_sources(record, bdsp, modules=[host.module_tables(tsv([]), '')]), ['sdk_patches.py'])
        self.assertEqual(host.stale_sources(record, bdsp), ['sdk_patches.py'])   # nothing known of the modules
        self.assertEqual(host.stale_sources(record, bdsp, modules=[]), ['sdk_patches.py'])
        main_cut = [(0x100, '00', '01', 'fps')]   # a --fps60 port: its extra cuts follow main's registered ones (none here)
        fps = host.module_tables(tsv(main_cut).replace(sdk, '94ceae325c205c4b9d6f7235552f28fd'), '')
        sdk_mod = host.module_tables(tsv(cuts), '')
        self.assertEqual(host.stale_sources(record, bdsp, modules=[fps, sdk_mod]), ['sdk_patches.py'])
        self.assertEqual(host.stale_sources(record + '\n' + '0' * 64 + '  ../../../../build/fpslock/patches.py', bdsp, modules=[fps, sdk_mod]), [])


class HostDevice(Adb):   # porter host's made-up device: a port's data pack (nx2a-data.json, mods/) and its installed APK
    def __init__(self, apk, files, debuggable=False):
        super().__init__('adb'); self.serial = 'dev'; self.apk, self.files, self.debuggable, self.ops, self.installed_apk = apk, files, debuggable, [], None
    def pick(self): return 'dev'
    def run(self, *a, timeout=None, check=True, retries=3):
        a = [str(x) for x in a]; self.ops.append(a[0] if a[0] != 'shell' else a[1].split()[0])
        if a[0] == 'pull': shutil.copyfile(self.apk, a[2]); return ''
        if a[0] == 'install': self.installed_apk = Path(a[-1]).read_bytes(); return 'Success'
        if a[0] != 'shell': return ''
        cmd = a[1]
        if cmd.startswith('pm path'): return f'package:/data/app/{PKG}/base.apk\n'
        if cmd.startswith('dumpsys package'): return '    flags=[ DEBUGGABLE HAS_CODE ]\n' if self.debuggable else '    flags=[ HAS_CODE ]\n'
        if cmd.startswith('ls '): return '\n'.join(n.split('/', 1)[1] for n in self.files if n.startswith('mods/')) + '\n'
        if cmd.startswith('cat '):
            path = cmd.split()[1].strip("'")
            return next((v for k, v in self.files.items() if path.endswith('/files/' + k)), '')
        return ''


class HostTest(unittest.TestCase):   # porter host's update(): its refusals, the debug or release build and the install, without an NDK (build.py is stood in for)
    BDSP, SDK = '0100000011D90000', '2d369d9999e3e10e665106f840ffff9b'   # Brilliant Diamond: a registered version, no patches.py of its own, an SDK build that sdk_patches.py cuts

    def setUp(self):
        from unittest import mock
        from porter import host
        self.host, self.t = host, Path(tempfile.mkdtemp(prefix='nx2a_host_test_'))
        self.addCleanup(shutil.rmtree, self.t, ignore_errors=True)
        so = self.t / 'libdmhost.so'; so.write_bytes(b'\x7fELF old host' * 8)
        subprocess.run([sys.executable, str(APK / 'mkapk.py'), str(so), str(self.t / 'a.apk'), str(paths.signing_key()), '--lib', 'dmhost', '--release'], check=True, capture_output=True)
        self.builds, real = [], subprocess.run
        def run(argv, *a, **kw):   # build.py stood in for: a made-up libdmhost.so in --out; everything else (apkdriver.py, game.py header) runs
            argv = [str(x) for x in argv]
            if argv[1].endswith('build.py'):
                self.builds.append(argv); out = Path(argv[argv.index('--out') + 1]); (out / 'libdmhost.so').write_bytes(b'\x7fELF new host' * 8)
                return subprocess.CompletedProcess(argv, 0, '', '')
            return real(argv, *a, **kw)
        for p in (mock.patch.object(host.subprocess, 'run', side_effect=run), mock.patch.object(host.toolchain, 'find_ndk', return_value=(self.t / 'ndk', '27.2'))):
            p.start(); self.addCleanup(p.stop)

    def files(self, **man):
        import hashlib
        tools = REPO / 'nxnative/dm/tools'
        record = ''.join(f'{hashlib.sha256((tools / r).read_bytes()).hexdigest()}  {r}\n' for r in ('patch_nso.py', 'nso_code.py', 'nso_asm.py', 'sdk_patches.py'))
        cuts = self.host._tables(tools / 'sdk_patches.py')['CUTS'][self.SDK]
        manifest = dict(title_id=self.BDSP, package=PKG, version='v393216', profile_h=self.host.profile_hash(self.BDSP)); manifest.update(man)
        return {'nx2a-data.json': json.dumps({k: v for k, v in manifest.items() if v is not None}), 'mods/build_src.sha256': record,
                'mods/sdk.cuts.tsv': f'# BUILD_ID\t{self.SDK}\n' + ''.join(f'{o:#x}\t{ob}\t{nb}\t{w}\n' for o, ob, nb, w in cuts), 'mods/sdk.obs.tsv': f'# BUILD_ID\t{self.SDK}\n'}

    def refused(self, files, says):
        dev = HostDevice(self.t / 'a.apk', files)
        with self.assertRaises(BuildError) as e: self.host.update(dev, PKG)
        self.assertIn(says, str(e.exception)); self.assertEqual(self.builds, []); self.assertIsNone(dev.installed_apk)

    def test_refusals(self):
        self.refused(self.files(title_id='../../etc/passwd'), 'not a title ID')   # never a path or a build.py --game argument
        self.refused(self.files(title_id='0100000011D9000'), 'not a title ID')
        self.refused(self.files(title_id=None), 'no title ID')
        self.refused(self.files(title_id='0100000000000ABC'), 'no profile of')
        self.refused(self.files(version='v458752'), 'not a version the profile')   # a derived profile's port, with its own alias region
        self.refused(self.files(profile_h=None), 'no profile_h')                    # a pack from before porter recorded it
        self.refused(self.files(profile_h='0' * 64), 'another profile')             # --alias-headroom, or the profile changed since
        f = self.files(); f['mods/build_src.sha256'] = f['mods/build_src.sha256'].replace(f['mods/build_src.sha256'][:64], '0' * 64)
        self.refused(f, 'patch_nso.py')                                              # the rewriter changed since the port
        f = self.files(); f['mods/build_src.sha256'] = ''
        self.refused(f, 'no mods/build_src.sha256')

    def test_update(self):
        for debuggable in (False, True):
            self.builds.clear()
            dev = HostDevice(self.t / 'a.apk', self.files(title_id=self.BDSP.lower()), debuggable)
            out = self.host.update(dev, PKG, jobs=2)
            self.assertEqual((out['title_id'], out['debug']), (self.BDSP, debuggable))
            self.assertEqual(len(self.builds), 1); b = self.builds[0]
            self.assertEqual(b[b.index('--game') + 1], self.BDSP); self.assertEqual('--release' in b, not debuggable)
            apk = self.t / 'installed.apk'; apk.write_bytes(dev.installed_apk); self.assertEqual(libs(apk)['libdmhost.so'], b'\x7fELF new host' * 8)
            self.assertEqual(dev.ops[-3:], ['pull', 'am', 'install'])


if __name__ == '__main__':
    unittest.main()
