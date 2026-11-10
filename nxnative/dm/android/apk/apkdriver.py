#!/usr/bin/env python3
# apkdriver.py: an installed port's APK again with another Vulkan driver library (DM_VK_DRIVER, gfx_vk_core.c vk_init), or with none (the device's own driver), and/or with its
# host library built again (--host: porter host), without the game's files: every other entry is kept as it is, the ZIP is written and signed again by apksign.py with the
# player's key (the same signer, so it installs over the app and keeps its saves). --asset NAME=FILE puts FILE in as assets/NAME, replacing an entry of that name (porter host: the
# overlay's font, which an APK ported before it came lacks).
# The driver is loaded through libadrenotools and its hook libraries, which only an APK ported with a driver carries (porter port --vulkan-driver): one without them is refused.
# Usage: apkdriver.py <in.apk> <out.apk> <key.json> [--driver <lib.so> | --system] [--host <libdmhost.so>] [--asset NAME=FILE ...]   → prints the driver library's name in the APK ('' with --system)
import argparse, json, os, sys, zipfile

from apksign import build_zip, sign_v2

ABI = 'lib/arm64-v8a/'
OWN = {'libdmhost.so', 'libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so', 'libfile_redirect_hook.so', 'libgsl_alloc_hook.so'}   # nx2android's own libraries; any other is the driver
NEEDS = {'libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so', 'libfile_redirect_hook.so', 'libgsl_alloc_hook.so'}   # what loads a driver (porter/pipeline.py ADRENOTOOLS_LIBS)


def lib_name(path):   # the package manager extracts only lib*.so from lib/<abi>/ (porter/pipeline.py driver_lib_name)
    n = os.path.basename(path)
    return n if n.startswith('lib') else 'lib' + n


KEEP = object()   # rebuild(driver=KEEP): the driver library the APK has stays


def rebuild(src, driver=KEEP, host=None, assets=None):
    """src's entries with every driver library dropped and driver (a path, or None for the device's own) added, libdmhost.so replaced by host (a path) when given, and
    assets ({name: path}) put in as assets/<name>, an entry of that name replaced, the others added after the APK's own:
    ([(name, data, align)], dropped names, the driver library's name)"""
    z = zipfile.ZipFile(src)
    if any(i.compress_type != zipfile.ZIP_STORED for i in z.infolist()): raise SystemExit(f'{src}: not an APK made by mkapk.py (a compressed entry)')
    libs = {os.path.basename(i.filename) for i in z.infolist() if i.filename.startswith(ABI)}
    if 'libdmhost.so' not in libs: raise SystemExit(f'{src}: not an nx2android port (no {ABI}libdmhost.so)')
    if driver not in (KEEP, None) and not NEEDS <= libs:
        raise SystemExit(f'{src} has no libadrenotools (it was ported without --vulkan-driver): port the game again with --vulkan-driver once; later driver changes need no game files')
    assets = dict(assets or {}); entries, dropped, kept = [], [], ''
    for i in z.infolist():
        base = os.path.basename(i.filename)
        if i.filename.startswith(ABI) and base not in OWN:
            if driver is KEEP: kept = base
            else: dropped.append(base); continue
        data = open(host, 'rb').read() if host and i.filename == ABI + 'libdmhost.so' else z.read(i)   # the host library built again (porter host)
        if i.filename.startswith('assets/') and i.filename[7:] in assets: data = open(assets[i.filename[7:]], 'rb').read()
        entries.append((i.filename, data, 16384 if i.filename.startswith(ABI) else 4))   # libraries page-aligned, as mkapk.py writes them (extracted, or mapped in place)
    have = {e for e, _, _ in entries}
    entries += [(f'assets/{n}', open(f, 'rb').read(), 4) for n, f in assets.items() if f'assets/{n}' not in have]   # stored, as mkapk.py writes them: AAsset reads them in place
    if driver is KEEP: return entries, dropped, kept
    added = ''
    if driver:
        added = lib_name(driver)
        if added in OWN: raise SystemExit(f'{driver}: named like one of nx2android\'s own libraries')
        at = next(n for n, (e, _, _) in enumerate(entries) if e == ABI + 'libdmhost.so') + 1
        entries.insert(at, (ABI + added, open(driver, 'rb').read(), 16384))
    return entries, dropped, added


def main():
    p = argparse.ArgumentParser(description='an installed port\'s APK with another Vulkan driver library, signed again with the same key')
    p.add_argument('src'); p.add_argument('out'); p.add_argument('key')
    g = p.add_mutually_exclusive_group(); g.add_argument('--driver', help='the driver library (.so, such as Mesa\'s Turnip)'); g.add_argument('--system', action='store_true', help='no driver library: the device\'s own')
    p.add_argument('--host', help='libdmhost.so built again from this checkout (porter host); the driver stays unless --driver / --system is given')
    p.add_argument('--asset', action='append', default=[], metavar='NAME=FILE', help='FILE as assets/NAME, replacing an entry of that name (porter host: the overlay font)')
    a = p.parse_args()
    if not (a.driver or a.system or a.host or a.asset): p.error('give --driver, --system, --host or --asset')
    if any('=' not in x or x.startswith('=') for x in a.asset): p.error('--asset takes NAME=FILE')
    assets = {n: f for n, _, f in (x.partition('=') for x in a.asset)}
    entries, dropped, added = rebuild(a.src, None if a.system else a.driver or KEEP, a.host, assets)
    kj = json.load(open(a.key)); k = {n: int(v) for n, v in kj.items() if n != 'cert'}; crt = bytes.fromhex(kj['cert'])
    z, cd, eocd = build_zip(entries)
    open(a.out, 'wb').write(sign_v2(z, cd, eocd, k, crt))
    print(json.dumps({'driver': added, 'dropped': dropped}))


if __name__ == '__main__': sys.exit(main())
