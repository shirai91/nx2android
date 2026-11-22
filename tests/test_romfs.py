# nxnative/dm/tools/romfs.py: the read-only views of a player's container (a mount over dm_romfs) and of a RomFS image (DMROMFS1),
# seen by the readers that opt into its os / open / mmap (python -m unittest discover tests).
import errno, os, select, shutil, signal, stat, struct, sys, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from porter.upstream import romfs   # noqa: E402  (dm/tools on sys.path)


class Tree:
    """A mount backend in memory: the same find / info / children / reader / read / read_into / stamp the container backend offers."""
    def __init__(self, files):
        self.files = {k: bytes(v) for k, v in files.items()}
        self.dirs = {''}
        for k in self.files:
            parts = k.split('/')
            for i in range(1, len(parts)): self.dirs.add('/'.join(parts[:i]))
    def find(self, rel):
        rel = rel.strip('/')
        if rel in self.files or rel in self.dirs: return rel
        raise FileNotFoundError(rel or '/')
    def info(self, k):
        if k in self.files: return True, len(self.files[k])
        if k in self.dirs: return False, 0
        raise FileNotFoundError(k or '/')
    def children(self, k):
        if k not in self.dirs: raise NotADirectoryError(k or '/')
        out = set()
        for p in list(self.dirs) + list(self.files):
            parent, _, name = p.rpartition('/')
            if p and parent == k: out.add(name)
        return sorted((n, f'{k}/{n}' if k else n) for n in out)
    def read(self, k, off, n): return self.files[k][off:off + n]
    def read_into(self, k, off, buffer, n):
        data = self.files[k][off:off + n]; buffer[:len(data)] = data; return len(data)
    def open(self, k): return k   # the container helper's handle: here the path itself
    def read_at(self, h, off, n): return self.files[h][off:off + n]
    def close(self, h): pass
    def reader(self, k): return romfs._DaemonFile(self, k)
    def stamp(self): return os.stat(__file__)


class Mount(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='romfs-mount-')
        self.tree = '/'.join((self.root, 'v655360', 'RomFS'))
        self.back = Tree({'main': b'\x00\x01\x02\x03' * 4, 'data/a.bin': bytes(range(256)) * 3,
                          'data/Pack/b.arc': b'BTCD' + b'\0' * 4092, 'glslc/x.glslc': b'#version 1\n'})
        romfs.mount(self.tree, self.back)
    def tearDown(self):
        romfs.unmount(self.tree)
        shutil.rmtree(self.root)
    def test_reads_and_stats(self):
        p = self.tree + '/data/a.bin'
        self.assertTrue(romfs.os.path.exists(p)); self.assertTrue(romfs.os.path.isfile(p))
        self.assertEqual(romfs.os.path.getsize(p), 768)
        with romfs.open(p, 'rb') as f:
            self.assertEqual(f.read(4), bytes(range(4)))
            f.seek(250); self.assertEqual(f.tell(), 250)
            self.assertEqual(f.read(10), bytes(range(250, 256)) + bytes(range(4)))
        st = romfs.stat(p); self.assertEqual(st.st_size, 768); self.assertTrue(st.st_mode & 0o444)
        self.assertTrue(romfs.isdir(self.tree + '/data')); self.assertFalse(romfs.isdir(p))
        self.assertEqual(romfs.stat(self.tree + '/data').st_size, 0)
        self.assertFalse(romfs.os.path.exists(self.tree + '/missing'))
    def test_text_mode(self):
        with romfs.open(self.tree + '/glslc/x.glslc', 'r') as f: self.assertEqual(f.readline(), '#version 1\n')
    def test_write_is_refused(self):
        with self.assertRaises(OSError) as e: romfs.open(self.tree + '/main', 'wb')
        self.assertEqual(e.exception.errno, errno.EROFS)
    def test_directory_open_is_refused(self):
        with self.assertRaises(IsADirectoryError): romfs.open(self.tree + '/data', 'rb')
    def test_walk_and_listdir(self):
        seen = [(os.path.relpath(dp, self.tree).replace(os.sep, '/'), sorted(fs)) for dp, _, fs in romfs.walk(self.tree)]
        self.assertEqual(seen, [('.', ['main']), ('data', ['a.bin']), ('data/Pack', ['b.arc']), ('glslc', ['x.glslc'])])
        self.assertEqual(sorted(romfs.os.listdir(self.tree)), ['data', 'glslc', 'main'])
        self.assertEqual(sorted(e.name for e in romfs.scandir(self.tree + '/data')), ['Pack', 'a.bin'])
        self.assertEqual([p.name for p in romfs.Path(self.tree).glob('**/*.arc')], ['b.arc'])
    def test_mmap_of_a_mounted_file(self):
        with romfs.open(self.tree + '/data/Pack/b.arc', 'rb') as f:
            with romfs.mmap.mmap(f.fileno(), 0, access=romfs.mmap.ACCESS_READ) as d:
                self.assertEqual(len(d), 4096)
                self.assertEqual(struct.unpack_from('<4s', d, 0)[0], b'BTCD')
                self.assertEqual(bytes(d[4:8]), b'\0\0\0\0')
                self.assertEqual(d.find(b'BTCD'), 0)
                d.seek(4); self.assertEqual(d.read(2), b'\0\0')
        self.assertEqual(romfs._fd_ranges, {})
    def test_fstat_of_a_container_entry(self):
        """getsize() on an open container file stat's its virtual fd: the entry's length, not an OS file's"""
        with romfs.open(self.tree + '/data/a.bin', 'rb') as f:
            entry = romfs.os.fstat(f.fileno())
            self.assertEqual(entry.st_size, 768)
            self.assertTrue(stat.S_ISREG(entry.st_mode))
        self.assertEqual(romfs._fd_ranges, {})
    def test_fstat_of_an_image_entry(self):
        """an image entry's real fd is the whole image's: stat through the facade gives the entry's length"""
        data = b'hello'
        name = b'a.bin'
        index = romfs.ENT.pack(1, 1, 0, 0, 0, 0) + romfs.ENT.pack(romfs.HEAD.size, len(data), 0, len(name), 1, 0) + name
        image = Path(self.root) / 'game.img'
        image.write_bytes(romfs.HEAD.pack(romfs.MAGIC, romfs.HEAD.size + len(data), len(index), 2) + data + index)
        with romfs.open(str(image) + '/' + name.decode(), 'rb') as f:
            self.assertEqual(os.fstat(f.fileno()).st_size, image.stat().st_size)   # the OS sees the image
            self.assertEqual(romfs.os.fstat(f.fileno()).st_size, len(data))        # the facade sees the entry
        self.assertEqual(romfs._fd_ranges, {})
    def test_unmount_restores_the_real_filesystem(self):
        romfs.unmount(self.tree)
        self.assertFalse(os.path.exists(self.tree))
        self.assertFalse(romfs.os.path.exists(self.tree))
    def test_real_paths_keep_their_writes(self):
        out = Path(self.root) / 'out.bin'
        with romfs.open(out, 'wb') as f: f.write(b'ok')
        self.assertEqual(out.read_bytes(), b'ok')
        self.assertFalse(romfs._resolve(out))
    def test_the_split_key_of_a_mounted_tree_equals_the_extracted_folder(self):
        from porter.upstream import nsp
        real = Path(self.root) / 'real'
        for rel, data in self.back.files.items():
            (real / rel).parent.mkdir(parents=True, exist_ok=True); (real / rel).write_bytes(data)
        nsp.romfs = romfs
        try: mounted = nsp.fingerprint(('tree', self.tree))
        finally: nsp.romfs = None
        self.assertEqual(mounted, nsp.fingerprint(('tree', real)))


@unittest.skipUnless(os.name == 'posix', 'the stand-in serve script runs through its shebang')
class Daemon(unittest.TestCase):
    """The real client (romfs.Container) against tests/fixtures/fake_container_serve.py, the protocol nx_extract --serve speaks."""
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='romfs-daemon-')
        self.tree = '/'.join((self.root, 'v1', 'RomFS'))
        exe = os.path.join(self.root, 'serve.py')
        shutil.copyfile(Path(__file__).parent / 'fixtures' / 'fake_container_serve.py', exe); os.chmod(exe, 0o755)
        self.back = romfs.Container(__file__, None, __file__, exe)
        romfs.mount(self.tree, self.back)
    def tearDown(self):
        romfs.unmount(self.tree)
        shutil.rmtree(self.root)
    def test_walk_descends_and_reads(self):
        seen = [(os.path.relpath(dp, self.tree).replace(os.sep, '/'), sorted(fs)) for dp, _, fs in romfs.walk(self.tree)]
        self.assertEqual(seen, [('.', ['main']), ('data', ['a.bin']), ('data/Pack', ['b.arc']), ('glslc', ['x.glslc'])])
        with romfs.open(self.tree + '/data/a.bin', 'rb') as f: self.assertEqual(f.read(4), bytes(range(4)))
        self.assertEqual(romfs.stat(self.tree + '/data/a.bin').st_size, 768)
        self.assertTrue(romfs.isdir(self.tree + '/data/Pack'))
        self.assertFalse(romfs.os.path.exists(self.tree + '/nope'))
    def test_mmap_of_a_daemon_file(self):
        with romfs.open(self.tree + '/data/Pack/b.arc', 'rb') as f:
            with romfs.mmap.mmap(f.fileno(), 0, access=romfs.mmap.ACCESS_READ) as d:
                self.assertEqual(struct.unpack_from('<4s', d, 0)[0], b'BTCD')
                self.assertEqual(d.find(b'BTCD'), 0)
        self.assertEqual(romfs._fd_ranges, {})
    @unittest.skipUnless(hasattr(os, 'fork'), 'the build forks its shader workers on POSIX')
    def test_a_forked_worker_uses_its_own_reader(self):
        """A worker forked after the mount shares the pipe, not only the reader: it must start one of its own"""
        parent_reader = self.back.proc.pid
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            try:
                os.close(r)
                with romfs.open(self.tree + '/data/Pack/b.arc', 'rb') as f: data = f.read(4)
                reader = self.back.proc
                os.write(w, f'{romfs.os.path.getsize(self.tree + "/data/Pack/b.arc")} {data.hex()} {reader.pid}'.encode())
                self.back.shutdown()
                try: reader.wait(timeout=5)
                except Exception: pass
            except BaseException as error:
                try: os.write(w, f'error {error!r}'.encode())
                except OSError: pass
            finally:
                os._exit(0)
        os.close(w)
        ready, _, _ = select.select([r], [], [], 10)
        if not ready: os.kill(pid, signal.SIGKILL)
        out = os.read(r, 4096) if ready else b''
        os.close(r)
        _, status = os.waitpid(pid, 0)
        self.assertTrue(ready, f'the forked worker did not answer: {out!r}')
        self.assertEqual(os.waitstatus_to_exitcode(status), 0)
        self.assertTrue(out.startswith(b'4096 42544344 '), out)
        self.assertNotEqual(int(out.rsplit(b' ', 1)[1]), parent_reader)
        self.assertEqual(romfs.os.path.getsize(self.tree + '/data/a.bin'), 768)   # and the parent's reader still works


if __name__ == '__main__':
    unittest.main()