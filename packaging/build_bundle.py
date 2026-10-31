# Build the player's bundle: dist/nx2android/ with nx2android.exe (the app, and the Python the pipeline runs the engine's tools with), the committed
# nxnative/ tree beside it (the engine the port builds from), and the install guide. Zipped as dist/nx2android-<version>-<os>-<cpu>.zip (windows-x64, macos-arm64).
#   python packaging/build_bundle.py      (with PySide6 and PyInstaller installed: pip install -r requirements.txt pyinstaller, and on Windows PyInstaller's
#                                          bootloader compiled on this PC: python packaging/build_bootloader.py)
# The bundle holds no toolchain: the Setup tab downloads NDK r27c, the .NET SDK 10 and platform-tools from their makers, or the player installs them (packaging/INSTALL.md).
import os, platform, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST, WORK = ROOT / 'dist', ROOT / 'build/pyinstaller'
APP = DIST / 'nx2android'
PLATFORM = {'win32': 'windows', 'darwin': 'macos'}.get(sys.platform, 'linux') + '-' + ('arm64' if platform.machine().lower() in ('arm64', 'aarch64') else 'x64')


def tracked(prefix):
    return [p for p in subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z', prefix], capture_output=True, text=True, check=True).stdout.split('\0') if p]


def main():
    if os.name == 'nt':   # the prebuilt launcher is flagged by Windows antivirus engines only
        sys.path.insert(0, str(ROOT / 'packaging'))
        import build_bootloader
        if not build_bootloader.is_local(): sys.exit('PyInstaller has its prebuilt bootloader, which antivirus engines flag: run python packaging/build_bootloader.py first')
    subprocess.run([sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--distpath', str(DIST), '--workpath', str(WORK), str(ROOT / 'packaging/nx2android.spec')], check=True)
    files = tracked('nxnative')   # the committed engine and profiles only: no local builds, keys or game files
    for f in files:
        d = APP / f; d.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(ROOT / f, d)
    for src, dst in (('packaging/INSTALL.md', 'INSTALL.md'), ('README.md', 'README.md'), ('docs/ARCHITECTURE.md', 'docs/ARCHITECTURE.md'), ('docs/DEVELOPMENT_GUIDE.md', 'docs/DEVELOPMENT_GUIDE.md'), ('nxnative/LICENSE', 'LICENSE')):
        (APP / dst).parent.mkdir(parents=True, exist_ok=True); shutil.copy2(ROOT / src, APP / dst)
    shutil.copytree(ROOT / 'docs/images', APP / 'docs/images', dirs_exist_ok=True)   # the README's games table
    ver = subprocess.run(['git', '-C', str(ROOT), 'describe', '--always', '--dirty'], capture_output=True, text=True).stdout.strip() or 'dev'
    (APP / 'VERSION').write_text(ver + '\n', encoding='utf-8')   # which commit the bundle was built from
    z = DIST / f'nx2android-{ver}-{PLATFORM}.zip'; z.unlink(missing_ok=True)
    if sys.platform == 'darwin': subprocess.run(['ditto', '-c', '-k', '--keepParent', str(APP), str(z)], check=True)   # keeps the Qt frameworks' symlinks, which zipfile copies as files
    else: shutil.make_archive(str(z.with_suffix('')), 'zip', DIST, 'nx2android')
    print(f'{APP} ({len(files)} nxnative files)\n{z} ({z.stat().st_size / 2**20:.0f} MB)')


if __name__ == '__main__':
    main()
