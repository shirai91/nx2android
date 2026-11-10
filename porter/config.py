# The player's settings, kept in <user dir>/config.json: toolchain paths, prod.keys, the output folder, the device. The GUI's setup screen and `porter config` write them;
# command-line options win over them.
import json

from porter.paths import user_dir

KEYS = {
    'keys': 'path of prod.keys',
    'firmware': "folder of the console's system content files (.nca; a firmware dump's NAND SYSTEM Contents/registered), for games that read system data",
    'ndk': 'Android NDK folder (r27c)',
    'dotnet': 'dotnet executable (.NET SDK 10)',
    'adb': 'adb executable (platform-tools)',
    'out': 'folder for the APK and data pack (empty = the game folder)',
    'device': 'adb serial of the device to install on (empty = the only one connected)',
    'language': 'game language code (empty = English)',
    'vulkan_driver': 'path of a Vulkan driver .so (Mesa\'s Turnip for Adreno) packed into every port instead of the system\'s (--vulkan-driver wins; empty = the system\'s)',
    'pad_layout': 'controller layout: xbox (A confirms at the bottom, B goes back at the right, Xbox button icons) or empty for the Switch\'s',
}


def path():
    return user_dir() / 'config.json'


def load():
    try: c = json.loads(path().read_text(encoding='utf-8'))
    except (OSError, ValueError): c = {}
    return {k: c.get(k, '') for k in KEYS}


def save(c):
    p = path(); tmp = p.with_suffix('.tmp')
    tmp.write_text(json.dumps({k: c.get(k, '') for k in KEYS}, indent=2), encoding='utf-8'); tmp.replace(p)


def set_value(key, value):
    if key not in KEYS: raise KeyError(f'unknown setting {key!r}: one of {", ".join(KEYS)}')
    c = load(); c[key] = value; save(c)
