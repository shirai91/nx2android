# Compressed RomFS (a profile's android.tsv romfs zstd): the game's files take less room on the device. A switch game's NCAs keep their sections compressed, so the extracted RomFS
# is larger than the NSPs it came from: Cardfight!! Vanguard Dear Days 2's base XCI and update are 21 GB, its merged RomFS 30.6 GB, and its UI and card textures (.tpc)
# compress to about 80 % with zstd. Each file that shrinks enough is stored as a packed file of the same name, which the Android host reads back (dm/host/hle_fs.c, zf_*):
#   header, 32 bytes, little-endian: 'NXZR', u32 version 1, u64 raw size, u32 block size, u32 block count n, u64 0
#   u64 offsets[n + 1]: where each block's bytes start in the file, and the end
#   the blocks: block i is offsets[i] .. offsets[i + 1]; as long as the block's raw bytes (the last block may be shorter) it is stored as it is, else it is one zstd frame
# Blocks of 256 KB keep random reads cheap (the game reads parts of large archives: at most one block more is decompressed at each end). The other files stay hard links to the
# extraction. The packed tree lives in <build folder>/android/romfs_zstd/<version>/ (its state in <version>.json beside it), made once and reused (a file is packed again only when its source's size or time changes).
import concurrent.futures as cf, io, json, os, shutil, struct
from pathlib import Path

MAGIC, VERSION, BLOCK = b'NXZR', 1, 1 << 18
HDR = struct.Struct('<4sIQIIQ')
LEVEL = 3              # zstd's default level; the host's decoder costs the same at every level
KEEP = 0.92            # a file is packed only when it comes out at most this fraction of its size: the rest is not worth a decoder pass on every read
MIN_SIZE = 64 << 10    # small files are left as they are (tables, scripts: little to gain, one more read each)


def zstd_mod():   # compression.zstd from Python 3.14, its backport before (pip install backports.zstd)
    try: from compression import zstd
    except ImportError:
        try: from backports import zstd
        except ImportError: return None
    return zstd


def pack_stream(src, size, out, zstd, level=LEVEL, block=BLOCK):
    """The packed file of the size bytes readable from src, written to out (a seekable binary file, at 0): one block in memory at a time, the header and the block table
    written last into the room left for them. True when it shrank to KEEP; False leaves out partly written (the caller drops it). A RomFS can hold multi-GB archives
    (Unreal's .pak, CRI's .cpk), and whole files read by every packing thread at once needed tens of GB of memory"""
    n = (size + block - 1) // block
    head = HDR.size + 8 * (n + 1); limit = size * KEEP
    out.seek(head); offs = [head]
    for _ in range(n):
        raw = src.read(block)
        if not raw: raise OSError('the file is shorter than its size')
        z = zstd.compress(raw, level)
        out.write(z if len(z) < len(raw) else raw); offs.append(offs[-1] + min(len(z), len(raw)))
        if offs[-1] > limit: return False   # will not shrink enough: stop reading
    out.seek(0); out.write(HDR.pack(MAGIC, VERSION, size, block, n, 0) + struct.pack(f'<{n + 1}Q', *offs))
    return True


def pack_bytes(data, zstd, level=LEVEL, block=BLOCK):
    """data → the packed file's bytes, or None when it does not shrink to KEEP (tests; pack_tree streams the files)"""
    out = io.BytesIO()
    return out.getvalue() if pack_stream(io.BytesIO(data), len(data), out, zstd, level, block) else None


def unpack_bytes(packed, zstd):
    """A packed file's bytes → the original bytes (tests; the host reads them block by block)"""
    magic, ver, raw, block, n, _ = HDR.unpack_from(packed)
    if magic != MAGIC or ver != VERSION: raise ValueError('not a packed RomFS file')
    offs = struct.unpack_from(f'<{n + 1}Q', packed, HDR.size)
    out = bytearray()
    for i in range(n):
        b = packed[offs[i]:offs[i + 1]]; want = min(block, raw - i * block)
        out += b if len(b) == want else zstd.decompress(b)
    if len(out) != raw: raise ValueError('packed RomFS file: wrong size')
    return bytes(out)


def is_packed(path):
    with open(path, 'rb') as f: return f.read(4) == MAGIC


def pack_tree(src, dst, jobs=8, progress=None):
    """src: an extracted RomFS; dst: the packed tree (made or brought up to date). Every file of src is in dst: packed, or a hard link (a copy across volumes) of the source.
    progress(done bytes, total bytes). Returns (files, packed files, source bytes, bytes in dst)"""
    zstd = zstd_mod()
    if not zstd: raise RuntimeError(f'Python {os.sys.version_info[0]}.{os.sys.version_info[1]} has no compression.zstd (3.14+): pip install backports.zstd')
    src, dst = Path(src), Path(dst); dst.mkdir(parents=True, exist_ok=True)
    sf = dst.with_name(dst.name + '.json')   # beside the tree, which becomes the pack's RomFS as it is
    try: state = json.loads(sf.read_text(encoding='utf-8'))
    except (OSError, ValueError): state = {}
    files = [p for p in src.rglob('*') if p.is_file()]
    total = sum(p.stat().st_size for p in files); done = 0; new = {}

    def one(p):
        rel = p.relative_to(src).as_posix(); st = p.stat(); key = [st.st_size, st.st_mtime_ns]; out = dst / rel
        old = state.get(rel)
        if old and old['src'] == key and out.is_file() and out.stat().st_size == old['size']: return rel, old, st.st_size
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.exists(): out.unlink()
        packed = False
        if st.st_size >= MIN_SIZE:
            tmp = out.with_name(out.name + '.partial')
            with open(p, 'rb') as f, open(tmp, 'wb') as o: packed = pack_stream(f, st.st_size, o, zstd)
            if packed: tmp.replace(out)
            else: tmp.unlink()
        if not packed:
            try: os.link(p, out)
            except OSError: shutil.copyfile(p, out)
        return rel, {'src': key, 'size': out.stat().st_size, 'packed': packed}, st.st_size

    with cf.ThreadPoolExecutor(max(1, jobs)) as ex:   # zstd lets go of the GIL
        for rel, ent, n in ex.map(one, files):
            new[rel] = ent; done += n
            if progress: progress(done, total)
    for p in dst.rglob('*'):   # files of an earlier version that this RomFS lacks
        if p.is_file() and p.relative_to(dst).as_posix() not in new: p.unlink()
    sf.write_text(json.dumps(new), encoding='utf-8')
    return len(new), sum(e['packed'] for e in new.values()), total, sum(e['size'] for e in new.values())
