"""Read-only paths inside DMROMFS1 images and the player's own containers, without extracting their contents.

Readers opt into Path/open/os/mmap here; the real filesystem is untouched. A path
like /game/RomFS.img/Data/archive is a bounded view of that image entry, and a path
under a mounted container (/game/extract/v655360/RomFS/Data/archive) is read out of
the player's .nsp / .xci by nx_extract's --serve mode (LibHac, the same merge and
decryption the extraction uses): the build reads the bytes it needs and writes
nothing. Ordinary paths retain their normal behavior, including writes to
generated outputs.
"""
import builtins
import errno
import fnmatch
import functools
import io
import json
import mmap as _mmap
import os as _os
from pathlib import Path as _Path
import stat as _stat
import struct
import sys
import tempfile
import threading
from types import SimpleNamespace
from typing import TYPE_CHECKING

HEAD = struct.Struct('<8sQQQ')
ENT = struct.Struct('<QQIHBB')
MAGIC = b'DMROMFS1'
_PREAD = getattr(_os, 'pread', None)   # absent on Windows: seek and read instead


class Image:
    def __init__(self, path):
        self.path = _os.path.abspath(path)
        self.stat = _os.stat(self.path)
        with builtins.open(self.path, 'rb') as f:
            head = f.read(HEAD.size)
            if len(head) < HEAD.size: raise ValueError(f'invalid RomFS image header: {path}')
            magic, off, size, count = HEAD.unpack(head)
            if magic != MAGIC or not 0 < count <= 0xffffffff or off < HEAD.size or off > self.stat.st_size or size != self.stat.st_size - off or count > size // ENT.size:
                raise ValueError(f'invalid RomFS image header: {path}')
            f.seek(off); self.index = f.read(size)
        if len(self.index) != size: raise ValueError(f'incomplete RomFS image: {path}')
        self.count, self.pool = count, count * ENT.size
        next_child = 1
        for k in range(count):
            a, b, no, nl, t, flags = self.entry(k)
            if no + nl > size - self.pool or t > 1 or flags > t or (k == 0 and (t or nl)):   # flags: 1 on a damaged file (romfs_image.py), 0 otherwise
                raise ValueError(f'invalid RomFS image entry {k}: {path}')
            name = self.name(k)
            if k and (not nl or nl > 0x300 or b'\0' in name or b'/' in name or name in (b'.', b'..')):
                raise ValueError(f'invalid RomFS image name: {path}')
            if t:
                if a < HEAD.size or a > off or b > off - a: raise ValueError(f'invalid RomFS image file range: {path}')
            else:
                if a != next_child or a <= k or a > count or b > count - a: raise ValueError(f'invalid RomFS image tree: {path}')
                next_child += b
        if next_child != count: raise ValueError(f'unreachable RomFS image entries: {path}')
        for k in range(count):
            a, b, _, _, t, _ = self.entry(k)
            if not t and any(self.name(j - 1) >= self.name(j) for j in range(a + 1, a + b)):
                raise ValueError(f'unsorted or duplicate RomFS image names: {path}')

    def entry(self, k): return ENT.unpack_from(self.index, k * ENT.size)
    def name(self, k):
        _, _, no, nl, _, _ = self.entry(k)
        return self.index[self.pool + no:self.pool + no + nl]

    def info(self, k):
        _, b, _, _, t, _ = self.entry(k)
        return bool(t), (b if t else 0)

    def find(self, relative):
        k = 0
        for part in relative.split('/'):
            if not part or part == '.': continue
            a, b, _, _, t, _ = self.entry(k)
            if t: raise NotADirectoryError(relative)
            lo, hi, wanted = a, a + b, _os.fsencode(part)
            while lo < hi:
                mid = (lo + hi) // 2
                if self.name(mid) < wanted: lo = mid + 1
                else: hi = mid
            if lo == a + b or self.name(lo) != wanted: raise FileNotFoundError(relative)
            k = lo
        return k

    def children(self, k):
        a, b, _, _, t, _ = self.entry(k)
        if t: raise NotADirectoryError(self.path)
        return [(_os.fsdecode(self.name(j)), j) for j in range(a, a + b)]

    def reader(self, k): return _File(self, k)

    def stamp(self): return self.stat


class Container:
    """The RomFS of a player's own container (an NSP, PFS0, or a gamecard image, XCI) through `nx_extract <keys> --serve <base> <update|->`: LibHac opens the base's
    Program NCA and layers the update's BKTR over it exactly as the extraction does; an integrated image is passed as both sides so its Patch content meta (the update
    the console runs) is what gets read. Requests: STAT / LIST / OPEN / READ / CLOSE, framed on stdin/stdout. The whole tree is indexed by the helper once at start, so
    listing and stat'ing are cheap; the bytes are decrypted only as they are read."""
    def __init__(self, base, update=None, keys=None, exe=None):
        self.base = _os.path.abspath(_os.fsdecode(base))
        self.update = _os.path.abspath(_os.fsdecode(update)) if update else None
        self.keys = _os.fsdecode(keys) if keys else _os.environ.get('DM_PROD_KEYS')
        exe = exe or _os.environ.get('DM_ROMFS_EXE')
        if not self.keys: raise OSError(f'{self.base}: no prod.keys (pass keys or set DM_PROD_KEYS)')
        if not exe: raise OSError('no nx_extract to read a container with (pass exe or set DM_ROMFS_EXE)')
        self.exe = exe
        self.files, self.dirs = {}, {}
        self._start_reader()
        self.stat_base = _os.stat(self.base)

    def _start_reader(self):
        import subprocess
        self.err = tempfile.TemporaryFile()
        self.proc = subprocess.Popen([self.exe, self.keys, '--serve', self.base, self.update or '-'],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.err, bufsize=0)
        self.lock = threading.Lock()
        self.pid = _os.getpid()
        if self.proc.stdout.read(1) != b'\0': raise OSError(f'{self.base}: the container reader did not start: {self.message()}')

    def _forked(self):
        """A forked child shares the parent's pipe descriptors, not only its reader: two processes' frames would
        interleave. The child closes its copies (the parent's reader keeps running) and starts its own."""
        for f in (self.proc.stdin, self.proc.stdout, self.err):
            try: f.close()
            except (OSError, ValueError): pass
        self._start_reader()

    def message(self):
        try:
            self.err.seek(0); text = self.err.read().decode('utf-8', 'replace').strip()
        except (OSError, ValueError): text = ''
        return text or 'no message'

    def request(self, op, payload=b''):
        if self.pid != _os.getpid(): self._forked()
        with self.lock:
            try:
                self.proc.stdin.write(bytes([op]) + struct.pack('<I', len(payload)) + payload); self.proc.stdin.flush()
                status = self.read_exact(1)[0]
                data = self.read_exact(struct.unpack('<I', self.read_exact(4))[0])
            except (OSError, ValueError):
                raise OSError(f'{self.base}: the container reader stopped: {self.message()}') from None
        if status: raise OSError(_os.fsdecode(data))
        return data

    def read_exact(self, n):
        chunks, left = [], n
        while left:
            b = self.proc.stdout.read(left)   # a pipe hands out what has arrived: loop to the frame's length
            if not b: raise OSError(f'{self.base}: the container reader stopped: {self.message()}')
            chunks.append(b); left -= len(b)
        return chunks[0] if len(chunks) == 1 else b''.join(chunks)

    def find(self, relative):
        return relative.strip('/')

    def info(self, k):
        k = k.strip('/')
        hit = self.files.get(k)
        if hit is not None: return hit
        if k in self.dirs: return False, 0
        d = self.request(1, path16(k))
        hit = (d[0] == 0, struct.unpack_from('<q', d, 1)[0])
        if hit[0]: self.files[k] = hit
        else: self.dirs[k] = None
        return hit

    def children(self, k):
        k = k.strip('/')
        hit = self.dirs.get(k)
        if hit is not None: return hit
        d = self.request(2, path16(k))
        count, o, out = struct.unpack_from('<i', d, 0)[0], 4, []
        for _ in range(count):
            n = struct.unpack_from('<H', d, o)[0]; o += 2
            name = _os.fsdecode(d[o:o + n]); o += n
            isdir = d[o] != 0; o += 1
            size = struct.unpack_from('<q', d, o)[0]; o += 8
            child = f'{k}/{name}' if k else name
            out.append((name, child))
            if isdir: self.dirs.setdefault(child, None)
            else: self.files[child] = (True, size)
        self.dirs[k] = out
        return out

    def open(self, k):
        return struct.unpack_from('<i', self.request(3, path16(k.strip('/'))), 0)[0]

    def read(self, k, offset, n):
        h = self.open(k)
        try: return self.read_at(h, offset, n)
        finally: self.close(h)

    def read_at(self, h, offset, n):
        d = self.request(4, struct.pack('<iqi', h, offset, n))
        return d[4:4 + struct.unpack_from('<i', d, 0)[0]]

    def read_into(self, k, offset, buffer, n):
        data = self.read(k, offset, n)
        buffer[:len(data)] = data
        return len(data)

    def close(self, h):
        self.request(5, struct.pack('<i', h))

    def reader(self, k): return _DaemonFile(self, k)

    def stamp(self): return self.stat_base

    def shutdown(self):
        # Closing stdin is what ends the helper (it returns on EOF); no wait or signal, which at interpreter shutdown reach half-cleared modules
        proc, self.proc = getattr(self, 'proc', None), None
        if proc is None: return
        try: proc.stdin.close()
        except Exception: pass
        try: proc.stdout.close()
        except Exception: pass
        try: self.err.close()
        except Exception: pass

    def __del__(self):
        try: self.shutdown()
        except BaseException: pass


def path16(s):
    b = _os.fsencode(s)
    return struct.pack('<H', len(b)) + b


@functools.lru_cache(maxsize=8)
def _image(path, identity): return Image(path) if is_image(path) else None


def is_image(path):
    try:
        with builtins.open(path, 'rb') as f: return f.read(8) == MAGIC
    except (OSError, TypeError): return False


_mounts = []   # [(root, backend)], longest root first
_SEP = _os.sep

def _root(path):
    return _os.path.abspath(_os.fsdecode(path)).rstrip('/' + _SEP) or _SEP

def _under(p, root):
    """p's '/'-separated path below root, '' for root itself, or None: mounts match on the platform's separators, backends key on '/'"""
    rp, rt = _os.path.normcase(p), _os.path.normcase(root)
    if rp == rt: return ''
    head = rt if rt.endswith(_SEP) else rt + _SEP
    return p[len(head):].replace(_SEP, '/') if rp.startswith(head) else None

def mount(path, backend):
    """Make the path a view of backend (a Container, or anything with find / info / children / reader / read / stamp): the path itself need not exist."""
    root = _root(path)
    unmount(root)
    _mounts.append((root, backend)); _mounts.sort(key=lambda e: -len(e[0]))
    return root


def unmount(path):
    root = _root(path)
    keep, gone = [], []
    for e in _mounts:
        if e[0] == root: gone.append(e[1])
        else: keep.append(e)
    _mounts[:] = keep
    for backend in gone:
        if hasattr(backend, 'shutdown'): backend.shutdown()


def mounted(path):
    p = _os.path.abspath(_os.fsdecode(path))
    for root, _ in _mounts:
        if _under(p, root) is not None: return root
    return None


def _resolve(path):
    p = _os.path.abspath(_os.fsdecode(path))
    for root, backend in _mounts:
        relative = _under(p, root)
        if relative is not None: return backend, relative
    parts = p.split(_SEP)
    for i, part in enumerate(parts):
        if not part.endswith('.img'): continue
        image = _SEP.join(parts[:i + 1])
        try: st = _os.stat(image)
        except OSError: continue
        if not _stat.S_ISREG(st.st_mode): continue
        loaded = _image(image, (st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns))
        if loaded is not None: return loaded, '/'.join(parts[i + 1:])
    return None


def mounts_from_env(var='DM_ROMFS_MOUNTS'):
    """JSON [{path, base, update, keys, exe}] (a container view per RomFS dir the build would have extracted, porter/pipeline.py) into this process's mounts."""
    for ent in json.loads(_os.environ.get(var) or '[]'):
        mount(ent['path'], Container(ent['base'], ent.get('update') or None, ent.get('keys') or None, ent.get('exe') or None))


mounts_from_env()


def source(extraction):
    image = _Path(extraction) / 'RomFS.img'
    return Path(image if image.is_file() else _Path(extraction) / 'RomFS')


_next_fd = [1 << 24]
_fd_ranges = {}   # virtual fd -> ('image', start, length) or ('node', backend, path)

def _virtual_fd():
    while _next_fd[0] in _fd_ranges: _next_fd[0] += 1
    fd = _next_fd[0]; _next_fd[0] += 1
    return fd


class _File(io.RawIOBase):
    def __init__(self, image, k):
        self.fd = _os.open(image.path, _os.O_RDONLY)
        self.start, self.length = image.entry(k)[:2]
        self.pos = 0
        _fd_ranges[self.fd] = ('image', self.start, self.length)
    def readable(self): return True
    def seekable(self): return True
    def fileno(self): return self.fd
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        pos = offset + (self.pos if whence == 1 else self.length if whence == 2 else 0)
        if whence not in (0, 1, 2) or pos < 0: raise ValueError('invalid seek')
        self.pos = pos; return pos
    def readinto(self, buffer):
        size = min(len(buffer), max(0, self.length - self.pos))
        if _PREAD:
            data = _PREAD(self.fd, size, self.start + self.pos)
        else:
            _os.lseek(self.fd, self.start + self.pos, 0); data = _os.read(self.fd, size)
        buffer[:len(data)] = data; self.pos += len(data)
        return len(data)
    def close(self):
        if not self.closed:
            _fd_ranges.pop(self.fd, None); _os.close(self.fd)
        super().close()


class _DaemonFile(io.RawIOBase):
    """A container entry as an ordinary file object, read through the helper's handle. fileno() is a virtual number: mmap() below maps it, the OS never sees it."""
    large_buffer = True   # the façade reads it in 4 MiB steps: one request each, not one per 8 KiB
    def __init__(self, backend, path):
        self.backend, self.path = backend, path
        is_file, self.length = backend.info(path)
        if not is_file: raise IsADirectoryError(path)
        self.handle = backend.open(path); self.pos = 0
        self.fd = _virtual_fd(); _fd_ranges[self.fd] = ('node', backend, path)
    def readable(self): return True
    def seekable(self): return True
    def fileno(self): return self.fd
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        pos = offset + (self.pos if whence == 1 else self.length if whence == 2 else 0)
        if whence not in (0, 1, 2) or pos < 0: raise ValueError('invalid seek')
        self.pos = pos; return pos
    def readinto(self, buffer):
        size = min(len(buffer), max(0, self.length - self.pos))
        if not size: return 0
        data = self.backend.read_at(self.handle, self.pos, size)
        buffer[:len(data)] = data; self.pos += len(data)
        return len(data)
    def close(self):
        if not self.closed:
            _fd_ranges.pop(self.fd, None)
            try: self.backend.close(self.handle)
            except (OSError, ValueError): pass
        super().close()


# Type checkers see the builtin's overloads, so a reader's open(..., 'rb').read() stays bytes and 'r' stays str.
if TYPE_CHECKING:
    from builtins import open
else:
    def open(file, mode='r', buffering=-1, encoding=None, errors=None, newline=None, **kwargs):
        resolved = _resolve(file) if not isinstance(file, int) else None
        if not resolved: return builtins.open(file, mode, buffering, encoding, errors, newline, **kwargs)
        if mode not in ('r', 'rb', 'rt'): raise OSError(errno.EROFS, 'RomFS images and containers are read-only', file)
        backend, relative = resolved; k = backend.find(relative)
        if not backend.info(k)[0]: raise IsADirectoryError(file)
        raw = backend.reader(k)
        if buffering == 0 and 'b' in mode: return raw
        if buffering < 0: buffering = 4 << 20 if getattr(raw, 'large_buffer', False) else io.DEFAULT_BUFFER_SIZE
        reader = io.BufferedReader(raw, buffering or io.DEFAULT_BUFFER_SIZE)
        return reader if 'b' in mode else io.TextIOWrapper(reader, encoding=encoding, errors=errors, newline=newline)


def stat(path, **kwargs):
    resolved = _resolve(path)
    if not resolved: return _os.stat(path, **kwargs)
    backend, relative = resolved
    return _entry_stat(backend, backend.find(relative))


def _entry_stat(backend, k):
    is_file, size = backend.info(k); s = backend.stamp()
    return SimpleNamespace(st_mode=(_stat.S_IFREG | 0o444) if is_file else (_stat.S_IFDIR | 0o555), st_size=size,
                           st_ino=hash(k) & 0x7fffffff, st_dev=s.st_dev, st_mtime=s.st_mtime, st_mtime_ns=s.st_mtime_ns,
                           st_ctime=s.st_ctime, st_ctime_ns=s.st_ctime_ns, st_atime=s.st_atime, st_atime_ns=s.st_atime_ns)


def fstat(fd):
    """A container entry's file descriptor is a virtual one: its stat comes from the entry, not from the OS.
    An image entry's fd is the image's: its size is the entry's length, not the whole image's."""
    range_ = _fd_ranges.get(fd)
    if range_ is None: return _os.fstat(fd)
    if range_[0] == 'node':
        backend, k = range_[1], range_[2]
        return _entry_stat(backend, k)
    st = _os.fstat(fd)
    return SimpleNamespace(st_mode=st.st_mode, st_size=range_[2], st_ino=st.st_ino, st_dev=st.st_dev,
                           st_nlink=st.st_nlink, st_uid=st.st_uid, st_gid=st.st_gid, st_atime=st.st_atime,
                           st_mtime=st.st_mtime, st_ctime=st.st_ctime, st_atime_ns=st.st_atime_ns,
                           st_mtime_ns=st.st_mtime_ns, st_ctime_ns=st.st_ctime_ns)


def exists(path):
    try: stat(path); return True
    except OSError: return False
def isdir(path):
    try: return _stat.S_ISDIR(stat(path).st_mode)
    except OSError: return False
def isfile(path):
    try: return _stat.S_ISREG(stat(path).st_mode)
    except OSError: return False


class _Entry:   # an entry as the scan that listed it found it: its type, size and inode come from that scan, not from the path resolved again
    def __init__(self, parent, name, backend, k): self.name, self.path, self._backend, self._k = name, _os.path.join(parent, name), backend, k
    def stat(self, **kwargs): return _entry_stat(self._backend, self._k)
    def is_dir(self, **kwargs): return not self._backend.info(self._k)[0]
    def is_file(self, **kwargs): return self._backend.info(self._k)[0]
    def is_symlink(self): return False
    def inode(self): return hash(self._k) & 0x7fffffff
    def __fspath__(self): return self.path


class _Scan:
    def __init__(self, entries): self.entries = iter(entries)
    def __iter__(self): return self
    def __next__(self): return next(self.entries)
    def __enter__(self): return self
    def __exit__(self, *args): self.close()
    def close(self): pass


def scandir(path):
    resolved = _resolve(path)
    if not resolved: return _os.scandir(path)
    backend, relative = resolved
    return _Scan(_Entry(path, name, backend, k) for name, k in backend.children(backend.find(relative)))


def walk(top, topdown=True, onerror=None, followlinks=False):
    if not _resolve(top):
        yield from _os.walk(top, topdown=topdown, onerror=onerror, followlinks=followlinks); return
    def visit(path):
        try:
            dirs, files = [], []
            with scandir(path) as entries:
                for e in entries: (dirs if e.is_dir() else files).append(e.name)
        except OSError as error:
            if onerror: onerror(error)
            return
        if topdown: yield str(path), dirs, files
        for name in dirs: yield from visit(_os.path.join(path, name))
        if not topdown: yield str(path), dirs, files
    yield from visit(top)


class Path(type(_Path())):
    def stat(self, **kwargs): return stat(self, **kwargs)
    def exists(self, **kwargs): return exists(self)
    def is_dir(self, **kwargs): return isdir(self)
    def is_file(self, **kwargs): return isfile(self)
    def read_bytes(self):
        with self.open('rb') as f: return f.read()
    def read_text(self, encoding=None, errors=None, newline=None):
        with self.open('r', encoding=encoding, errors=errors, newline=newline) as f: return f.read()
    def open(self, mode='r', buffering=-1, encoding=None, errors=None, newline=None):
        return open(self, mode, buffering, encoding, errors, newline)
    def iterdir(self):
        with scandir(self) as entries:
            for e in entries: yield self / e.name
    def glob(self, pattern, **kwargs):
        if not _resolve(self): yield from super().glob(pattern, **kwargs); return
        if '/' not in pattern:
            yield from (p for p in self.iterdir() if fnmatch.fnmatchcase(p.name, pattern)); return
        for directory, dirs, files in walk(self):
            for name in dirs + files:
                p = Path(directory) / name; rel = p.relative_to(self).as_posix()
                if fnmatch.fnmatchcase(rel, pattern) or (pattern.startswith('**/') and fnmatch.fnmatchcase(rel, pattern[3:])): yield p
    def rglob(self, pattern, **kwargs):
        if not _resolve(self): yield from super().rglob(pattern, **kwargs); return
        yield from self.glob('**/' + str(pattern))


class _PathOps:
    exists = staticmethod(exists); isdir = staticmethod(isdir); isfile = staticmethod(isfile)
    getsize = staticmethod(lambda path: stat(path).st_size)
    def __getattr__(self, name): return getattr(_os.path, name)
class _OS:
    path = _PathOps()
    stat = staticmethod(stat); fstat = staticmethod(fstat); scandir = staticmethod(scandir); walk = staticmethod(walk)
    @staticmethod
    def listdir(path):
        with scandir(path) as entries: return [e.name for e in entries]
    def __getattr__(self, name): return getattr(_os, name)
os = _OS()


class _Map:
    """mmap-compatible bounded buffer. Python 3.12's buffer protocol keeps archive parsing zero-copy."""
    def __init__(self, fd, start, length):
        self.length, self.pos = length, 0
        self.delta = start % _mmap.ALLOCATIONGRANULARITY
        self.mapping = _mmap.mmap(fd, length + self.delta, access=_mmap.ACCESS_READ, offset=start - self.delta)
        self.view = memoryview(self.mapping)[self.delta:self.delta + length]
    def __buffer__(self, flags): return self.view
    def __len__(self): return self.length
    def __getitem__(self, key):
        value = self.view[key]
        return value.tobytes() if isinstance(key, slice) else value
    def __contains__(self, value): return self.find(value) >= 0
    def find(self, value, start=0, end=None):
        a, b, _ = slice(start, end).indices(self.length)
        found = self.mapping.find(value, self.delta + a, self.delta + b)
        return found - self.delta if found >= 0 else -1
    def rfind(self, value, start=0, end=None):
        a, b, _ = slice(start, end).indices(self.length)
        found = self.mapping.rfind(value, self.delta + a, self.delta + b)
        return found - self.delta if found >= 0 else -1
    def index(self, value, start=0, end=None):
        found = self.find(value, start, end)
        if found < 0: raise ValueError('subsection not found')
        return found
    def size(self): return self.length
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        pos = offset + (self.pos if whence == 1 else self.length if whence == 2 else 0)
        if whence not in (0, 1, 2) or not 0 <= pos <= self.length: raise ValueError('invalid mmap seek')
        self.pos = pos
    def read(self, size=-1):
        end = self.length if size < 0 else min(self.length, self.pos + size)
        data = self[self.pos:end]; self.pos = end; return data
    def close(self): self.view.release(); self.mapping.close()
    def __enter__(self): return self
    def __exit__(self, *args): self.close()


class _Mmap:
    @staticmethod
    def mmap(fd, length, *args, **kwargs):
        range_ = _fd_ranges.get(fd)
        if range_ is None: return _mmap.mmap(fd, length, *args, **kwargs)
        if args or kwargs.get('access', _mmap.ACCESS_READ) != _mmap.ACCESS_READ: raise ValueError('RomFS mappings are read-only')
        offset = kwargs.get('offset', 0)
        if range_[0] == 'node':   # a container entry: read once into memory (shader_split only maps archives whose shaders it reads whole)
            backend, path = range_[1], range_[2]
            size = backend.info(path)[1]
            if offset < 0 or offset > size or (length and length > size - offset): raise ValueError('mapping exceeds RomFS file')
            n = length or size - offset
            limit = int(_os.environ.get('DM_ROMFS_MAP_LIMIT', 2 << 30))
            if n > limit: raise OSError(f'{path}: {n / 2**30:.1f} GiB mapped from a container (DM_ROMFS_MAP_LIMIT={limit}); extract this file first')
            data = backend.read(path, offset, n)
            if len(data) != n: raise OSError(f'{path}: short read ({len(data)} of {n})')
            return _MemoryMap(data)
        if sys.version_info < (3, 12): raise RuntimeError('RomFS image archive scans require Python 3.12 or newer')
        _, start, size = range_
        if offset < 0 or offset > size or (length and length > size - offset): raise ValueError('mapping exceeds RomFS file')
        return _Map(fd, start + offset, length or size - offset)
    def __getattr__(self, name): return getattr(_mmap, name)
mmap = _Mmap()


class _MemoryMap:
    """mmap-compatible buffer over bytes a container entry decrypted (no file descriptor to map)."""
    def __init__(self, data):
        self.data = data; self.view = memoryview(data); self.length, self.pos = len(data), 0
    def __buffer__(self, flags): return self.view
    def __len__(self): return self.length
    def __getitem__(self, key):
        value = self.view[key]
        return value.tobytes() if isinstance(key, slice) else value
    def __contains__(self, value): return self.find(value) >= 0
    def find(self, value, start=0, end=None):
        a, b, _ = slice(start, end).indices(self.length)
        return self.data.find(value, a, b)
    def rfind(self, value, start=0, end=None):
        a, b, _ = slice(start, end).indices(self.length)
        return self.data.rfind(value, a, b)
    def index(self, value, start=0, end=None):
        found = self.find(value, start, end)
        if found < 0: raise ValueError('subsection not found')
        return found
    def size(self): return self.length
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        pos = offset + (self.pos if whence == 1 else self.length if whence == 2 else 0)
        if whence not in (0, 1, 2) or not 0 <= pos <= self.length: raise ValueError('invalid mmap seek')
        self.pos = pos
    def read(self, size=-1):
        end = self.length if size < 0 else min(self.length, self.pos + size)
        data = self[self.pos:end]; self.pos = end; return data
    def close(self): self.view.release()
    def __enter__(self): return self
    def __exit__(self, *args): self.close()


if __name__ == '__main__':
    try:
        root = Path(sys.argv[1]); marker = sys.argv[2] if len(sys.argv) > 2 else ''
        if not root.is_dir() or not (root / marker).exists(): raise FileNotFoundError(f'missing RomFS marker {marker!r} in {root}')
    except (OSError, ValueError, IndexError, struct.error) as error: sys.exit(f'[RED] {error}')