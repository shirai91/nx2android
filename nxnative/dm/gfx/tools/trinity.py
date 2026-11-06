# Game Freak's Trinity engine file system (Pokémon Legends: Z-A): arc/data.trpfs (packs of files) with arc/data.trpfd as its index, both FlatBuffers. Read by the shader split
# (shader_split.py trpfs_jobs, trinity_shaders) and by the porter, which has the packs' Oodle files decoded by the game's own decoder on the Android device (porter/pipeline.py device_oodle)
import mmap, os, struct

ONEPACK = b'ONEPACK\0'

def fb_field(d, t, i):
    """FlatBuffers: absolute offset of field i of the table at t, None when the field is absent"""
    vt = t - struct.unpack_from('<i', d, t)[0]
    if 4 + 2 * i >= struct.unpack_from('<H', d, vt)[0]: return None
    off = struct.unpack_from('<H', d, vt + 4 + 2 * i)[0]
    return t + off if off else None

def fb_vec(d, t, i):
    """FlatBuffers: (start, count) of the vector in field i"""
    p = fb_field(d, t, i)
    if p is None: return 0, 0
    p += struct.unpack_from('<I', d, p)[0]
    return p + 4, struct.unpack_from('<I', d, p)[0]

def fb_refs(d, t, i):   # the tables / strings a vector of offsets points at
    st, n = fb_vec(d, t, i)
    return [st + 4 * k + struct.unpack_from('<I', d, st + 4 * k)[0] for k in range(n)]

def fb_root(d, at=0): return at + struct.unpack_from('<I', d, at)[0]

def packs(p):
    """The packs of a Trinity file system p (a .trpfs with its .trpfd beside it). The descriptor's root table: 0 file hashes [u64], 1 pack names [string], 2 file infos [{0 pack index u64}]
    (in file hash order), 3 pack infos [{0 pack size u64, 1 file count u64}]. The file system: 'ONEPACK\\0', u64 offset of its root table {0 pack name hashes [u64], 1 pack offsets [u64]}, each pack
    a table of its own at its offset. A pack is named through its first file's hash (its own name hash is not a plain FNV-1a of the descriptor's name). → [(pack offset, name, size)] (name None
    for a pack whose files the descriptor does not list), or None when p is no Trinity file system"""
    if not p.endswith('.trpfs') or not os.path.exists(p[:-1] + 'd'): return None
    with open(p, 'rb') as f:
        if f.read(8) != ONEPACK: return None
    fd = open(p[:-1] + 'd', 'rb').read(); fr = fb_root(fd)
    hashes = struct.unpack_from(f'<{fb_vec(fd, fr, 0)[1]}Q', fd, fb_vec(fd, fr, 0)[0])
    names = [fd[s + 4:s + 4 + struct.unpack_from('<I', fd, s)[0]].decode() for s in fb_refs(fd, fr, 1)]
    pack_of = {h: struct.unpack_from('<Q', fd, fb_field(fd, t, 0))[0] if fb_field(fd, t, 0) is not None else 0 for h, t in zip(hashes, fb_refs(fd, fr, 2))}
    sizes = [struct.unpack_from('<Q', fd, fb_field(fd, t, 0))[0] for t in fb_refs(fd, fr, 3)]
    out = []
    with open(p, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
        sr = fb_root(d, struct.unpack_from('<Q', d, 8)[0]); st, n = fb_vec(d, sr, 1)
        for off in struct.unpack_from(f'<{n}Q', d, st):
            pr = fb_root(d, off); h0 = fb_vec(d, pr, 0)
            i = pack_of.get(struct.unpack_from('<Q', d, h0[0])[0]) if h0[1] else None
            out.append((off, None, 0) if i is None else (off, names[i], sizes[i]))
    return out

def pack_files(d, off):
    """The files of the pack at off. Pack table: 0 file hashes [u64], 1 files [{1 compression i8 (3 = Oodle; stored otherwise), 2 level u8, 3 length u64, 4 data [u8]}].
    → (index, file hash, compression, length, data start, stored length) of each file"""
    pr = fb_root(d, off); hs = fb_vec(d, pr, 0)
    for k, t in enumerate(fb_refs(d, pr, 1)):
        c, ln = fb_field(d, t, 1), fb_field(d, t, 3)
        st, sz = fb_vec(d, t, 4)
        yield (k, struct.unpack_from('<Q', d, hs[0] + 8 * k)[0], struct.unpack_from('<b', d, c)[0] if c is not None else 0,
               struct.unpack_from('<Q', d, ln)[0] if ln is not None else 0, st, sz)
