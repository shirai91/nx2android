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


if __name__ == '__main__':
    unittest.main()
