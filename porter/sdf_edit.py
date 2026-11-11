# Edits inside a Snowdrop game's packed data (SDF slices, camel/sdf/nx/data/sdf-*.sdfdata), as a game's profile lists them (dm/games/<id>/sdf_edits.tsv;
# South Park: The Fractured But Whole drops the Nintendo eShop and Ubisoft Club entries from its main menu). The patched slices go into the pack's romfs_patch
# folder (DM_ROMFS_OVERLAY), beside the Xbox icons (porter/xbox_icons.py); the RomFS stays as the dump has it.
#
# The slices hold each asset as zlib streams (a 1 KB window, header 28 15) of 64 KB of data each, back to back. A changed stream is made again to exactly its
# old length (its deflate data ends in a stored block sized to fit), so nothing else in the slice moves and the slice's table of contents stays right.
# Scripts and tweakables are BVM ("Binary Variant Map"): 0xFF 'BVM', a string table, then one typed value; the reader follows Sir Kane's loader
# (gist.github.com/SirKane/84f4d8bf1392a0a4b720087e414b4143). Signed integers keep their bytes as stored (the game does not always use the shortest form).
import hashlib, shutil, struct, zlib
from pathlib import Path, PurePosixPath, PureWindowsPath

TABLE = 'sdf_edits.tsv'
CHUNK = 65536
TYPES = ['Empty', 'VarMap', 'BoolTrue', 'BoolFalse', 'Int32', 'Int64', 'Float', 'Vec4f', 'Vec4i', 'Vec3f', 'Vec3i', 'Vec2f', 'Vec2i', 'String', 'File', 'Uid', 'UidRef']


class EditError(Exception):
    pass


# ---- BVM ----
class _Reader:
    def __init__(self, b): self.b, self.p = b, 0
    def u8(self): v = self.b[self.p]; self.p += 1; return v
    def var(self):
        v = self.u8(); lead = v & 0xC0
        if lead == 0xC0: v = struct.unpack_from('<I', self.b, self.p)[0]; self.p += 4; return v
        n, v, shift = {0x40: 1, 0x80: 3}.get(lead, 0), v & 0x3F, 6
        for _ in range(n): v |= self.u8() << shift; shift += 8
        return v
    def raw_var(self): a = self.p; self.var(); return self.b[a:self.p]


def _wvar(v):
    if v < 0x40: return bytes([v])
    if v < 1 << 14: return bytes([0x40 | (v & 0x3F), v >> 6])
    if v < 1 << 30: return bytes([0x80 | (v & 0x3F), (v >> 6) & 0xFF, (v >> 14) & 0xFF, (v >> 22) & 0xFF])
    return b'\xC0' + struct.pack('<I', v)


def bvm_load(b):
    """(strings, root value); a value is (type name, payload), a VarMap's payload (int-keyed values, [(key, value)])"""
    if b[:4] != b'\xffBVM': raise EditError('not a BVM asset')
    r = _Reader(b); r.p = 4; strs = []
    for _ in range(r.var()): n = r.var(); strs.append(b[r.p:r.p + n]); r.p += n
    def val():
        t = TYPES[r.u8()]
        if t == 'VarMap': ni, nk = r.var(), r.var(); return t, ([val() for _ in range(ni)], [(val(), val()) for _ in range(nk)])
        if t in ('Empty', 'BoolTrue', 'BoolFalse'): return t, None
        if t == 'Int32': return t, r.raw_var()
        if t in ('String', 'File'): return t, r.var()
        if t.startswith('Vec') and t.endswith('i'): return t, [r.raw_var() for _ in range(int(t[3]))]
        n = {'Int64': 8, 'Float': 4, 'Vec4f': 16, 'Vec3f': 12, 'Vec2f': 8, 'Uid': 16, 'UidRef': 16}[t]; v = b[r.p:r.p + n]; r.p += n; return t, v
    root = val()
    if r.p != len(b): raise EditError(f'BVM asset: {len(b) - r.p} bytes after its value')
    return strs, root


def bvm_dump(strs, root):
    out = [b'\xffBVM', _wvar(len(strs))]
    for s in strs: out += [_wvar(len(s)), s]
    def w(v):
        t, p = v; out.append(bytes([TYPES.index(t)]))
        if t == 'VarMap':
            ints, kv = p; out.append(_wvar(len(ints))); out.append(_wvar(len(kv)))
            for x in ints: w(x)
            for k, x in kv: w(k); w(x)
        elif t in ('String', 'File'): out.append(_wvar(p))
        elif t.startswith('Vec') and t.endswith('i'): out.extend(p)
        elif p is not None: out.append(p)
    w(root)
    return b''.join(out)


def _named(v, strs, name):   # the VarMaps whose 'name' key holds this string
    t, p = v
    if t != 'VarMap': return []
    ints, kv = p; hits = []
    if b'name' in strs and name in strs and any(k == ('String', strs.index(b'name')) and x == ('String', strs.index(name)) for k, x in kv): hits.append(v)
    for x in ints: hits += _named(x, strs, name)
    for _, x in kv: hits += _named(x, strs, name)
    return hits


def bvm_set_list(asset, var, indices, value, expect):
    """asset with the entries at indices of the tweakable var's data list set to the string value; expect: the list as the profile was made from"""
    strs, root = bvm_load(asset)
    hits = _named(root, strs, var.encode())
    if len(hits) != 1: raise EditError(f'{var}: {len(hits)} variables of that name')
    kv = dict(hits[0][1][1]); data = kv.get(('String', strs.index(b'data'))) if b'data' in strs else None
    if not data or data[0] != 'VarMap': raise EditError(f'{var}: no data list')
    ints = data[1][0]
    now = [strs[x[1]].decode() if x[0] == 'String' else '?' for x in ints]
    if now != expect: raise EditError(f'{var} is {now}, not the {expect} the profile was made from (another version of the game?)')
    if value.encode() not in strs: raise EditError(f'{var}: no string {value!r} in the asset')
    for i in indices: ints[i] = ('String', strs.index(value.encode()))
    return bvm_dump(strs, root)


# ---- the slices' zlib streams ----
def streams(d, at):
    """[(start, end, data)] of the asset whose first stream starts at at"""
    out = []
    while True:
        if d[at:at + 2] != b'\x28\x15': raise EditError(f'no zlib stream at {at}')
        o = zlib.decompressobj(10)
        try: data = o.decompress(d[at:at + 2 * CHUNK + 1024])
        except zlib.error as e: raise EditError(f'stream at {at}: {e}')
        if not o.eof: raise EditError(f'stream at {at} does not end')
        end = at + min(2 * CHUNK + 1024, len(d) - at) - len(o.unused_data)
        out.append((at, end, data))
        if len(data) < CHUNK: return out
        at = end


def fit(data, n):
    """a zlib stream of exactly n bytes holding data (EditError when it cannot be made that short)"""
    for level in (9, 6, 1):
        for m in range(min(len(data), 0xFFFF) + 1):
            c = zlib.compressobj(level, zlib.DEFLATED, -10)
            body = c.compress(data[:len(data) - m]) + c.flush(zlib.Z_FULL_FLUSH) if m < len(data) else b''   # (all of it stored: no deflate part)
            total = 2 + len(body) + 5 + m + 4
            if total == n:
                z = b'\x28\x15' + body + b'\x01' + struct.pack('<HH', m, m ^ 0xFFFF) + data[len(data) - m:] + struct.pack('>I', zlib.adler32(data))
                o = zlib.decompressobj(10)
                if o.decompress(z) != data or not o.eof or o.unused_data: raise EditError('a re-made stream does not read back')
                return z
            if total > n + 4096: break
    raise EditError(f'{len(data)} bytes do not fit in a {n}-byte stream')


def replace_asset(d, at, new):
    """d with the asset at at holding new (the same length); the number of streams made again"""
    ss = streams(d, at)
    if len(new) != sum(len(x) for _, _, x in ss): raise EditError('an edit changed the asset\'s length')
    d = bytearray(d); n = 0
    for i, (a, e, old) in enumerate(ss):
        part = new[i * CHUNK:i * CHUNK + len(old)]
        if part != old: d[a:e] = fit(part, e - a); n += 1
    return bytes(d), n


# ---- the profile table ----
def read_table(path):
    """[(slice, asset offset, asset md5, op, args)]: one edit per line, tab-separated, '#' comments"""
    rows = []
    for ln in Path(path).read_text(encoding='utf-8').splitlines():
        ln = ln.split('#', 1)[0].strip()
        if not ln: continue
        f = ln.split('\t')
        rows.append((f[0], int(f[1]), f[2], f[3], f[4:]))
    return rows


def patch(romfs, table, out):
    """apply the table's edits; a slice already under out (the Xbox icons) is edited there. → the number of edits"""
    rows = read_table(table); by = {}
    for r in rows: by.setdefault(r[0], []).append(r)
    for f, rs in by.items():
        if PurePosixPath(f).is_absolute() or PureWindowsPath(f).is_absolute() or PureWindowsPath(f).drive or '..' in PureWindowsPath(f).parts:
            raise EditError(f'{f}: a slice path must stay inside the RomFS (no absolute path, no ..)')
        dst = Path(out) / f; src = dst if dst.is_file() else Path(romfs) / f
        if not src.is_file(): raise EditError(f'{f} is not in the RomFS')
        d = src.read_bytes()
        for _, at, md5, op, args in rs:
            asset = b''.join(x for _, _, x in streams(d, at))
            if hashlib.md5(asset).hexdigest() != md5: raise EditError(f'{f} @{at}: not the asset the profile was made from (another version of the game?)')
            if op == 'bvm-list':   # var, indices (1,2), new string, the list as it was (a,b,c)
                var, idx, value, was = args[:4]
                new = bvm_set_list(asset, var, [int(i) for i in idx.split(',')], value, was.split(','))
            else: raise EditError(f'unknown edit {op!r}')
            d, _ = replace_asset(d, at, new)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(d)
    return len(rows)
