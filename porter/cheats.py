# Atmosphère cheat files on the device (python -m porter cheats …): the engine reads <build ID>.txt from the app's files folder's cheats/ (hle_cheats.c), and the
# pause menu's Cheats picker lists and toggles its entries. This module puts a file there over adb, names it after the build the installed port runs (the
# BUILD_ID line of mods/main.fixups.tsv), and lists what is there. Cheat files are the player's: nothing here fetches or ships one.
import re, shlex
from pathlib import Path

from porter import datapack
from porter.upstream import BuildError, say

BUILD = re.compile(r'^[0-9A-Fa-f]{16}$')   # the first 16 hex digits of main's NSO build ID, as cheat sites name their files


def folder(package): return f'{datapack.files_folder(package)}/cheats'


def build_id(adb, package):
    """The build the installed port's main module runs, upper case; the host picks <build ID>.txt by it"""
    text = adb.read_text(f'{datapack.files_folder(package)}/mods/main.fixups.tsv')
    for line in text.splitlines():
        if line.startswith('# BUILD_ID'):
            v = line.split('\t')[-1].strip().upper()   # patch_nso.py writes the full 32-hex NSO ID; the host and the cheat files use its first 16
            if len(v) >= 16 and BUILD.fullmatch(v[:16]): return v[:16]
    raise BuildError(f'{package}: no build ID in mods/main.fixups.tsv: port the game again (older ports do not name the build the host runs)')


def listing(adb, package):
    """The cheat files in the app's cheats folder: {package, build, folder, files: [{name, build, bytes, current, enabled}]}"""
    build, root = build_id(adb, package), folder(package)
    have = adb.remote_sizes(root)
    out = []
    for rel, size in sorted(have.items()):
        if '/' in rel or not rel.lower().endswith('.txt'): continue
        b = Path(rel).stem.upper()
        if not BUILD.fullmatch(b): continue
        en = f'{b}.enabled'
        on = len([ln for ln in adb.read_text(f'{root}/{en}').splitlines() if ln.strip()]) if en in have else 0
        out.append({'name': rel, 'build': b, 'bytes': size, 'current': b == build, 'enabled': on})
    return {'package': package, 'build': build, 'folder': root, 'files': out}


def install(adb, package, src, build='', any_build=False):
    """Push src as cheats/<build ID>.txt for the installed port (or --build / a 16-hex file name); an old file of that name is kept as .txt.bak.
    Returns {package, build, folder, file, bytes, backup}; the picker reads it when it opens (dm_cheats_reload), so the game need not restart"""
    src = Path(src)
    if not src.is_file(): raise BuildError(f'{src}: not a file')
    if src.suffix.lower() == '.toml':
        raise BuildError(f'{src}: an anchored .cheats.toml needs scripts/cheat_anchor.py to compile it for a build; this checkout does not carry that tool yet')
    if src.suffix.lower() != '.txt': raise BuildError(f'{src}: a cheat file is a .txt (Atmosphère format)')
    have = build_id(adb, package)
    if build:
        if not BUILD.fullmatch(build): raise BuildError(f'--build {build!r}: not 16 hexadecimal digits')
        target_build = build.upper()
    elif BUILD.fullmatch(src.stem):
        target_build = src.stem.upper()
    else:
        target_build = have
    if target_build != have and not any_build:
        raise BuildError(f'{src.name} is for build {target_build}, but {package} runs {have}: rename it, or pass --any-build to put it there anyway '
                         '(the host refuses a file of another build unless DM_CHEATS_ANY_BUILD=1)')
    root = folder(package)
    adb.shell(f'mkdir -p {shlex.quote(root)}')
    name, target, backup = f'{target_build}.txt', f'{root}/{target_build}.txt', ''
    if name in adb.remote_sizes(root):
        backup = f'{target}.bak'
        adb.shell(f'mv -f {shlex.quote(target)} {shlex.quote(backup)}')
    adb.push_file(src, target)
    got = adb.remote_sizes(root).get(name)
    if got != src.stat().st_size: raise BuildError(f'{name}: {got if got is not None else "no"} bytes on the device, {src.stat().st_size} expected')
    say(f'{package}: {name} ({src.stat().st_size:,} bytes) in {root}' + (f'; the old file is {name}.bak' if backup else ''))
    return {'package': package, 'build': target_build, 'folder': root, 'file': name, 'bytes': src.stat().st_size, 'backup': backup}