#!/usr/bin/env python3
# mkapk.py: build and sign a code-less NativeActivity APK with the Python standard library only (no JDK, no Android SDK).
#   binary AndroidManifest.xml (AXML) -> stored ZIP with the .so page-aligned -> APK Signature Scheme v2 (RSA-2048, SHA-256) with a self-signed cert.
# Usage: mkapk.py <lib.so> <out.apk> [key.json] [--package P] [--label L] [--lib NAME] [--landscape] [--icon icon.png [--icon-fg fg.png --icon-bg bg.png]] [--release] [--version-code N] [--version-name V]   (the key is created on first use and reused, so reinstalls keep the same signer)
import hashlib, json, os, struct, sys, time

import argparse as _ap
_p = _ap.ArgumentParser(); _p.add_argument('so'); _p.add_argument('out'); _p.add_argument('key', nargs='?'); _p.add_argument('--package', default='org.nx2a.probe')
_p.add_argument('--label', default='NX2A Probe'); _p.add_argument('--lib', default='probe'); _p.add_argument('--landscape', action='store_true')
_p.add_argument('--extra-so', action='append', default=[], help='another arm64 library packed beside the main one (a Vulkan validation layer)')
_p.add_argument('--icon', help='launcher icon (a PNG, used at its own size: nodpi)')
_p.add_argument('--icon-fg', help='with --icon-bg: an adaptive icon\'s foreground layer (a PNG of the 108 dp layer, the launcher\'s mask shows the middle 72 dp); --icon stays for older launchers')
_p.add_argument('--icon-bg', help='the adaptive icon\'s background layer (a PNG, like --icon-fg)')
_p.add_argument('--asset', action='append', default=[], metavar='NAME=FILE', help='a file the app reads with AAssetManager as NAME (assets/NAME in the APK): the gamepad theme')
_p.add_argument('--dex', help='optional classes.dex for a custom NativeActivity')
_p.add_argument('--activity', default='android.app.NativeActivity', help='NativeActivity class name')
_p.add_argument('--release', action='store_true', help='not debuggable')
_p.add_argument('--version-code', type=int, default=1, help='versionCode: a later port of the game must have a higher one to install over it')
_p.add_argument('--version-name', default='1.0', help='versionName: the version the player sees (the game\'s display version)'); ARGS = _p.parse_args()
PKG, LIB, LABEL = ARGS.package, ARGS.lib, ARGS.label
if ARGS.activity != 'android.app.NativeActivity' and not ARGS.dex: _p.error('--activity requires --dex')
if bool(ARGS.icon_fg) != bool(ARGS.icon_bg) or (ARGS.icon_fg and not ARGS.icon): _p.error('--icon-fg and --icon-bg go together, with --icon')
MIN_SDK, TARGET_SDK = 29, 36
ANDROID_NS = 'http://schemas.android.com/apk/res/android'
# configChanges: what the game takes without its activity made again. Android relaunches the activity for any other change while it runs, and that ends the game
# (front_android.c on_destroy: one game per process): switching dark mode (uiMode) ended Link's Awakening in the emulator's Android 13 image. The rest are what
# Android can change during play (a keyboard or controller connected, split screen, the language, the font size): the set game engines declare
CONFIG_CHANGES = (0x0001 | 0x0002 | 0x0004 | 0x0008 | 0x0010 | 0x0020 | 0x0040 | 0x0080 | 0x0100 | 0x0200 | 0x0400 | 0x0800 | 0x1000 | 0x2000 | 0x4000
                  | 0x10000000 | 0x40000000)   # mcc mnc locale touchscreen keyboard keyboardHidden navigation orientation screenLayout uiMode screenSize
                                               # smallestScreenSize density layoutDirection colorMode fontWeightAdjustment fontScale

# ---------------- binary XML ----------------
ATTR_IDS = {  # android: attribute resource IDs (frameworks/base/core/res/res/values/public.xml)
    'label': 0x01010001, 'icon': 0x01010002, 'name': 0x01010003, 'hasCode': 0x0101000c, 'debuggable': 0x0101000f, 'exported': 0x01010010,
    'configChanges': 0x0101001f, 'screenOrientation': 0x0101001e, 'value': 0x01010024, 'minSdkVersion': 0x0101020c, 'versionCode': 0x0101021b,
    'versionName': 0x0101021c, 'targetSdkVersion': 0x01010270, 'maxSdkVersion': 0x01010271, 'isGame': 0x010103f4, 'extractNativeLibs': 0x010104ea, 'appCategory': 0x01010545,
    'requestLegacyExternalStorage': 0x01010603, 'enableOnBackInvokedCallback': 0x0101066c, 'drawable': 0x01010199}   # (maxSdkVersion and requestLegacyExternalStorage read from android-36's android.jar, R$attr)
T_REF, T_STRING, T_INT_DEC, T_INT_HEX, T_BOOL = 0x01, 0x03, 0x10, 0x11, 0x12
ICON_ID, ICON_FG_ID, ICON_BG_ID = 0x7f010000, 0x7f010001, 0x7f010002   # drawable/icon, icon_fg, icon_bg in resources.arsc (res_table)
# A plain icon is drawn small on a white disc by Android 8+ launchers; an adaptive one fills the launcher's icon shape
ADAPTIVE_ICON = ('adaptive-icon', [], [('background', [(True, 'drawable', ('ref', ICON_BG_ID))], []), ('foreground', [(True, 'drawable', ('ref', ICON_FG_ID))], [])])

# (tag, [(ns_android?, name, value)], children)
MANIFEST = ('manifest', [(False, 'package', PKG), (True, 'versionCode', ARGS.version_code), (True, 'versionName', ARGS.version_name)], [
    ('uses-sdk', [(True, 'minSdkVersion', MIN_SDK), (True, 'targetSdkVersion', TARGET_SDK)], []),
    ('uses-permission', [(True, 'name', 'android.permission.VIBRATE')], []),   # rumble and the touch gamepad's taps (a normal permission: granted at install)
    # Local play over the LAN (env.txt's DM_LAN=1, hle_ldn.c / hle_socket.c): sockets, the Wi-Fi address, and a multicast lock without which the Wi-Fi driver drops the broadcasts
    # other players scan with. All normal permissions (granted at install); a game with DM_LAN off opens no socket
    ('uses-permission', [(True, 'name', 'android.permission.INTERNET')], []),
    ('uses-permission', [(True, 'name', 'android.permission.ACCESS_NETWORK_STATE')], []),
    ('uses-permission', [(True, 'name', 'android.permission.ACCESS_WIFI_STATE')], []),
    ('uses-permission', [(True, 'name', 'android.permission.CHANGE_WIFI_MULTICAST_STATE')], []),
    # A RomFS read from a folder outside the app's files folder (porter install --romfs-at: a game dump another app also reads, a folder on the SD card; env.txt's DM_ROMFS):
    # All files access from Android 11 (an appop, off until porter's `appops set` or the player's switch in Settings turns it on), and on Android 10, which has no
    # All files access, READ_EXTERNAL_STORAGE in the legacy storage view (requestLegacyExternalStorage below; `pm grant`). Neither is asked for at run time, so a port
    # that reads its RomFS from its files folder runs as before; requestLegacyExternalStorage is ignored from Android 11 with this target SDK
    ('uses-permission', [(True, 'name', 'android.permission.MANAGE_EXTERNAL_STORAGE')], []),
    ('uses-permission', [(True, 'name', 'android.permission.READ_EXTERNAL_STORAGE'), (True, 'maxSdkVersion', 29)], []),
    ('application', [(True, 'label', LABEL)] + ([(True, 'icon', ('ref', ICON_ID))] if ARGS.icon else []) + [(True, 'hasCode', bool(ARGS.dex)), (True, 'debuggable', not ARGS.release), (True, 'isGame', True), (True, 'extractNativeLibs', True),
        (True, 'requestLegacyExternalStorage', True),
        (True, 'appCategory', 0),   # a game (appCategory game = 0, and the older isGame): Android's game mode and game dashboard, and the device maker's game tools, apply to it
        # no predictive back: from target SDK 36 the system takes the back gesture itself and finishes the activity, which ends the game (front_android.c on_destroy), without
        # dispatching KEYCODE_BACK; opted out, the gesture comes as the Back key, which opens the front end's menu
        (True, 'enableOnBackInvokedCallback', False)], [
        ('activity', [(True, 'name', ARGS.activity), (True, 'exported', True), (True, 'configChanges', ('hex', CONFIG_CHANGES))] + ([(True, 'screenOrientation', 6)] if ARGS.landscape else []), [
            ('meta-data', [(True, 'name', 'android.app.lib_name'), (True, 'value', LIB)], []),
            ('intent-filter', [], [
                ('action', [(True, 'name', 'android.intent.action.MAIN')], []),
                ('category', [(True, 'name', 'android.intent.category.LAUNCHER')], [])])])])])

def axml(root):
    # string pool: attribute names that carry resource IDs come first, in resource-map order
    res_names = sorted({n for _, n, _ in walk_attrs(root) if n in ATTR_IDS}, key=lambda n: ATTR_IDS[n])
    strings = list(res_names)
    def s(x):
        if x not in strings: strings.append(x)
        return strings.index(x)
    s('android'); s(ANDROID_NS)
    body = []
    def chunk(t, hsize, payload): return struct.pack('<HHI', t, hsize, 8 + len(payload)) + payload
    def node(n, line=[1]):
        tag, attrs, kids = n
        enc = []
        for is_ns, name, v in attrs:
            ns = s(ANDROID_NS) if is_ns else 0xffffffff
            if isinstance(v, bool): raw, dt, data = 0xffffffff, T_BOOL, 0xffffffff if v else 0
            elif isinstance(v, int): raw, dt, data = 0xffffffff, T_INT_DEC, v
            elif isinstance(v, tuple): raw, dt, data = 0xffffffff, T_REF if v[0] == 'ref' else T_INT_HEX, v[1]
            else: raw, dt, data = s(v), T_STRING, s(v)
            enc.append((ATTR_IDS.get(name, 0xffffffff) if is_ns else 0xffffffff, struct.pack('<IIIHBBI', ns, s(name), raw, 8, 0, dt, data)))
        enc.sort(key=lambda a: a[0])   # resource-ID attributes ascending, plain ones (package) last
        a = b''.join(e for _, e in enc)
        body.append(chunk(0x0102, 16, struct.pack('<II', line[0], 0xffffffff) + struct.pack('<IIHHHHHH', 0xffffffff, s(tag), 20, 20, len(attrs), 0, 0, 0) + a))
        line[0] += 1
        for k in kids: node(k)
        body.append(chunk(0x0103, 16, struct.pack('<IIII', line[0], 0xffffffff, 0xffffffff, s(tag)))); line[0] += 1
    ns = struct.pack('<IIII', 1, 0xffffffff, s('android'), s(ANDROID_NS))
    node(root)
    nodes = chunk(0x0100, 16, ns) + b''.join(body) + chunk(0x0101, 16, ns)
    # UTF-16 string pool
    offs, data = [], b''
    for x in strings:
        offs.append(len(data)); u = x.encode('utf-16-le'); data += struct.pack('<H', len(x)) + u + b'\0\0'
    data += b'\0' * (-len(data) % 4)
    hdr = struct.pack('<IIIII', len(strings), 0, 0, 28 + 4 * len(strings), 0)
    pool = chunk(0x0001, 28, hdr + b''.join(struct.pack('<I', o) for o in offs) + data)
    rmap = chunk(0x0180, 8, b''.join(struct.pack('<I', ATTR_IDS[n]) for n in res_names))
    return chunk(0x0003, 8, pool + rmap + nodes)

# ---------------- resources.arsc: the icon's drawables ----------------
def rchunk(t, hsize, header, body=b''): return struct.pack('<HHI', t, hsize, hsize + len(body)) + header + body
def string_pool(strings):   # UTF-16 string pool chunk
    offs, data = [], b''
    for x in strings:
        offs.append(len(data)); data += struct.pack('<H', len(x)) + x.encode('utf-16-le') + b'\0\0'
    data += b'\0' * (-len(data) % 4)
    return rchunk(0x0001, 28, struct.pack('<IIIII', len(strings), 0, 0, 28 + 4 * len(strings), 0), b''.join(struct.pack('<I', o) for o in offs) + data)
NODPI = struct.pack('<I', 64) + b'\0' * 10 + struct.pack('<H', 0xffff) + b'\0' * 48                                       # ResTable_config: density nodpi, the rest any
ANYDPI_V26 = struct.pack('<I', 64) + b'\0' * 10 + struct.pack('<H', 0xfffe) + b'\0' * 8 + struct.pack('<HH', 26, 0) + b'\0' * 36   # density anydpi, sdkVersion 26
def res_table(drawables):   # [(name, [(config, path)])]: drawable i is resource ICON_ID + i, a file per config
    paths = [p for _, files in drawables for _, p in files]
    types, keys = string_pool(['drawable']), string_pool([n for n, _ in drawables])
    # an entry with files for more than one config varies by density and SDK version (ResTable_config CONFIG_DENSITY | CONFIG_VERSION)
    spec = rchunk(0x0202, 16, struct.pack('<BBHI', 1, 0, 0, len(drawables)), b''.join(struct.pack('<I', 0x0500 if len(f) > 1 else 0) for _, f in drawables))
    typs = b''
    for config in dict.fromkeys(c for _, files in drawables for c, _ in files):
        offs, entries = [], b''
        for i, (_, files) in enumerate(drawables):
            path = dict(files).get(config)
            if path is None: offs.append(0xffffffff); continue   # NO_ENTRY: no file for this config
            offs.append(len(entries)); entries += struct.pack('<HHI', 8, 0, i) + struct.pack('<HBBI', 8, 0, T_STRING, paths.index(path))   # key i -> the file's path
        typs += rchunk(0x0201, 20 + 64, struct.pack('<BBHII', 1, 0, 0, len(drawables), 20 + 64 + 4 * len(drawables)) + config,
                       b''.join(struct.pack('<I', o) for o in offs) + entries)
    name = PKG.encode('utf-16-le')[:254].ljust(256, b'\0')
    pkg = rchunk(0x0200, 288, struct.pack('<I', 0x7f) + name + struct.pack('<IIIII', 288, 1, 288 + len(types), len(drawables), 0), types + keys + spec + typs)
    return rchunk(0x0002, 12, struct.pack('<I', 1), string_pool(paths) + pkg)

def walk_attrs(n):
    for a in n[1]: yield a
    for k in n[2]: yield from walk_attrs(k)

from apksign import build_zip, cert, gen_key, sign_v2   # the stored ZIP and the v2 signer (apksign.py)

def main():
    so, out = ARGS.so, ARGS.out
    keyf = ARGS.key if ARGS.key else os.path.join(os.path.dirname(os.path.abspath(out)), 'probe-signing-key.json')
    if os.path.exists(keyf): k = {a: int(v) for a, v in json.load(open(keyf)).items() if a != 'cert'}; crt = bytes.fromhex(json.load(open(keyf))['cert'])
    else:
        t = time.time(); k = gen_key(); crt = cert(k)
        json.dump({**{a: str(v) for a, v in k.items()}, 'cert': crt.hex()}, open(keyf, 'w'))
        print(f'new signing key {keyf} ({time.time() - t:.1f} s)')
    entries = [('AndroidManifest.xml', axml(MANIFEST), 4), (f'lib/arm64-v8a/lib{LIB}.so', open(so, 'rb').read(), 16384)]
    if ARGS.dex: entries.append(('classes.dex', open(ARGS.dex, 'rb').read(), 4))
    entries += [(f'lib/arm64-v8a/{os.path.basename(x)}', open(x, 'rb').read(), 16384) for x in ARGS.extra_so]
    entries += [(f'assets/{n}', open(f, 'rb').read(), 4) for n, _, f in (x.partition('=') for x in ARGS.asset)]   # stored, as every entry: AAsset reads them in place
    if ARGS.icon:
        icon = [(NODPI, 'res/drawable-nodpi-v4/icon.png')] + ([(ANYDPI_V26, 'res/drawable-anydpi-v26/icon.xml')] if ARGS.icon_fg else [])
        drawables = [('icon', icon)] + ([('icon_fg', [(NODPI, 'res/drawable-nodpi-v4/icon_fg.png')]), ('icon_bg', [(NODPI, 'res/drawable-nodpi-v4/icon_bg.png')])] if ARGS.icon_fg else [])
        entries += [('resources.arsc', res_table(drawables), 4), ('res/drawable-nodpi-v4/icon.png', open(ARGS.icon, 'rb').read(), 4)]
        if ARGS.icon_fg: entries += [('res/drawable-anydpi-v26/icon.xml', axml(ADAPTIVE_ICON), 4), ('res/drawable-nodpi-v4/icon_fg.png', open(ARGS.icon_fg, 'rb').read(), 4),
                                     ('res/drawable-nodpi-v4/icon_bg.png', open(ARGS.icon_bg, 'rb').read(), 4)]
    z, cd, eocd = build_zip(entries)
    apk = sign_v2(z, cd, eocd, k, crt)
    open(out, 'wb').write(apk)
    print(f'{out}: {len(apk)} bytes, signer SHA-256 {hashlib.sha256(crt).hexdigest()[:16]}...')

if __name__ == '__main__': main()
