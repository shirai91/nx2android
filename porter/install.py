# Installing a port on a device over adb: the APK, then the data pack into the app's files folder (porter/adb.py pushes it in resumable pieces).
# The player's own files there are kept: the pause menu's settings.txt, the logs, the Vulkan pipeline caches; saves are in the app's internal storage, which an update keeps.
# --romfs-at <folder>: the app reads the game's RomFS from a folder already on the device (a game dump an emulator also reads, a folder on the SD card) instead of a copy in
# its files folder, which saves that copy's 7 to 18 GB. The folder must hold exactly the pack's RomFS, names and sizes and a sample's content (--romfs-push puts it there), the app gets All files
# access over adb, and the installer writes DM_ROMFS=<folder> into the device's env.txt under ROMFS_MARK. The pack's env.txt is the same on every device, and the folder is
# a fact of one device, so the row is the installer's, not the pack's: a later install without --romfs-at carries it over from the device's env.txt (and checks the folder
# again), and --romfs-at app moves the RomFS back into the files folder. The DLC (aoc/) stays in the files folder: small for most games, and laid out by the port.
import hashlib, posixpath, re, shlex, tempfile, zipfile
from pathlib import Path

from porter import datapack, paths
from porter.adb import Adb, AdbError, long_path
from porter.upstream import BuildError, PROG, say

APP = 'app'                                                  # --romfs-at app: the RomFS in the app's files folder (the default)
ROMFS_MARK = '# porter install --romfs-at'                   # in the device's env.txt: the DM_ROMFS row under it is the installer's, carried over by later installs
ACCESS = 'android.permission.MANAGE_EXTERNAL_STORAGE'        # All files access, which the APK asks for (mkapk.py) and the installer grants
ACCESS_HOW = 'Settings > Apps > {name} > All files access (Android 10: Permissions > Files and media)'
SAMPLE_FILES, SAMPLE_MAX = 256, 4 << 20                      # the folder's content check: up to this many of its files, each of at most this many bytes (romfs_sample)


def romfs_target(arg, files=None):
    """--romfs-at's folder, checked before anything touches the device: None for the app's files folder (app, or that folder's own RomFS). files: the app's files folder"""
    if arg is None or arg == APP: return None
    if arg != arg.strip() or any(c in arg for c in '\r\n\0') or not arg.startswith('/'):
        raise BuildError(f'--romfs-at takes a folder on the device as an absolute path (/sdcard/…, /storage/…), or app: {arg!r}')
    d = posixpath.normpath(arg)
    if files and d == f'{files}/{datapack.ROMFS}': return None
    if files and (d + '/').startswith(files + '/'): raise BuildError(f'--romfs-at {d}: the app\'s files folder is the installer\'s own; --romfs-at app keeps the RomFS there')
    if not d.startswith(('/sdcard/', '/storage/')): raise BuildError(f'--romfs-at {d}: the app can read only shared storage, /sdcard/… or /storage/… (an SD card: /storage/<its id>/…)')
    if re.search(r'/Android/(data|obb)(/|$)', d, re.I):   # FUSE keeps every app out of the others' Android/data and Android/obb, All files access or not
        raise BuildError(f'--romfs-at {d}: Android lets no app read a folder under another app\'s Android/data or Android/obb, even with All files access: '
                         'move the game\'s files elsewhere (/sdcard/Games/…), or keep them in this app\'s files folder with --romfs-at app')
    if len(d.encode()) > 512: raise BuildError(f'--romfs-at: a folder path of at most 512 bytes (the host\'s path buffers): {d}')
    return d


def device_romfs(env_text):   # the folder an earlier --romfs-at install wrote into the device's env.txt (the DM_ROMFS row under ROMFS_MARK), None when there is none
    lines, out = env_text.splitlines(), None
    for prev, ln in zip([''] + lines, lines):
        if prev.startswith(ROMFS_MARK) and ln.startswith('DM_ROMFS='): out = ln[len('DM_ROMFS='):].rstrip() or None
    return out


def env_with_romfs(pack_env, folder):   # the pack's env.txt with the installer's row last: the app applies the rows in order (front_android.c env_file), so it wins
    return pack_env.rstrip('\n') + f'\n{ROMFS_MARK}: the game\'s RomFS is read from this folder (later installs keep it; --romfs-at app moves it into the files folder)\nDM_ROMFS={folder}\n'


def romfs_diff(want, have):   # want, have: {path in the RomFS: size} → (missing, of another size, not the game's), each sorted
    return sorted(want.keys() - have.keys()), sorted(r for r in want.keys() & have.keys() if want[r] != have[r]), sorted(have.keys() - want.keys())


def romfs_sample(rom, n=SAMPLE_FILES, max_size=SAMPLE_MAX):
    """The RomFS files whose content the installer compares by MD5 (romfs_changed): up to n of at most max_size bytes, spread evenly over the sorted paths. Names and
    sizes alone let a dump of another game version through when the files it changed kept their sizes; a sample of the small files catches that (at most SAMPLE_FILES x SAMPLE_MAX = 1 GB read on each side: seconds on internal storage, longer on an SD card)"""
    small = sorted(r for r, s in rom.items() if s <= max_size)
    return small if len(small) <= n else [small[i * len(small) // n] for i in range(n)]


def romfs_changed(adb, data, target, rom, there):
    """The sample's files that target holds at the pack's size but with other bytes (MD5), sorted; None when the device gave no MD5 at all (nothing compared)"""
    sample = [r for r in romfs_sample(rom) if there.get(r) == rom[r]]
    if not sample: return []
    got = adb.remote_md5(target, sample)
    if not got: return None
    return sorted(r for r in sample if r in got and got[r] != hashlib.md5(Path(long_path(Path(data) / datapack.ROMFS / r)).read_bytes()).hexdigest())


def romfs_mismatch(folder, want, have, n=5, changed=()):
    """What keeps folder's listing (have) from being the pack's RomFS (want), the first n of each kind; changed: files of the right size whose content differs
    (romfs_changed). '' when it is the same"""
    missing, other, extra = romfs_diff(want, have)
    if not have: return f'{folder} holds no files (it is not on the device, or it is empty)'
    first = lambda xs, f=str: ', '.join(f(x) for x in xs[:n]) + (', …' if len(xs) > n else '')
    sizes = lambda r: f'{r}: {have[r]:,} bytes, the pack has {want[r]:,}'
    parts = ([f'{len(missing):,} of the game\'s files are not there ({first(missing)})'] if missing else []) + \
            ([f'{len(other):,} have another size ({first(other, sizes)})'] if other else []) + \
            ([f'{len(changed):,} have the right size but other content (MD5: {first(list(changed))})'] if changed else []) + \
            ([f'{len(extra):,} are not the game\'s ({first(extra)})'] if extra else [])
    return f'{folder} is not this pack\'s RomFS ({len(want):,} files): ' + '; '.join(parts) if parts else ''


def apk_asks_access(apk):   # whether the APK asks for All files access: one made before mkapk.py did cannot be given it (`appops set` takes, but FUSE checks the permission)
    try:
        with zipfile.ZipFile(apk) as z: m = z.read('AndroidManifest.xml')
    except (OSError, KeyError, zipfile.BadZipFile): return False
    return ACCESS.encode('utf-16-le') in m or ACCESS.encode() in m


def install(apk, data, adb_exe, serial=None, replace_signer=False, romfs_at=None, romfs_push=False):
    """romfs_at: a folder on the device that holds the game's RomFS (romfs_push: push the pack's RomFS into it first), 'app' for the app's files folder, None: as the
    device has it (an earlier --romfs-at install's folder, else the files folder)"""
    apk, data = Path(apk), Path(data)
    man = datapack.read_manifest(data)
    if not man: raise BuildError(f'not a data pack (no {datapack.MANIFEST}): {data}')
    if not apk.is_file(): raise BuildError(f'no APK: {apk}')
    if not adb_exe: raise BuildError('adb not found: install Android platform-tools, or set its path with `porter config adb <path>`')
    files = datapack.files_folder(man['package'])
    target = romfs_target(romfs_at, files)
    if romfs_push and not target: raise BuildError('--romfs-push needs --romfs-at <a folder on the device outside the app\'s files folder>')
    adb = Adb(adb_exe, serial, log=say)
    PROG.stage('install', step='finding the device')
    pushing = False
    try:
        dev = adb.pick(); lock = adb.hold(paths.user_dir())   # (lock: released when this returns)
        model = adb.prop('ro.product.model')
        say(f'device: {model or dev} ({dev}), GPU {adb.gpu() or "unknown"}')
        if 'bcn' in man.get('requires', []):   # the profile's requires: BC-compressed textures
            bc = adb.bc_textures()
            if bc is False: raise BuildError(f'{man["name"]} needs BC-compressed textures, which this device\'s GPU ({adb.gpu()}) cannot read')
            if bc is None: say(f'[RED] {man["name"]} needs BC-compressed textures; whether this GPU ({adb.gpu() or "unknown"}) reads them is not known')
        was = device_romfs(adb.read_text(f'{files}/env.txt'))   # an earlier --romfs-at install's folder
        if romfs_at is None and was:
            target = romfs_target(was, files)
            if target: say(f'the app reads its RomFS from {target} (an earlier install\'s --romfs-at; --romfs-at app moves it into its files folder)')
        rom = datapack.romfs_sizes(data) if target else {}
        missing = other = changed = []; there = {}
        if target:   # what can refuse comes before the APK goes in, so a refused install changes nothing on the device (but a new app's grant: below)
            if not rom: raise BuildError(f'{data} has no RomFS to compare {target} with')
            if not apk_asks_access(apk): raise BuildError(f'{apk.name} does not ask for All files access, which reading {target} needs (a port made by an earlier nx2android): port the game again')
            PROG.set(step=f'comparing {target} with the pack\'s RomFS')
            there = adb.remote_sizes(target)
            missing, other, extra = romfs_diff(rom, there)
            if not extra and (romfs_push or not (missing or other)):   # a listing that can be used: a sample of the files compared by content too
                PROG.set(step=f'comparing the content of {target} with the pack\'s RomFS')
                changed = romfs_changed(adb, data, target, rom, there)
                if changed is None: say(f'[RED] the device gave no MD5 of the files in {target}: only their names and sizes were compared with the pack\'s RomFS'); changed = []
            if bad := romfs_mismatch(target, rom, there, changed=changed):
                if extra or not romfs_push:
                    whose = f'{man["name"]} {man.get("display") or man["version"]}'
                    raise BuildError(f'{bad}. ' + (f'It came from an earlier install\'s --romfs-at. ' if romfs_at is None else '') + f'Give --romfs-at the folder of {whose}\'s RomFS'
                                     + (' (--romfs-push adds files, it never deletes any: empty the folder, or choose another)' if extra else
                                        ', add --romfs-push to put the pack\'s RomFS into that folder') + ', or --romfs-at app to keep the RomFS in the app\'s files folder')
                say(f'{target}: {len(missing) + len(other) + len(changed):,} of the RomFS\'s {len(rom):,} files to push (--romfs-push)')
            else: say(f'{target} holds this pack\'s RomFS ({len(rom):,} files, {sum(rom.values()) / 2**30:.1f} GB): it is not pushed')
        free = adb.free_bytes()
        have = adb.remote_sizes(files)
        if target:   # the RomFS is not pushed into the files folder; a copy an earlier install left there goes only after env.txt points elsewhere, so it is not counted as freed
            need = man['bytes'] - sum(rom.values()) - sum(s for r, s in have.items() if not r.startswith(datapack.ROMFS + '/'))
            if push := sum(rom[r] for r in missing + other + changed):
                if adb.same_volume('/sdcard', target): need += push
                elif (tfree := adb.free_bytes(target)) is not None and push > tfree: raise BuildError(f'{target}\'s volume has {tfree / 2**30:.1f} GB free; the RomFS needs {push / 2**30:.1f} GB more')
        else: need = man['bytes'] - sum(have.values())
        if free is not None and need > free: raise BuildError(f'the device has {free / 2**30:.1f} GB free; the game needs {need / 2**30:.1f} GB more')
        if target and not replace_signer and adb.installed(man['package']):   # an update: the grant (the app's uid's, which the update keeps) before the APK goes in
            PROG.set(step='granting the app All files access')
            ok, state = adb.storage_access(man['package'], grant=True)
            if not ok and adb.requests_permission(man['package'], ACCESS if adb.sdk() >= 30 else 'android.permission.READ_EXTERNAL_STORAGE'):
                raise BuildError(f'the app needs All files access to read {target}, and the device did not grant it over adb ({state}): allow it on the device '
                                 f'({ACCESS_HOW.format(name=man["name"])}), then install again')
        PROG.set(step=f'installing {apk.name}')
        adb.install(apk, man['package'], replace_signer=replace_signer)
        PROG.set(step='letting the app create its files folder')
        if adb.prepare_app_files(man['package']): say('started the app once so Android made its files folder (it stops at once without its data)')
        if target:   # after the install too: a new package, one --replace-signer made again, or an update from an APK that did not ask for the access (granted above
            # when it did) has no grant yet. Before the push, so a refusal leaves the data as it was: a new app has none, and an older APK's update is the one case
            # that cannot be refused before the APK goes in
            PROG.set(step='granting the app All files access')
            ok, state = adb.storage_access(man['package'], grant=True)
            if not ok: raise BuildError(f'the app needs All files access to read {target}, and the device did not grant it over adb ({state}): allow it on the device '
                                        f'({ACCESS_HOW.format(name=man["name"])}), then install again')
            say(f'{state}: the app reads its RomFS from {target}')
        PROG.set(step='pushing the game data', done=0, total=man['bytes'] - sum(rom.values()), unit='B'); pushing = True
        have = adb.remote_sizes(files)   # again: an install with --replace-signer uninstalls first, and Android deletes the files folder with the app
        adb.push_tree(data, files, keep=datapack.KEEP_ON_DEVICE, skip=(datapack.MANIFEST,) + (('env.txt',) if target else ()), exclude=(datapack.ROMFS,) if target else (),
                      verify=lambda rel: rel.split('/', 1)[0] not in ('RomFS', 'aoc'),   # the game's own files change only with their version
                      progress=lambda d, t, note: PROG.set(done=d, total=t, note=note), have=have)
        if target and (missing or other or changed):   # --romfs-push: the pack's RomFS into the folder (changed: pushed again when their MD5 differs), then the folder checked again
            PROG.set(step=f'pushing the RomFS into {target}', done=0, total=sum(rom[r] for r in missing + other + changed), unit='B')
            again = set(changed)
            adb.push_tree(data / datapack.ROMFS, target, verify=lambda rel: rel in again, progress=lambda d, t, note: PROG.set(done=d, total=t, note=note), have=there)
            now = adb.remote_sizes(target)
            if bad := romfs_mismatch(target, rom, now, changed=romfs_changed(adb, data, target, rom, now) or []): raise BuildError(f'after the push, {bad}')
        if target:   # the pack's env.txt with the installer's row
            with tempfile.TemporaryDirectory(prefix='nx2a_env_') as t:
                env = Path(t) / 'env.txt'; env.write_text(env_with_romfs((data / 'env.txt').read_text(encoding='utf-8'), target), encoding='utf-8', newline='\n')
                adb.push_file(env, f'{files}/env.txt')
        elif was and romfs_at == APP: say(f'the app reads its RomFS from its files folder again; {was} is left as it is')
        stale = [rel for rel in have if rel.split('/', 1)[0] in datapack.OWNED and not (target and rel.startswith(datapack.ROMFS + '/')) and not (data / rel).is_file()]   # the push only adds the pack's own files, which are never stale
        if stale:   # an older version's files, which the game would still find
            PROG.set(step=f'removing {len(stale):,} stale files')
            for i in range(0, len(stale), 64): adb.shell(f'cd {shlex.quote(files)} && rm -f ' + ' '.join(shlex.quote(r) for r in stale[i:i + 64]))
        if old := [r for r in have if r.startswith(datapack.ROMFS + '/')] if target else []:   # a second copy of the game, now that env.txt points at target
            PROG.set(step='removing the RomFS copy in the files folder')
            adb.shell(f'rm -rf {shlex.quote(f"{files}/{datapack.ROMFS}")}', timeout=1200)
            say(f'removed the earlier copy of the RomFS from the app\'s files folder ({len(old):,} files, {sum(have[r] for r in old) / 2**30:.1f} GB)')
        adb.push_file(data / datapack.MANIFEST, f'{files}/{datapack.MANIFEST}')   # last: a pack on the device with its manifest is a complete one
    except AdbError as e:
        try: gone = pushing and adb.serial in [s for s, st, _ in adb.devices() if st == 'device'] and not adb.installed(man['package'])   # a device that went away is not an uninstall
        except AdbError: gone = False
        if gone:   # Android deletes an app's files folder with the app: a push into it then fails part-way (a size mismatch)
            raise BuildError(f'{man["package"]} was uninstalled on the device while its data was being pushed ({e})') from e
        raise BuildError(str(e)) from e
    PROG.end(f'{man["name"]} on {model or dev}')
    say(f'installed: open "{man["name"]}" on the device')
