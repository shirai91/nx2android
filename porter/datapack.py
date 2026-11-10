# The data pack: everything the app reads from its files folder on the device (/sdcard/Android/data/<package>/files, front_android.c), as a folder beside the APK:
#   mods/          the rewritten code modules (patch_nso.py's outputs)
#   RomFS/         the game's files (the packed version's merged RomFS)
#   romfs_patch/   patched copies of a few RomFS files, read before the RomFS (DM_ROMFS_OVERLAY; the Xbox button icons, porter/xbox_icons.py)
#   aoc/<index>/   the DLC the player has
#   sysdata/<title ID>/  system data from the player's firmware dump that the game reads (android.tsv sysdata, porter --firmware; env.txt's DM_SYSDATA)
#   shaders.dmsv   the Vulkan shaders (dm/android/tools/shader_vk.py)
#   nintendo_ext.ttf  the button-icon font, the stand-in for the Nintendo extension shared font (dm/tools/make_ext_font.py; env.txt's DM_SHARED_FONT_EXT)
#   standard.ttf   the player's font for the system's shared fonts, with --shared-font (env.txt's DM_SHARED_FONT)
#   env.txt        host switches: the game language, the DLC folder, the profile's env rows, then the port's (--fps60, --mt-replay); on the device the installer adds its
#                  DM_ROMFS row after them when the app reads its RomFS from another folder (porter/install.py, --romfs-at): that is a fact of one device, not of the pack
#   settings.txt   the pause menu's first-launch defaults (the profile's setting rows, then the port's: --touch); the installer never replaces the player's own
#   nx2a-data.json what the pack is: the game, the package, the version, the device features the profile requires (android.tsv requires), which the installer checks
# Big trees are hard links into the extraction when the volume allows, so the pack takes no extra space; the pack is rebuilt whole when anything in it changes.
import json, os, shutil, time
from pathlib import Path

MANIFEST = 'nx2a-data.json'
KEEP_ON_DEVICE = ('settings.txt',)          # the player's choices win over the pack's defaults
OWNED = ('mods', 'RomFS', 'romfs_patch', 'aoc', 'sysdata')            # folders the pack owns on the device: files there that the pack lacks are stale (an older version's) and removed
ROMFS = 'RomFS'


def link_or_copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    try: os.link(src, dst)
    except OSError: shutil.copy2(src, dst)


def link_tree(src, dst):   # (files, bytes)
    n = b = 0
    for root, _, files in os.walk(src):
        for f in files:
            s = Path(root) / f
            link_or_copy(s, dst / s.relative_to(src)); n += 1; b += s.stat().st_size
    return n, b


def files_folder(package): return f'/sdcard/Android/data/{package}/files'


def romfs_sizes(d):   # {path in the RomFS: size} of the pack in d, '/'-separated as the device lists them (install --romfs-at compares a folder on the device with it)
    r = Path(d) / ROMFS
    return {p.relative_to(r).as_posix(): p.stat().st_size for p in r.rglob('*') if p.is_file()} if r.is_dir() else {}


def build(dst, *, profile, version, display, patched, romfs, shaders, romfs_patch=None, aoc=None, sysdata=None, language='en-US', package=None, env=(), settings=None, files=None, progress=None):
    """Write the pack to dst (replacing it). profile: the AndroidProfile; package: the app's, when not the profile's; env: more env.txt rows, after the profile's; settings: settings.txt values over the profile's;
    files: {name in the pack: source} (the 60 fps memory writes, fpslock.tsv; the button-icon font). Returns the manifest"""
    package = package or profile.package
    tmp = dst.with_name(dst.name + '.partial')
    if tmp.exists(): shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    n = b = 0
    for what, src, sub in (('the rewritten modules', patched, 'mods'), ('the RomFS', romfs, 'RomFS'), ('the patched RomFS files', romfs_patch, 'romfs_patch'), ('the DLC', aoc, 'aoc'),
                           ('the system data', sysdata, 'sysdata')):
        if not src: continue
        if progress: progress(what)
        dn, db = link_tree(src, tmp / sub); n += dn; b += db
    if shaders: link_or_copy(shaders, tmp / 'shaders.dmsv'); n += 1; b += shaders.stat().st_size   # none for an OpenGL ES game
    for name, src in (files or {}).items(): shutil.copyfile(src, tmp / name); n += 1; b += Path(src).stat().st_size
    rows = [f'# Written by nx2android\'s porter for {profile.name} ({profile.id}); KEY=VALUE per line, read by the app at launch',
            f'DM_LANG={language}']
    if aoc: rows.append(f'DM_AOC_DIR={files_folder(package)}/aoc')
    if sysdata: rows.append(f'DM_SYSDATA={files_folder(package)}/sysdata')
    if romfs_patch: rows.append(f'DM_ROMFS_OVERLAY={files_folder(package)}/romfs_patch')
    rows += profile.env + list(env)
    (tmp / 'env.txt').write_text('\n'.join(rows) + '\n', encoding='utf-8', newline='\n')
    if sets := {**profile.settings, **(settings or {})}: (tmp / 'settings.txt').write_text(''.join(f'{k}={v}\n' for k, v in sets.items()), encoding='utf-8', newline='\n')
    man = dict(title_id=profile.id, name=profile.name, package=package, version=version, display=display, language=language, requires=profile.requires,
               files=n, bytes=b, dlc=sorted(int(p.name) for p in aoc.iterdir() if p.is_dir() and p.name.isdigit()) if aoc else [],
               sysdata=sorted(p.name for p in sysdata.iterdir() if p.is_dir()) if sysdata else [], made=time.strftime('%Y-%m-%d %H:%M:%S'))
    (tmp / MANIFEST).write_text(json.dumps(man, indent=2), encoding='utf-8')
    if dst.exists(): shutil.rmtree(dst)
    for i in range(20):   # Windows: a folder just removed can stay delete-pending while another process (an indexer, a virus scan) holds it, and the rename is refused meanwhile
        try: tmp.rename(dst); break
        except PermissionError:
            if i == 19: raise
            time.sleep(0.5)
    return man


def read_manifest(d):
    try: return json.loads((Path(d) / MANIFEST).read_text(encoding='utf-8'))
    except (OSError, ValueError): return None
