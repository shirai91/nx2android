# Another Vulkan driver for a port already on the device (python -m porter driver …), without the game's files: the installed APK is pulled, written again with the new
# driver library by nxnative/dm/android/apk/apkdriver.py and signed with the player's key (the same signer: it installs over the app, its saves stay), and env.txt's
# DM_VK_DRIVER row names the library (gfx_vk_core.c vk_init loads it through libadrenotools; no row: the device's driver). The game's data on the device is not touched.
# Only an APK ported with a driver carries libadrenotools: one ported without needs one port with --vulkan-driver first.
import json, re, shlex, subprocess, sys, tempfile
from pathlib import Path

from porter import datapack, paths
from porter.adb import AdbError
from porter.paths import DM
from porter.upstream import BuildError, say

ROW = 'DM_VK_DRIVER'


def env_with_driver(text, lib):
    """env.txt with its DM_VK_DRIVER row naming lib, in place of the old row (appended when there was none), or dropped when lib is '' (the device's driver)"""
    lines = text.splitlines(); out, done = [], False
    for ln in lines:
        if re.match(rf'\s*{ROW}\s*=', ln):
            if lib and not done: out.append(f'{ROW}={lib}'); done = True
            continue
        out.append(ln)
    if lib and not done: out.append(f'{ROW}={lib}')
    return '\n'.join(out) + '\n'


def installed_apk(adb, package):   # the installed APK's path on the device (ports are one APK, no splits)
    rows = [ln[len('package:'):].strip() for ln in adb.shell(f'pm path {shlex.quote(package)}', check=False).splitlines() if ln.startswith('package:')]
    if not rows: raise BuildError(f'{package} is not installed on the device')
    return rows[0]


def swap(adb, package, driver=None):
    """package's APK again with driver (a .so path), or with the device's own driver when None; env.txt names it. → {'package', 'driver', 'dropped'}"""
    if driver and not Path(driver).is_file(): raise BuildError(f'{driver}: no such driver library')
    with tempfile.TemporaryDirectory(prefix='nx2a_driver_') as t:
        src, out = Path(t) / 'installed.apk', Path(t) / 'driver.apk'
        say(f'{package}: pulling the installed APK')
        adb.run('pull', installed_apk(adb, package), src, timeout=600)
        r = subprocess.run([sys.executable, str(DM / 'android/apk/apkdriver.py'), str(src), str(out), str(paths.signing_key())]
                           + (['--driver', str(driver)] if driver else ['--system']), capture_output=True, text=True, errors='replace')
        if r.returncode: raise BuildError((r.stderr.strip() or r.stdout.strip()).splitlines()[-1])
        res = json.loads(r.stdout.strip().splitlines()[-1])
        adb.shell(f'am force-stop {shlex.quote(package)}', check=False)
        say(f'{package}: installing the APK with ' + (f'{res["driver"]}' if res['driver'] else 'no driver library (the device\'s driver)'))
        try: adb.install(out, package)
        except AdbError as e: raise BuildError(str(e)) from e
        env = f'{datapack.files_folder(package)}/env.txt'
        new = Path(t) / 'env.txt'; new.write_text(env_with_driver(adb.read_text(env), res['driver']), encoding='utf-8', newline='\n')
        adb.run('push', new, env)
    say(f'{package}: ' + (f'{ROW}={res["driver"]}' if res['driver'] else f'no {ROW} row: the device\'s driver') + ' (dropped: ' + (', '.join(res['dropped']) or 'none') + ')')
    return dict(res, package=package)
