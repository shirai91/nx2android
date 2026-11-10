# The Xbox button layout (config pad_layout xbox): a game that draws its button icons from its own files rather than the system's button font gets its own
# Xbox icons where its profile lists them (dm/games/<id>/xbox_icons.tsv, South Park: The Fractured But Whole). Each row names a place in a RomFS file that holds
# one of the game's Switch icons and the place of the Xbox icon that replaces it; the Xbox PNG is re-encoded to exactly the Switch one's length (a tEXt chunk
# pads it), so the archive's own table of contents stays right. The patched files go into the pack's romfs_patch folder, which the host reads before the
# RomFS (DM_ROMFS_OVERLAY, dm/host/hle_fs.c); the RomFS itself is left as the dump has it.
import hashlib, io, shutil, struct, zlib
from pathlib import Path

TABLE = 'xbox_icons.tsv'
FOLDER = 'romfs_patch'


class IconError(Exception):
    pass


def read_table(path):   # [(RomFS path, offset, length, md5, source offset, source length, source md5)]
    rows = []
    for ln in Path(path).read_text(encoding='utf-8').splitlines():
        ln = ln.split('#', 1)[0].strip()
        if not ln: continue
        f, o, n, m, so, sn, sm = ln.split('\t')[:7]
        rows.append((f, int(o), int(n), m, int(so), int(sn), sm))
    return rows


def png_chunk(kind, data): return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def fit_png(src, length):
    """src (a PNG) as RGBA, re-encoded and padded to exactly length bytes; IconError when it does not fit. Kept truecolor: a palette would be smaller, but the
    game's decoder has only ever seen its own RGBA icons"""
    from PIL import Image
    b = io.BytesIO(); Image.open(io.BytesIO(src)).convert('RGBA').save(b, 'PNG', optimize=True); png = b.getvalue()
    pad = length - len(png)
    if pad < 0: raise IconError(f'{len(png)} bytes re-encoded, {-pad} more than its place')
    if pad == 0: return png
    if pad < 14: raise IconError(f'{pad} bytes to spare, less than the smallest padding chunk')
    iend = png.rindex(b'IEND') - 4
    return png[:iend] + png_chunk(b'tEXt', b'p\0' + b' ' * (pad - 14)) + png[iend:]


def patch(romfs, table, out):
    """The patched copies of the RomFS files the table names, under out (the same paths). → the number of icons replaced"""
    rows = read_table(table)
    if out.exists(): shutil.rmtree(out)
    by_file = {}
    for r in rows: by_file.setdefault(r[0], []).append(r)
    for f, rs in by_file.items():
        src = Path(romfs) / f
        if not src.is_file(): raise IconError(f'{f} is not in the RomFS')
        d = bytearray(src.read_bytes())
        for _, o, n, m, so, sn, sm in rs:
            if hashlib.md5(d[o:o + n]).hexdigest() != m or hashlib.md5(d[so:so + sn]).hexdigest() != sm:
                raise IconError(f'{f}: the bytes at {o} or {so} are not the ones the profile was made from (another version of the game?)')
        for _, o, n, _, so, sn, _ in rs:
            try: d[o:o + n] = fit_png(bytes(d[so:so + sn]), n)
            except IconError as e: raise IconError(f'{f} @{o}: {e}')
        dst = out / f; dst.parent.mkdir(parents=True, exist_ok=True); dst.write_bytes(d)
    return len(rows)
