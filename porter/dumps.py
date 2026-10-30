# Compressed dumps: NSZ / XCZ (the nsz tool's format: an NSP / XCI whose NCAs are NCZ files, zstd-compressed past their first 0x4000 bytes) → an NSP in a temporary folder,
# which nx_extract (LibHac) reads; it reads NCAs only. An NCZ's section table carries each section's AES-CTR key and counter (the data was decrypted to compress it), so the
# conversion needs no keys: it decompresses and encrypts each section again. An XCZ becomes an NSP of its secure partition: nx_extract opens an XCI only to reach that
# partition's NCAs, which carry their keys in their key area (no ticket). Every NCA made is checked against its name, the first 16 bytes of its SHA-256 (as in every NSP / XCI).
# Layouts as nsz 4.x/5.x writes them (nsz/Decompressor.py, BlockDecompressorReader.py, Header.py).
import hashlib, os, re, struct
from pathlib import Path

from porter.upstream import BuildError

EXTS = ('nsp', 'xci', 'nsz', 'xcz')   # the dumps a game folder may hold
COMPRESSED = ('.nsz', '.xcz')
NCA_HEAD = 0x4000   # an NCZ's first 0x4000 bytes are the NCA's, as they are (its header)
CHUNK = 4 << 20


def compressed(p): return Path(p).suffix.lower() in COMPRESSED


def unpacked_name(p): return Path(p).stem + '.nsp'


def zstd_mod():   # compression.zstd from Python 3.14, its backport before (pip install backports.zstd), the same API
    try: from compression import zstd
    except ImportError:
        try: from backports import zstd
        except ImportError: raise BuildError('NSZ / XCZ dumps need zstd: Python 3.14, or pip install backports.zstd') from None
    return zstd


def aes_mod():
    try: from Crypto.Cipher import AES
    except ImportError:
        try: from Cryptodome.Cipher import AES   # what some system package managers call pycryptodome
        except ImportError: raise BuildError('NSZ / XCZ dumps need AES: pip install pycryptodome (or decompress them to NSP / XCI first: nsz -D)') from None
    return AES


def entries(path):
    """The files of a dump: [(name, offset in the file, size)]. NSP / NSZ = PFS0; XCI / XCZ = the secure partition (an HFS0 inside the root HFS0, which the card header points to)"""
    with open(path, 'rb') as f:
        if f.read(4) == b'PFS0': return _fs(f, 0, 0x18)
        f.seek(0x100)
        if f.read(4) != b'HEAD': raise BuildError(f'{Path(path).name} is neither an NSP (PFS0) nor an XCI (no card header)')
        f.seek(0x130); root = struct.unpack('<Q', f.read(8))[0]
        sec = next((o for n, o, _ in _fs(f, root, 0x40) if n == 'secure'), None)
        if sec is None: raise BuildError(f'no secure partition in {Path(path).name}')
        return _fs(f, sec, 0x40)


def _fs(f, at, esize):   # a PFS0 (entries of 0x18 bytes) or HFS0 (0x40) at offset at: its files with offsets in the file
    f.seek(at); magic, n, st = struct.unpack('<4sII4x', f.read(16))
    if magic not in (b'PFS0', b'HFS0'): raise BuildError(f'no PFS0 / HFS0 at {at:#x} of {f.name}')
    ent, strs = f.read(n * esize), f.read(st)
    data = at + 16 + n * esize + st
    out = []
    for i in range(n):
        off, size, name = struct.unpack_from('<QQI', ent, i * esize)
        e = strs.find(b'\0', name)   # some packers leave out the last name's NUL when it ends the string table
        out.append((strs[name:e if e >= 0 else len(strs)].decode(), data + off, size))
    return out


def _ncz_head(f, at):   # an NCZ's sections [(offset, size, crypto type, key, counter)] and its block header (None: one zstd stream), f left after both
    f.seek(at + NCA_HEAD)
    magic, n = struct.unpack('<8sQ', f.read(16))
    if magic != b'NCZSECTN': raise BuildError(f'not an NCZ (no NCZSECTN) at {at:#x} of {f.name}')
    secs = [(o, s, c, k, ctr) for o, s, c, _, k, ctr in (struct.unpack('<QQQQ16s16s', f.read(0x40)) for _ in range(n))]
    if secs and secs[0][0] > NCA_HEAD: secs.insert(0, (NCA_HEAD, secs[0][0] - NCA_HEAD, 1, b'', b''))   # older compressors start the first section after the header: the gap is plain
    pos = f.tell(); block = None
    if f.read(8) == b'NCZBLOCK':
        ver, typ, _, exp, nb, size = struct.unpack('<BBBBIQ', f.read(16))
        if not 14 <= exp <= 32: raise BuildError(f'a damaged NCZBLOCK header in {f.name} (block size 2^{exp})')
        block = (1 << exp, size, struct.unpack(f'<{nb}I', f.read(4 * nb)))
    else: f.seek(pos)
    return secs, block


def nca_size(f, at):   # the NCA an NCZ at offset at expands to: the header plus the block header's size, else the end of its last section
    secs, block = _ncz_head(f, at)
    return NCA_HEAD + block[1] if block else max([NCA_HEAD] + [o + s for o, s, *_ in secs])


def _pfs0_header(names, sizes):   # the header of a PFS0 holding these files in this order; its string table pads the header to 0x20
    strs = b''.join(n.encode() + b'\0' for n in names)
    hlen = 16 + 0x18 * len(names) + len(strs)
    strs += b'\0' * (-hlen % 0x20)
    ent, off = b'', 0
    for i, s in enumerate(sizes):
        ent += struct.pack('<QQII', off, s, sum(len(n.encode()) + 1 for n in names[:i]), 0); off += s
    return struct.pack('<4sII4x', b'PFS0', len(names), len(strs)) + ent + strs


def plan(path):   # [(name in the NSP, offset, size in the dump, size in the NSP, is an NCZ)]
    with open(path, 'rb') as f:
        return [(n[:-4] + '.nca' if n.lower().endswith('.ncz') else n, o, s, nca_size(f, o) if n.lower().endswith('.ncz') else s, n.lower().endswith('.ncz')) for n, o, s in entries(path)]


def size(path):   # what a dump takes once decompressed (an NSP / XCI: its own size), from the headers alone
    n = Path(path).stat().st_size
    if not n or not compressed(path): return n   # an empty file (a download cut short) holds nothing, and the scan skips it
    p = plan(path)
    return len(_pfs0_header([e[0] for e in p], [e[3] for e in p])) + sum(e[3] for e in p)


class _Stream:   # one zstd stream of an NCZ, read through a view that ends where the NCZ does (else the reader would go on into the next file)
    def __init__(self, f, end, zstd):
        self.f, self.end = f, end
        self.z = zstd.ZstdFile(self)
    def read(self, n=-1):
        left = self.end - self.f.tell()
        return self.f.read(left if n < 0 else min(n, left)) if left > 0 else b''
    def take(self, n): return self.z.read(n)


class _Blocks:   # NCZBLOCK: blocks compressed one by one; a block is stored as it is when compressing it did not make it smaller
    def __init__(self, f, block, zstd):
        self.f, (self.bs, self.size, self.sizes), self.zstd = f, block, zstd
        self.i, self.buf = 0, b''
    def take(self, n):
        while len(self.buf) < n and self.i < len(self.sizes):
            want = self.bs if self.i < len(self.sizes) - 1 else (self.size % self.bs or self.bs)   # the last block holds the rest
            c = self.f.read(self.sizes[self.i]); self.i += 1
            self.buf += self.zstd.decompress(c) if len(c) < want else c
        out, self.buf = self.buf[:n], self.buf[n:]
        return out


def _ncz(f, at, end, out, done):
    """The NCZ at offset at (to end) of f → its NCA, written to out; returns the NCA's SHA-256. Sections of crypto type 3 (CTR) and 4 (BKTR, which nsz splits into CTR parts)
    are encrypted again: AES-128-CTR with the section's key, the counter's first 8 bytes and the NCA offset / 16 as the rest (nsz's AESCTR)"""
    AES, zstd = aes_mod(), zstd_mod()
    f.seek(at); head = f.read(NCA_HEAD)
    h = hashlib.sha256(head); out.write(head); done(len(head))
    secs, block = _ncz_head(f, at)
    src = _Blocks(f, block, zstd) if block else _Stream(f, end, zstd)
    pos = NCA_HEAD
    def copy(n, c=None, strict=True):   # the next n bytes of the stream, encrypted with c when given
        nonlocal pos
        while n > 0:
            d = src.take(min(CHUNK, n))
            if not d:
                if strict: raise BuildError(f'{f.name}: an NCZ ends {n:,} bytes before its section at {pos:#x} does (a damaged or cut-short dump)')
                return
            if c: d = c.encrypt(d)
            out.write(d); h.update(d); done(len(d)); n -= len(d); pos += len(d)
    for o, s, crypto, key, ctr in secs:
        if o < pos == NCA_HEAD: s, o = max(0, o + s - pos), pos   # a first section that starts in the header (0xc00, nsz 5.0): its part there is in the header, as it is
        if o < pos: raise BuildError(f'{f.name}: NCZ sections overlap at {o:#x}')
        copy(o - pos)   # a gap between sections (none seen): plain, as nsz writes it
        if crypto in (3, 4) and o & 15: raise BuildError(f'{f.name}: an NCZ section at {o:#x} is not 16-byte aligned')
        copy(s, AES.new(key, AES.MODE_CTR, nonce=ctr[:8], initial_value=o >> 4) if crypto in (3, 4) else None)
    if block: copy(NCA_HEAD + block[1] - pos, strict=False)   # past the last section, when the block header counts more
    return h.hexdigest()


def decompress(src, dst, done=lambda n: None):
    """A compressed dump → an NSP at dst (written to dst.partial, then renamed), with the source's modification time (stage keys built on names, sizes and times stay the same
    from one run to the next). done(n) is called with each n bytes written"""
    src, dst = Path(src), Path(dst)
    p = plan(src)
    hdr = _pfs0_header([e[0] for e in p], [e[3] for e in p])
    tmp = dst.with_name(dst.name + '.partial')
    # A ticketless NSZ (no .tik: its NCAs were converted from title-key to standard crypto, which rewrites their headers) has NCAs whose SHA-256 is no longer their name,
    # as nsz says (it skips the check for them: Fs/Nsp.py ticketless): Metroid Dread's base game decompressed by nsz itself to the same hash. Cartridge dumps (XCZ) have no tickets
    # and are never converted, so they keep the check
    with open(src, 'rb') as f: ticketless = f.read(4) == b'PFS0' and not any(e[0].lower().endswith('.tik') for e in p)
    try:
        with open(src, 'rb') as f, open(tmp, 'wb') as out:
            out.write(hdr); done(len(hdr))
            for name, off, n, want, ncz in p:
                start = out.tell()
                if ncz:
                    try: sha = _ncz(f, off, off + n, out, done)
                    except zstd_mod().ZstdError as e: raise BuildError(f'{src.name}: {name} does not decompress ({e}): a damaged dump') from None
                    if not ticketless and re.fullmatch(r'[0-9a-f]{32}', name.split('.')[0].lower()) and sha[:32] != name.split('.')[0].lower():
                        raise BuildError(f'{src.name}: {name} decompressed to an NCA whose SHA-256 is not its name ({sha[:32]}): a damaged dump')
                else:
                    f.seek(off)
                    for i in range(0, n, CHUNK):
                        d = f.read(min(CHUNK, n - i)); out.write(d); done(len(d))
                if out.tell() - start != want: raise BuildError(f'{src.name}: {name} came out {out.tell() - start:,} bytes, its header says {want:,}')
    except BaseException:
        tmp.unlink(missing_ok=True); raise
    st = src.stat(); os.utime(tmp, ns=(st.st_atime_ns, st.st_mtime_ns))
    os.replace(tmp, dst)
    return dst
