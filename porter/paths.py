# Where things are: the repository (and the nxnative subtree in it), the player's own folder (settings, the APK signing key).
import os, shutil, sys
from pathlib import Path

REPO = Path(sys.executable).parent if getattr(sys, 'frozen', False) else Path(__file__).resolve().parent.parent   # the bundle: nxnative/ beside nx2android.exe
LOCAL_TOOLS = REPO / 'tools'   # a developer's toolchains next to the repository (not committed); the player installs their own


def user_dir():
    """The player's nx2android folder: %APPDATA%\\nx2android on Windows, ~/Library/Application Support/nx2android on a Mac, ~/.config/nx2android elsewhere.
    NX2A_HOME overrides it (tests)"""
    if os.environ.get('NX2A_HOME'): d = Path(os.environ['NX2A_HOME'])
    elif os.name == 'nt': d = Path(os.environ.get('APPDATA') or Path.home() / 'AppData/Roaming') / 'nx2android'
    elif sys.platform == 'darwin': d = Path.home() / 'Library/Application Support/nx2android'
    else: d = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'nx2android'
    d.mkdir(parents=True, exist_ok=True)
    return d


def engine_copy(bundle, image):
    """The engine (bundle/nxnative) copied to <user dir>/engine, once for each AppImage (packaging/build_appimage.py) → the copy's nxnative folder.
    An AppImage is a read-only mount at a new /tmp/.mount_* path each launch: the .NET builds write obj/ beside their projects, and the host build
    (dm/android/build.py) rebuilds every object when its include paths change. The copy (about 15 MB) is writable and keeps one path"""
    try: st = Path(image).stat(); key = f'{(bundle / "VERSION").read_text().strip()}\n{st.st_size}\n{st.st_mtime_ns}\n'
    except OSError: key = None
    dst = user_dir() / 'engine'
    try:
        if key and (dst / 'KEY').read_text() == key: return dst / 'nxnative'
    except OSError: pass
    tmp = dst.with_name(f'engine.{os.getpid()}.partial'); shutil.rmtree(tmp, ignore_errors=True)
    # without the bundle's file times: a new copy is newer than the objects an older AppImage's port left, so the host build compiles them again
    shutil.copytree(bundle / 'nxnative', tmp / 'nxnative', copy_function=shutil.copy)
    (tmp / 'KEY').write_text(key or '')
    shutil.rmtree(dst, ignore_errors=True)
    try: tmp.rename(dst)
    except OSError: shutil.rmtree(tmp, ignore_errors=True)   # another launch put its copy in first
    return dst / 'nxnative'


NXNATIVE = engine_copy(REPO, os.environ['APPIMAGE']) if getattr(sys, 'frozen', False) and os.environ.get('APPIMAGE') else REPO / 'nxnative'
DM = NXNATIVE / 'dm'


def player_tools():   # the toolchains `porter tools --download` fetched for the player: android-ndk-r27c/, dotnet/, platform-tools/
    return user_dir() / 'toolchains'


def signing_key():   # one key per player: every app they port keeps its signer, so a new port installs over the last one
    return user_dir() / 'signing-key.json'
