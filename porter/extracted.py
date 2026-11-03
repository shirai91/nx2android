# Game files already extracted, as a console dump tool writes them (no NSP, no keys needed):
#   base/exefs/ (main, main.npdm, sdk, ...), base/romfs/
#   updates/<version>/exefs/, updates/<version>/romfs/   (<version> the title version as a decimal number; its RomFS is the patched tree the game sees, not a patch)
#   dlc/<DLC title ID>/<n>/romfs/
# placed into the build folder's extraction layout (extract/v<N>/{ExeFS,RomFS}, aoc/<index>/) as hard links, so they take no space.
import hashlib, os, shutil, struct
from pathlib import Path

from porter.upstream import BuildError


def program_id(npdm):
    """The ACI0 program ID of a main.npdm"""
    d = Path(npdm).read_bytes()
    off, size = struct.unpack_from('<II', d, 0x70) if d[:4] == b'META' else (0, 0)
    if d[off:off + 4] != b'ACI0' or size < 0x18: raise BuildError(f'{npdm}: not an NPDM with an ACI0 section')
    return struct.unpack_from('<Q', d, off + 0x10)[0]


def scan(folder):
    """{'app': title ID, 'versions': {'v<N>': folder with exefs/ and romfs/}, 'dlc': {DLC title ID: romfs folder}}, or None when folder is not an extracted dump"""
    folder = Path(folder)
    base = folder / 'base'
    if not (base / 'exefs/main.npdm').is_file() or not (base / 'romfs').is_dir(): return None
    app = program_id(base / 'exefs/main.npdm')
    versions = {'v0': base}
    for p in sorted((folder / 'updates').glob('*')) if (folder / 'updates').is_dir() else []:
        if p.name.isdigit() and (p / 'exefs/main').is_file():
            if program_id(p / 'exefs/main.npdm') != app: raise BuildError(f'{p}: an update of another title than {app:016X}')
            versions[f'v{int(p.name)}'] = p
    dlc = {}
    for p in sorted((folder / 'dlc').glob('*')) if (folder / 'dlc').is_dir() else []:
        roms = sorted(p.glob('*/romfs')) or ([p / 'romfs'] if (p / 'romfs').is_dir() else [])
        try: tid = int(p.name, 16)
        except ValueError: continue
        if len(roms) == 1 and 0 < tid - app - 0x1000 <= 2000: dlc[tid] = roms[0]
    return {'app': app, 'versions': versions, 'dlc': dlc}


def link_tree(src, dst):
    """dst = src as hard links (copies across volumes), through dst.partial; kept when src's names, sizes and times are as when it was made"""
    files = sorted((p.relative_to(src).as_posix(), p.stat().st_size, p.stat().st_mtime_ns) for p in Path(src).rglob('*') if p.is_file() and not p.name.startswith('.'))
    key = hashlib.sha256(repr(files).encode()).hexdigest()
    mark = dst.with_name(dst.name + '.key')
    if dst.is_dir() and mark.is_file() and mark.read_text() == key: return
    tmp = dst.with_name(dst.name + '.partial')
    for d in (tmp, dst):
        if d.exists(): shutil.rmtree(d)
    for rel, _, _ in files:
        s, t = Path(src) / rel, tmp / rel
        t.parent.mkdir(parents=True, exist_ok=True)
        try: os.link(s, t)
        except OSError: shutil.copy2(s, t)
    tmp.mkdir(parents=True, exist_ok=True); tmp.rename(dst); mark.write_text(key)


def place(dump, ver, extract):
    """Version ver of the dump as extract/<ver>/{ExeFS,RomFS} → that folder"""
    d = extract / ver
    for sub, name in (('exefs', 'ExeFS'), ('romfs', 'RomFS')): link_tree(dump['versions'][ver] / sub, d / name)
    return d


def place_aoc(dump, work):
    """The dump's DLC as <build folder>/aoc/<index>/ (index = title ID − base − 0x1000, the host's DM_AOC_DIR) → that folder, None without DLC"""
    if not dump['dlc']: return None
    for tid, romfs in dump['dlc'].items(): link_tree(romfs, work / 'aoc' / str(tid - dump['app'] - 0x1000))
    return work / 'aoc'
