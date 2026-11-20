"""EA BIG archives, EASF envelopes and chunkzip streams.

FIFA 21's shader archives are BIG -> EASF -> chunkzip v2 -> BIG -> NXSHADER.
The EASF key is taken from the executable's key-ID table, never shipped here.
Format references: FIFA-Mods/EASF (header/CBC), FIFALibrary22/FifaFile.vb (chunkzip).
"""
import hashlib, io, re, struct, zlib
import uepak

MAGICS = (b'BIG4', b'BIGF', b'EASF', b'chunkzip', b'NXSHADER', bytes.fromhex('91688619'))
JOB = 'EA_BIG'

def members(f):
    """Read the directory only; member bounds use the real archive length."""
    f.seek(0, 2); end = f.tell(); f.seek(0); h = f.read(16)
    if h[:4] not in (b'BIG4', b'BIGF'): return None
    if len(h) != 16: raise ValueError('truncated BIG header')
    count, size = struct.unpack_from('>II', h, 8)
    if not 16 <= size <= end or count > (size - 16) // 9: raise ValueError('invalid BIG directory bounds')
    table = f.read(size - 16); pos = 0; out = []
    for _ in range(count):
        if pos + 9 > len(table): raise ValueError('truncated BIG directory entry')
        off, n = struct.unpack_from('>II', table, pos); stop = table.find(b'\0', pos + 8)
        if stop < 0: raise ValueError('unterminated BIG member name')
        name = table[pos + 8:stop].decode('utf-8', errors='replace'); pos = stop + 1
        if off < size or off > end or n > end - off: raise ValueError(f'BIG member outside archive: {name}')
        out.append((name, off, n))
    return out

def cbc(key, data):
    """AES-128 CBC over the shared platform AES implementation (no padding removal)."""
    if len(data) % 16: raise ValueError('unaligned EASF ciphertext')
    data = bytes(data)   # CommonCrypto's ctypes binding needs bytes, including for slices of bytearrays
    raw = uepak.aes_decrypt(key, data); prev = key + data[:-16]
    return bytes(a ^ b for a, b in zip(raw, prev))

def find_key(img, head):
    """EASF key-ID records are eight bytes of ID followed by a 16-byte key.
    A SHA-256 of the first 32 plaintext bytes validates candidates, including
    unaligned records (the executable's table is byte-packed).
    """
    if len(head) < 80 or head[:4] != b'EASF': raise ValueError('truncated EASF envelope')
    for m in re.finditer(re.escape(head[8:16]), img):
        key = img[m.end():m.end() + 16]
        if len(key) == 16 and hashlib.sha256(cbc(key, head[48:80])).digest() == head[16:48]: return key
    raise ValueError('EASF key-ID table entry not found in executable')

def decrypt(d, keys):
    if len(d) < 80: raise ValueError('truncated EASF envelope')
    key = keys.get(bytes(d[8:16]))
    if key is None: raise ValueError('no executable key for EASF key ID')
    n = struct.unpack_from('>I', d, 4)[0]; out = cbc(key, d[48:])
    if not 32 <= n <= len(out) or hashlib.sha256(out[:32]).digest() != d[16:48]: raise ValueError('EASF size or plaintext digest mismatch')
    return out[:n]

def chunkzip(d):
    if len(d) < 16: raise ValueError('truncated chunkzip header')
    v2 = struct.unpack_from('>I', d, 8)[0] == 2
    if v2 and len(d) < 40: raise ValueError('truncated chunkzip v2 header')
    total, block = struct.unpack_from('>II', d, 12 if v2 else 8)
    if not block or not total: raise ValueError('invalid chunkzip lengths')
    count = (total + block - 1) // block; align = 1
    if v2:
        nc, align = struct.unpack_from('>II', d, 20)
        if nc != count or not align or align & (align - 1): raise ValueError('invalid chunkzip v2 block table')
    pos = 40 if v2 else 16; out = []
    for i in range(count):
        hs = 8 if v2 else 4
        if pos + hs > len(d): raise ValueError('truncated chunkzip block header')
        n = struct.unpack_from('>I', d, pos)[0]
        if v2 and struct.unpack_from('>I', d, pos + 4)[0] != 1: raise ValueError('unsupported chunkzip v2 block method')
        pos += hs
        if n > len(d) - pos: raise ValueError('truncated chunkzip block')
        expected = min(block, total - i * block); z = zlib.decompressobj(-15 if v2 else 15)
        raw = z.decompress(d[pos:pos + n], expected + 1)
        if len(raw) != expected or not z.eof or z.unused_data or z.unconsumed_tail: raise ValueError('chunkzip block length or stream mismatch')
        out.append(raw); pos += n
        if v2 and i + 1 < count: pos = ((pos + 8 + align - 1) & -align) - 8
    if pos != len(d): raise ValueError('trailing chunkzip data')
    return b''.join(out)

def leaves(d, label, keys, depth=0):
    if depth > 16: raise ValueError('EA archive nesting exceeds 16 levels')
    if d[:4] == b'EASF': yield from leaves(decrypt(d, keys), label, keys, depth + 1)
    elif d[:8] == b'chunkzip': yield from leaves(chunkzip(d), label, keys, depth + 1)
    elif d[:4] in (b'BIG4', b'BIGF'):
        for name, off, n in members(io.BytesIO(d)): yield from leaves(d[off:off + n], f'{label}/{name}', keys, depth + 1)
    else: yield label, d

def jobs(files, img):
    """Select shader/archive members of .big files without reading unrelated RomFS files."""
    out, archives = [], set()
    for path, rel in files:
        if not path.lower().endswith('.big'): continue
        with open(path, 'rb') as f:
            mm = members(f)
            if mm is None: continue
            archives.add(path); selected, keys = [], {}
            for name, off, n in mm:
                f.seek(off); h = f.read(min(n, 80))
                if not h.startswith(MAGICS): continue
                if h[:4] == b'EASF' and h[8:16] not in keys: keys[h[8:16]] = find_key(img, h)
                selected.append((name, off, n))
            if selected: out.append((path, rel, (JOB, selected, keys)))
    return out, archives
