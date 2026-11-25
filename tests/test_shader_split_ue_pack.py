"""shader_split.py: an Unreal modern pak (version 11) and the IoStore containers are not read whole (their shaders are in the shader code archives), a legacy pak still is; synthetic payloads, no game assets."""
import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'
CTL_MAGIC, GFX_MAGIC = bytes.fromhex('34127698'), bytes.fromhex('78563412')
PAK_MAGIC = 0x5A6F12E1   # uepak.py: the magic in a pak's footer


def pak(version, size):
    """A pak file of the size bytes with the footer the reader looks for (the magic and the version): version 4's is 0x2c from the end, version 11's 0xc8"""
    d = bytearray(size)
    struct.pack_into('<II', d, size - (0x2c if version == 4 else 0xc8), PAK_MAGIC, version)
    return bytes(d)


class Counted:
    """A file that counts the bytes read from it"""

    def __init__(self, file, count):
        self.file = file
        self.count = count

    def read(self, n=-1):
        d = self.file.read(n)
        self.count[0] += len(d)
        return d

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.file.close()

    def __getattr__(self, name):
        return getattr(self.file, name)


def counting_open(count, paths):
    """open that counts the bytes read from the given paths"""
    real = open

    def op(path, *a, **kw):
        f = real(path, *a, **kw)
        return Counted(f, count) if str(path) in paths else f

    return op


class ModernPak(unittest.TestCase):
    """The split of an Unreal modern pak or an IoStore container: the file is not read whole"""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(TOOLS))
        spec = importlib.util.spec_from_file_location('split_uepak_test', TOOLS / 'shader_split.py')
        cls.s = importlib.util.module_from_spec(spec)
        os.environ.setdefault('DM_GAME', '0100000000010000')
        spec.loader.exec_module(cls.s)
        cls.tmp = Path(tempfile.mkdtemp(prefix='nx2a-uepak-'))

    @classmethod
    def tearDownClass(cls):
        import shutil
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def scan(self, name, data):
        p = self.tmp / name
        p.write_bytes(data)
        count = [0]
        self.s.open = counting_open(count, {str(p)})
        return self.s.scan_file((str(p), name)), count[0]

    def test_a_modern_pak_is_not_read_whole(self):
        (rel, uniq, pairs, extra), read = self.scan('FFRS-Switch.pak', pak(11, 1 << 20))
        self.assertEqual((uniq, pairs, extra), ({}, set(), {}))   # no shaders: they are in the shader code archives
        self.assertLess(read, 4096)   # the footer only, not the file

    def test_an_iostore_container_is_not_read_whole(self):
        for name in ('FFRS-Switch.ucas', 'FFRS-Switch.utoc'):
            (rel, uniq, pairs, extra), read = self.scan(name, b'\0' * (1 << 20))
            self.assertEqual((uniq, pairs, extra), ({}, set(), {}))
            self.assertLess(read, 4096)   # the head only, not the file

    def test_a_legacy_pak_is_read_as_before(self):
        (rel, uniq, pairs, extra), read = self.scan('pakchunk0-Switch.pak', pak(4, 1 << 20))
        self.assertGreater(read, 1 << 19)   # the whole file: its shaders are inline in the cooked packages


def lslg(tag, code, ctl_len=0x880):
    """A decoded Switch shader (LSLG): the tag, the code area and the control section (the control's own length, its code area's length)"""
    ctl = bytearray(ctl_len)
    ctl[0:4] = CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, ctl_len)
    struct.pack_into('<I', ctl, 0x704, len(code))
    return b'LSLG' + tag + b'\0' * 6 + struct.pack('<I', len(code)) + code + struct.pack('<I', ctl_len) + bytes(ctl) + b'\0' * 24


def code_area(n, seed=0):
    c = bytearray(0x30 + 0x50 + n)
    c[0:4] = GFX_MAGIC
    for i in range(0x30, len(c), 4):
        c[i] = (i * 7) & 0xff
    struct.pack_into('<I', c, 0x30, 0x0461)   # the SPH word: shader type 1 (vertex)
    struct.pack_into('<I', c, 0x34, seed)
    return bytes(c)


def archive(shaders):
    """A version 2 shader code archive: the tables, then the code"""
    head = bytearray(struct.pack('<I', 2)) + struct.pack('<I', 0) * 2 + struct.pack('<I', 0)
    head += struct.pack('<I', len(shaders))
    off = 0
    for rec, fr in shaders:
        head += struct.pack('<QIIB', off, len(rec), len(rec), fr)
        off += len(rec)
    head += struct.pack('<I', 0) + struct.pack('<I', 0)
    return bytes(head) + b''.join(r for r, _ in shaders)


def digest(folder):
    out = {}
    for p in sorted(folder.rglob('*')):
        if p.is_file():
            out[str(p.relative_to(folder))] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


@unittest.skipUnless(importlib.util.find_spec('lz4'), 'no lz4')
class Products(unittest.TestCase):
    """The split of a game with an Unreal modern pak: the package and the tables are the same as without it"""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='nx2a-uepak-prod-'))
        (cls.tmp / 'romfs').mkdir()
        (cls.tmp / 'ue_shaders').mkdir()
        (cls.tmp / 'ue_shaders' / 'ShaderArchive-Test.ushaderbytecode').write_bytes(
            archive([(lslg(b'SV', code_area(4096, i)), 0) for i in range(8)] + [(lslg(b'SP', code_area(2048, i)), 3) for i in range(4)]))
        (cls.tmp / 'main.nso').write_bytes(bytes(0x100))   # an empty NSO: no shaders in main

    @classmethod
    def tearDownClass(cls):
        import shutil
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def split(self, name):
        out = self.tmp / name
        out.mkdir()
        # the porter runs the split with PYTHONUTF8=1 (porter/pipeline.py): the tables hold a '→' in their header, which the runner's code page cannot encode
        env = dict(os.environ, DM_GAME='0100000000010000', DM_UE_SHADERS=str(self.tmp / 'ue_shaders'), PYTHONUTF8='1')
        r = subprocess.run([sys.executable, str(TOOLS / 'shader_split.py'), str(self.tmp / 'romfs'), str(self.tmp / 'main.nso'),
                            str(out / 'pkg'), str(out / 'map.tsv'), str(out / 'facts.tsv'), str(out / 'pairs.tsv')],
                           env=env, capture_output=True, text=True, encoding='utf-8')
        return r, out

    def test_the_modern_pak_changes_nothing(self):
        r1, a = self.split('a')
        self.assertEqual(r1.returncode, 0, r1.stderr)
        (self.tmp / 'romfs' / 'FFRS-Switch.pak').write_bytes(pak(11, 1 << 20))
        r2, b = self.split('b')
        self.assertEqual(r2.returncode, 0, r2.stderr)
        self.assertEqual(digest(a), digest(b))   # the package and the three tables, byte for byte
        self.assertIn('skipped FFRS-Switch.pak', r2.stdout)   # and the log says what it skipped


if __name__ == '__main__':
    unittest.main()
