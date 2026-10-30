"""The pause menu's game data page off the device: mkdex.py's classes.dex, mkapk.py's GameActivity and its alias, and data_zip.c's zip both ways."""
import hashlib, importlib.util, shutil, struct, subprocess, sys, tempfile, unittest, zipfile, zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
APK = REPO / 'nxnative/dm/android/apk'
TID = '010082400BCC6000'

spec = importlib.util.spec_from_file_location('mkdex', APK / 'mkdex.py'); mkdex = importlib.util.module_from_spec(spec); spec.loader.exec_module(mkdex)


class Dex(unittest.TestCase):
    def test_header(self):
        d = mkdex.dex()
        self.assertEqual(d[:8], b'dex\n035\0')
        self.assertEqual(struct.unpack_from('<I', d, 8)[0], zlib.adler32(d[12:]))
        self.assertEqual(d[12:32], hashlib.sha1(d[32:]).digest())
        size, hsize, endian = struct.unpack_from('<III', d, 32)
        self.assertEqual((size, hsize, endian), (len(d), 0x70, 0x12345678))
        n_str, str_off = struct.unpack_from('<II', d, 56)
        strings = []
        for i in range(n_str):
            o = struct.unpack_from('<I', d, str_off + 4 * i)[0]
            while d[o] & 0x80: o += 1   # the length (uleb128)
            strings.append(d[o + 1:d.index(b'\0', o + 1)].decode())
        self.assertEqual(strings, sorted(strings))
        for s in (mkdex.CLASS, mkdex.SUPER, 'onActivityResult', 'resultSeq', 'resultUri', 'requestCode', 'resultCode'): self.assertIn(s, strings)
        n_cls, cls_off = struct.unpack_from('<II', d, 96)
        self.assertEqual(n_cls, 1)
        types = struct.unpack_from('<II', d, 64); type_str = lambda t: strings[struct.unpack_from('<I', d, types[1] + 4 * t)[0]]
        cls, _, sup = struct.unpack_from('<III', d, cls_off)
        self.assertEqual((type_str(cls), type_str(sup)), (mkdex.CLASS, mkdex.SUPER))
        map_off = struct.unpack_from('<I', d, 52)[0]; n = struct.unpack_from('<I', d, map_off)[0]
        offs = [struct.unpack_from('<HHII', d, map_off + 4 + 12 * i)[3] for i in range(n)]
        self.assertEqual(offs, sorted(offs))

    def test_apk(self):   # the class, the activity that is it, and the launcher alias under the former name
        t = Path(tempfile.mkdtemp(prefix='nx2a_test_dex_'))
        try:
            so = t / 'libdmhost.so'; so.write_bytes(b'\x7fELF' + b'\0' * 60)
            subprocess.run([sys.executable, str(APK / 'mkapk.py'), str(so), str(t / 'a.apk'), str(t / 'k.json'), '--lib', 'dmhost'], check=True, capture_output=True)
            with zipfile.ZipFile(t / 'a.apk') as z:
                self.assertEqual(z.read('classes.dex'), mkdex.dex())
                m = z.read('AndroidManifest.xml')
            for s in (mkdex.CLASS_NAME, 'android.app.NativeActivity', 'activity-alias', 'targetActivity'): self.assertIn(s.encode('utf-16-le'), m)
        finally:
            shutil.rmtree(t, ignore_errors=True)


HARNESS = r'''
#include "data_zip.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argv[1][0] == 'w') {   // w out.zip dir name [dir name]
        const char *d[2], *n[2]; int k = (argc - 3) / 2; for (int i = 0; i < k; i++) { d[i] = argv[3 + 2 * i]; n[i] = argv[4 + 2 * i]; }
        int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644); unsigned files = 0; int r = dz_write(fd, d, n, k, &files); close(fd);
        printf("%d %u\n", r, files); return 0;
    }
    FILE *f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);   // x in.zip dest dest_device
    unsigned char *b = malloc(n + 1); fread(b, 1, n, f); fclose(f);
    const char *dest[2] = {argv[3], argv[4]}; printf("%d\n", dz_extract(b, (size_t)n, dest)); return 0;
}
'''


class DataZip(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: raise unittest.SkipTest('a C compiler is needed for the game data zip')
        cls.t = Path(tempfile.mkdtemp(prefix='nx2a_test_zip_')); (cls.t / 'h.c').write_text(HARNESS)
        src = REPO / 'nxnative/dm/android'
        subprocess.run([cc, '-O1', '-I', str(src), str(src / 'data_zip.c'), str(cls.t / 'h.c'), '-lz', '-o', str(cls.t / 'h')], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.t, ignore_errors=True)

    def run_h(self, *a): return subprocess.run([str(self.t / 'h'), *map(str, a)], check=True, capture_output=True, text=True).stdout.split()

    def tree(self, files):
        d = Path(tempfile.mkdtemp(dir=self.t))
        for n, b in files.items(): (d / n).parent.mkdir(parents=True, exist_ok=True); (d / n).write_bytes(b)
        return d

    def zip_of(self, entries, method=zipfile.ZIP_DEFLATED):
        p = Path(tempfile.mkstemp(dir=self.t, suffix='.zip')[1])
        with zipfile.ZipFile(p, 'w', method) as z:
            for n, b in entries.items(): z.writestr(n, b)
        return p

    def extract(self, zp):
        dest = [Path(tempfile.mkdtemp(dir=self.t)) / 's', Path(tempfile.mkdtemp(dir=self.t)) / 'd']
        n = int(self.run_h('x', zp, *dest)[0])
        files = [{str(f.relative_to(d)): f.read_bytes() for f in d.rglob('*') if f.is_file()} if d.exists() else {} for d in dest]
        return n, files

    def test_write(self):   # Eden's layout, the Finder's files left out, a standard zip
        save = self.tree({'Goose2': b'g' * 4096, 'sub/c.xml': b'<x/>', '.DS_Store': b'x', '._Goose2': b'x'}); dev = self.tree({'d.bin': bytes(range(256))})
        out = self.t / 'w.zip'
        self.assertEqual(self.run_h('w', out, save, TID, dev, TID + '_device'), ['0', '3'])
        with zipfile.ZipFile(out) as z:
            self.assertIsNone(z.testzip())
            self.assertEqual(sorted(z.namelist()), [f'{TID}/Goose2', f'{TID}/sub/c.xml', f'{TID}_device/d.bin'])
            self.assertEqual(z.read(f'{TID}/Goose2'), b'g' * 4096); self.assertEqual(z.read(f'{TID}_device/d.bin'), bytes(range(256)))

    def test_write_no_device_save(self):
        out = self.t / 'n.zip'
        self.assertEqual(self.run_h('w', out, self.tree({'a': b'1'}), TID, self.t / 'missing', TID + '_device'), ['0', '1'])
        with zipfile.ZipFile(out) as z: self.assertEqual(z.namelist(), [f'{TID}/a'])

    def test_eden_zip(self):   # deflated or stored, a Mac's __MACOSX and ._ files left out
        for method in (zipfile.ZIP_DEFLATED, zipfile.ZIP_STORED):
            n, (save, dev) = self.extract(self.zip_of({f'{TID}/Goose2': b'goose' * 100, f'{TID}/sub/c': b'c', f'__MACOSX/{TID}/._Goose2': b'x', f'{TID}/.DS_Store': b'x'}, method))
            self.assertEqual((n, save, dev), (2, {'Goose2': b'goose' * 100, 'sub/c': b'c'}, {}))

    def test_device_folder(self):
        n, (save, dev) = self.extract(self.zip_of({f'{TID}/a': b'1', f'{TID}_device/b': b'2'}))
        self.assertEqual((n, save, dev), (2, {'a': b'1'}, {'b': b'2'}))

    def test_files_at_top(self):
        n, (save, dev) = self.extract(self.zip_of({'a': b'1', 'b/c': b'2'}))
        self.assertEqual((n, save, dev), (2, {'a': b'1', 'b/c': b'2'}, {}))

    def test_refused(self):
        self.assertEqual(self.extract(self.zip_of({f'{TID}/../x': b'1'}))[0], -1)
        p = self.t / 'junk.zip'; p.write_bytes(b'not a zip' * 10)
        self.assertEqual(self.extract(p)[0], -1)
        self.assertEqual(self.extract(self.zip_of({f'{TID}/': b''}))[0], 0)

    def test_round_trip(self):
        files = {'Goose2': bytes(range(256)) * 16, 'SaveTime2': b't' * 256, 'x/y': b'z'}
        out = self.t / 'r.zip'; self.run_h('w', out, self.tree(files), TID)
        self.assertEqual(self.extract(out)[1][0], files)


if __name__ == '__main__':
    unittest.main()
