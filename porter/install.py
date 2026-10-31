# Installing a port on a device over adb: the APK, then the data pack into the app's files folder (porter/adb.py pushes it in resumable pieces).
# The player's own files there are kept: the pause menu's settings.txt, the logs, the Vulkan pipeline caches; saves are in the app's internal storage, which an update keeps.
import shlex
from pathlib import Path

from porter import datapack
from porter.adb import Adb, AdbError
from porter.upstream import BuildError, PROG, say


def install(apk, data, adb_exe, serial=None, replace_signer=False):
    apk, data = Path(apk), Path(data)
    man = datapack.read_manifest(data)
    if not man: raise BuildError(f'not a data pack (no {datapack.MANIFEST}): {data}')
    if not apk.is_file(): raise BuildError(f'no APK: {apk}')
    if not adb_exe: raise BuildError('adb not found: install Android platform-tools, or set its path with `porter config adb <path>`')
    adb = Adb(adb_exe, serial, log=say)
    PROG.stage('install', step='finding the device')
    pushing = False
    try:
        dev = adb.pick()
        model = adb.prop('ro.product.model')
        say(f'device: {model or dev} ({dev}), GPU {adb.gpu() or "unknown"}')
        if 'bcn' in man.get('requires', []):   # the profile's requires: BC-compressed textures
            bc = adb.bc_textures()
            if bc is False: raise BuildError(f'{man["name"]} needs BC-compressed textures, which this device\'s GPU ({adb.gpu()}) cannot read')
            if bc is None: say(f'[RED] {man["name"]} needs BC-compressed textures; whether this GPU ({adb.gpu() or "unknown"}) reads them is not known')
        free = adb.free_bytes()
        files = datapack.files_folder(man['package'])
        have = adb.remote_sizes(files)
        need = man['bytes'] - sum(have.values())
        if free is not None and need > free: raise BuildError(f'the device has {free / 2**30:.1f} GB free; the game needs {need / 2**30:.1f} GB more')
        PROG.set(step=f'installing {apk.name}')
        adb.install(apk, man['package'], replace_signer=replace_signer)
        PROG.set(step='letting the app create its files folder')
        if adb.prepare_app_files(man['package']): say('started the app once so Android made its files folder (it stops at once without its data)')
        PROG.set(step='pushing the game data', done=0, total=man['bytes'], unit='B'); pushing = True
        have = adb.remote_sizes(files)   # again: an install with --replace-signer uninstalls first, and Android deletes the files folder with the app
        adb.push_tree(data, files, keep=datapack.KEEP_ON_DEVICE, skip=(datapack.MANIFEST,), verify=lambda rel: rel.split('/', 1)[0] not in ('RomFS', 'aoc'),   # the game's own files change only with their version
                      progress=lambda d, t, note: PROG.set(done=d, total=t, note=note), have=have)
        stale = [rel for rel in have if rel.split('/', 1)[0] in datapack.OWNED and not (data / rel).is_file()]   # the push only adds the pack's own files, which are never stale
        if stale:   # an older version's files, which the game would still find
            PROG.set(step=f'removing {len(stale):,} stale files')
            for i in range(0, len(stale), 64): adb.shell(f'cd {shlex.quote(files)} && rm -f ' + ' '.join(shlex.quote(r) for r in stale[i:i + 64]))
        adb.push_file(data / datapack.MANIFEST, f'{files}/{datapack.MANIFEST}')   # last: a pack on the device with its manifest is a complete one
    except AdbError as e:
        try: gone = pushing and adb.serial in [s for s, st, _ in adb.devices() if st == 'device'] and not adb.installed(man['package'])   # a device that went away is not an uninstall
        except AdbError: gone = False
        if gone:   # Android deletes an app's files folder with the app: a push into it then fails part-way (a size mismatch)
            raise BuildError(f'{man["package"]} was uninstalled on the device while its data was being pushed ({e})') from e
        raise BuildError(str(e)) from e
    PROG.end(f'{man["name"]} on {model or dev}')
    say(f'installed: open "{man["name"]}" on the device')
