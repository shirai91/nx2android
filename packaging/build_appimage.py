# Pack the Linux bundle (dist/nx2android/, which packaging/build_bundle.py makes first) as one AppImage: dist/nx2android-<version>-linux-<cpu>.AppImage (issue #440).
#   python packaging/build_bundle.py && python packaging/build_appimage.py      (on Linux, x64 or arm64)
# The AppImage starts the same executable as the zip: with no arguments the app, with -m porter … the command line. Mounted read-only at a new path each
# launch, it runs the engine from a copy in the user folder (porter/paths.py engine_copy). appimagetool and the AppImage runtime are downloaded at pinned
# versions and checked by sha256 (build/appimage/); the runtime mounts with the system's FUSE 3 and needs no libfuse2.
import hashlib, os, platform, shutil, subprocess, sys, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP, WORK = ROOT / 'dist/nx2android', ROOT / 'build/appimage'
ARCH, CPU = ('aarch64', 'arm64') if platform.machine().lower() in ('arm64', 'aarch64') else ('x86_64', 'x64')
TOOL = ('https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-{}.AppImage',
        {'x86_64': 'ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0', 'aarch64': 'f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158'})
RUNTIME = ('https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-{}',
           {'x86_64': '2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d', 'aarch64': '00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444'})
APPRUN = '#!/bin/sh\nHERE="$(dirname "$(readlink -f "$0")")"\nexec "$HERE/nx2android" "$@"\n'
DESKTOP = '[Desktop Entry]\nType=Application\nName=nx2android\nComment=Port your own Nintendo Switch games to Android\nExec=nx2android\nIcon=nx2android\nCategories=Utility;\nTerminal=false\n'


def fetch(url, sha):   # a pinned download, kept in build/appimage/ for the next run
    f = WORK / url.rsplit('/', 1)[1]
    if not (f.is_file() and hashlib.sha256(f.read_bytes()).hexdigest() == sha):
        print(f'downloading {url}', flush=True)
        with urllib.request.urlopen(url, timeout=120) as r: data = r.read()
        if hashlib.sha256(data).hexdigest() != sha: sys.exit(f'{url}: sha256 differs from the pinned one')
        f.write_bytes(data)
    f.chmod(0o755)
    return f


def main():
    if not sys.platform.startswith('linux'): sys.exit('an AppImage is built on Linux')
    if not (APP / 'nx2android').is_file(): sys.exit(f'{APP} has no nx2android: run python packaging/build_bundle.py first')
    WORK.mkdir(parents=True, exist_ok=True)
    tool, runtime = fetch(TOOL[0].format(ARCH), TOOL[1][ARCH]), fetch(RUNTIME[0].format(ARCH), RUNTIME[1][ARCH])
    appdir = WORK / 'nx2android.AppDir'; shutil.rmtree(appdir, ignore_errors=True)
    shutil.copytree(APP, appdir, symlinks=True)   # the bundle as it is, with its symlinks and executable marks
    (appdir / 'AppRun').write_text(APPRUN); (appdir / 'AppRun').chmod(0o755)
    (appdir / 'nx2android.desktop').write_text(DESKTOP)
    shutil.copy2(ROOT / 'gui/icon.png', appdir / 'nx2android.png'); (appdir / '.DirIcon').symlink_to('nx2android.png')
    ver = (APP / 'VERSION').read_text().strip()
    out = ROOT / f'dist/nx2android-{ver}-linux-{CPU}.AppImage'; out.unlink(missing_ok=True)
    # appimagetool is an AppImage too: extracted and run, it needs no FUSE on the build machine
    subprocess.run([str(tool), '--no-appstream', '--runtime-file', str(runtime), str(appdir), str(out)], check=True,
                   env=dict(os.environ, ARCH=ARCH, APPIMAGE_EXTRACT_AND_RUN='1'))
    print(f'{out} ({out.stat().st_size / 2**20:.0f} MB)')


if __name__ == '__main__':
    main()
