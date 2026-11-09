#!/usr/bin/env python3
# apkdriver.py: an installed port's APK again with another Vulkan driver library (DM_VK_DRIVER, gfx_vk_core.c vk_init), or with none (the device's own driver), without the
# game's files: every other entry is kept as it is, the ZIP is written and signed again by apksign.py with the player's key (the same signer, so it installs over the app and keeps its saves).
# The driver is loaded through libadrenotools and its hook libraries, which only an APK ported with a driver carries (porter port --vulkan-driver): one without them is refused.
# Usage: apkdriver.py <in.apk> <out.apk> <key.json> (--driver <lib.so> | --system)   → prints the driver library's name in the APK ('' with --system)
import argparse, json, os, sys, zipfile

from apksign import build_zip, sign_v2

ABI = 'lib/arm64-v8a/'
OWN = {'libdmhost.so', 'libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so', 'libfile_redirect_hook.so', 'libgsl_alloc_hook.so'}   # nx2android's own libraries; any other is the driver
NEEDS = {'libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so', 'libfile_redirect_hook.so', 'libgsl_alloc_hook.so'}   # what loads a driver (porter/pipeline.py ADRENOTOOLS_LIBS)


def lib_name(path):   # the package manager extracts only lib*.so from lib/<abi>/ (porter/pipeline.py driver_lib_name)
    n = os.path.basename(path)
    return n if n.startswith('lib') else 'lib' + n


def rebuild(src, driver=None):
    """src's entries with every driver library dropped and driver (a path, or None for the device's own) added: ([(name, data, align)], dropped names, added name)"""
    z = zipfile.ZipFile(src)
    if any(i.compress_type != zipfile.ZIP_STORED for i in z.infolist()): raise SystemExit(f'{src}: not an APK made by mkapk.py (a compressed entry)')
    libs = {os.path.basename(i.filename) for i in z.infolist() if i.filename.startswith(ABI)}
    if 'libdmhost.so' not in libs: raise SystemExit(f'{src}: not an nx2android port (no {ABI}libdmhost.so)')
    if driver and not NEEDS <= libs:
        raise SystemExit(f'{src} has no libadrenotools (it was ported without --vulkan-driver): port the game again with --vulkan-driver once; later driver changes need no game files')
    entries, dropped = [], []
    for i in z.infolist():
        if i.filename.startswith(ABI) and os.path.basename(i.filename) not in OWN: dropped.append(os.path.basename(i.filename)); continue
        entries.append((i.filename, z.read(i), 16384 if i.filename.startswith(ABI) else 4))   # libraries page-aligned, as mkapk.py writes them (extracted, or mapped in place)
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
    g = p.add_mutually_exclusive_group(required=True); g.add_argument('--driver', help='the driver library (.so, such as Mesa\'s Turnip)'); g.add_argument('--system', action='store_true', help='no driver library: the device\'s own')
    a = p.parse_args()
    entries, dropped, added = rebuild(a.src, None if a.system else a.driver)
    kj = json.load(open(a.key)); k = {n: int(v) for n, v in kj.items() if n != 'cert'}; crt = bytes.fromhex(kj['cert'])
    z, cd, eocd = build_zip(entries)
    open(a.out, 'wb').write(sign_v2(z, cd, eocd, k, crt))
    print(json.dumps({'driver': added, 'dropped': dropped}))


if __name__ == '__main__': sys.exit(main())
