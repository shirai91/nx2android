# The tools a port needs on the player's machine, found and checked (`porter doctor`, the GUI's setup screen). Nothing here is shipped with the tool:
# the player installs the Android NDK (r27c), the .NET SDK (10) and, to install from the tool, platform-tools (adb), or has `porter tools --download` (porter.fetch)
# fetch them from Google and Microsoft into their nx2android folder (paths.player_tools()), which the finders here also search.
import glob, importlib.util, os, re, shutil, subprocess, sys
from dataclasses import dataclass, field
from pathlib import Path

from porter import config
from porter.paths import LOCAL_TOOLS, player_tools

NDK_REVISION = '27.2.12479018'   # r27c: the NDK the Android host is built and tested with
NDK_RELEASE = 'r27c'
DOTNET_MAJOR = 10
PY_MODULES = {'lz4.block': 'lz4', 'capstone': 'capstone'}   # the rewriter (capstone) and the NSO reader (lz4)
EXE = '.exe' if os.name == 'nt' else ''
DISK_GB = 20   # the setup check's free space: a NEO-sized port (7 GB of build folder) with room to spare


class ToolError(Exception):
    pass


@dataclass
class Check:
    name: str
    ok: bool
    detail: str
    required: bool = True
    path: str = ''
    fix: str = ''

    def as_dict(self): return dict(self.__dict__)


def _sdk_roots():   # Android SDK folders that may hold ndk/<version> and platform-tools
    out = [os.environ.get(k) for k in ('ANDROID_HOME', 'ANDROID_SDK_ROOT')]
    if os.name == 'nt': out.append(os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk'))
    elif sys.platform == 'darwin': out.append(os.path.expanduser('~/Library/Android/sdk'))
    else: out.append(os.path.expanduser('~/Android/Sdk'))
    return [Path(p) for p in out if p]


def ndk_revision(d):   # Pkg.Revision from an NDK folder's source.properties, None when it is not an NDK
    try: t = (Path(d) / 'source.properties').read_text(errors='replace')
    except OSError: return None
    m = re.search(r'Pkg\.Revision\s*=\s*([\d.]+)', t)
    return m.group(1) if m else None


def find_ndk(cfg=None):
    """(folder, revision) of the NDK to build with: the configured one, ANDROID_NDK_HOME / ANDROID_NDK_ROOT, then the usual install places; r27c is preferred when several are found"""
    cfg = cfg if cfg is not None else config.load()
    given = [cfg.get('ndk'), os.environ.get('ANDROID_NDK_HOME'), os.environ.get('ANDROID_NDK_ROOT')]
    for d in filter(None, given):   # an explicit choice is taken as it is, even when it is not r27c (the check says so)
        if ndk_revision(d): return Path(d), ndk_revision(d)
    found = [Path(p) for t in (LOCAL_TOOLS, player_tools()) for p in glob.glob(str(t / 'android-ndk-*'))] + [p for r in _sdk_roots() for p in sorted((r / 'ndk').glob('*'))] + [r / 'ndk-bundle' for r in _sdk_roots()]
    found = [(d, ndk_revision(d)) for d in found if ndk_revision(d)]
    if not found: return None, None
    return next((x for x in found if x[1] == NDK_REVISION), found[-1])


def find_dotnet(cfg=None):
    """The dotnet to use: the configured one as it is; otherwise the first of DOTNET_ROOT, PATH and the usual places that has SDK 10 (a runtime-only dotnet, as Windows
    installs for apps, cannot build nx_extract), else the first found"""
    cfg = cfg if cfg is not None else config.load()
    if cfg.get('dotnet') and Path(cfg['dotnet']).is_file(): return Path(cfg['dotnet'])
    progs = [os.environ.get(k) for k in ('ProgramFiles', 'ProgramW6432')] if os.name == 'nt' else []
    cands = [os.environ.get('DOTNET_ROOT') and str(Path(os.environ['DOTNET_ROOT']) / f'dotnet{EXE}'), shutil.which('dotnet'), str(LOCAL_TOOLS / 'dotnet' / f'dotnet{EXE}'), str(player_tools() / 'dotnet' / f'dotnet{EXE}'),
             str(Path.home() / '.dotnet' / f'dotnet{EXE}'), *[str(Path(p) / 'dotnet' / f'dotnet{EXE}') for p in progs if p], '/usr/local/share/dotnet/dotnet']
    found = list(dict.fromkeys(Path(c) for c in cands if c and Path(c).is_file()))
    return next((d for d in found if any(int(s.split('.')[0]) >= DOTNET_MAJOR for s in dotnet_sdks(d))), found[0] if found else None)


def dotnet_sdks(dotnet):   # ['10.0.402', ...]
    try: r = subprocess.run([str(dotnet), '--list-sdks'], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired): return []
    return re.findall(r'^(\d+\.\d+\.\d+)', r.stdout, re.M)


def dotnet_root(dotnet):
    """The folder holding dotnet's runtime (host/, shared/, sdk/), which an apphost such as nx_extract's looks for in DOTNET_ROOT: the parent of the SDK folder
    `--list-sdks` prints ("10.0.403 [/opt/homebrew/Cellar/dotnet/10.0.403/libexec/sdk]"). Not the executable's folder: Homebrew's dotnet is a wrapper script in
    /opt/homebrew/bin that sets DOTNET_ROOT to libexec itself, and with DOTNET_ROOT=/opt/homebrew/bin nx_extract found no runtime ("You must install .NET")"""
    try: r = subprocess.run([str(dotnet), '--list-sdks'], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired): r = None
    m = r and re.search(r'^\d+\.\d+\.\d+\S* \[(.+)\]\s*$', r.stdout, re.M)
    return Path(m.group(1)).parent if m else Path(dotnet).resolve().parent


def find_adb(cfg=None):
    cfg = cfg if cfg is not None else config.load()
    for c in [cfg.get('adb'), shutil.which('adb'), *[str(r / 'platform-tools' / f'adb{EXE}') for r in _sdk_roots()], str(LOCAL_TOOLS / 'platform-tools' / f'adb{EXE}'), str(player_tools() / 'platform-tools' / f'adb{EXE}')]:
        if c and Path(c).is_file(): return Path(c)
    return None


def _importable(m):
    try: return importlib.util.find_spec(m) is not None
    except (ImportError, ValueError): return False


def checks(cfg=None, keys=None):
    """Every requirement, as Check rows (the order the setup screen shows them)"""
    cfg = cfg if cfg is not None else config.load()
    out = []
    v = sys.version_info
    out.append(Check('Python', v >= (3, 11), f'{v.major}.{v.minor}.{v.micro} ({sys.executable})', fix='Python 3.11 or later'))
    miss = [pip for m, pip in PY_MODULES.items() if not _importable(m)]
    if not (_importable('compression.zstd') or _importable('backports.zstd')): miss.append('backports.zstd')   # zstd: in the standard library from Python 3.14
    out.append(Check('Python modules', not miss, 'missing: ' + ', '.join(miss) if miss else 'lz4, capstone, zstd', fix='pip install ' + ' '.join(miss) if miss else ''))
    out.append(Check('Pillow', _importable('PIL'), 'the app icon from the game\'s icon' if _importable('PIL') else 'missing: the app gets no icon', required=False, fix='pip install pillow'))
    aes = _importable('Crypto.Cipher') or _importable('Cryptodome.Cipher')
    out.append(Check('pycryptodome', aes, 'NSZ / XCZ dumps (porter.dumps)' if aes else 'missing: NSZ / XCZ dumps must be decompressed to NSP / XCI first', required=False, fix='' if aes else 'pip install pycryptodome'))
    out.append(Check('fontTools', _importable('fontTools'), 'the button icons in game text' if _importable('fontTools') else 'missing: button icons in game text draw as "?"', required=False, fix='pip install fonttools'))
    ndk, rev = find_ndk(cfg)
    if not ndk: out.append(Check('Android NDK', False, 'not found', fix=f'Download missing tools, or install NDK {NDK_RELEASE} (developer.android.com/ndk/downloads) and set its folder'))
    else: out.append(Check('Android NDK', rev == NDK_REVISION, f'{rev} at {ndk}' + ('' if rev == NDK_REVISION else f': the host is built with {NDK_RELEASE} ({NDK_REVISION}); others are untested'),
                           required=rev.split('.')[0] == NDK_REVISION.split('.')[0], path=str(ndk), fix='' if rev == NDK_REVISION else f'install NDK {NDK_RELEASE}'))
    dn = find_dotnet(cfg)
    sdks = dotnet_sdks(dn) if dn else []
    good = [s for s in sdks if int(s.split('.')[0]) >= DOTNET_MAJOR]
    out.append(Check('.NET SDK', bool(good), (f'{", ".join(sdks) or "no SDK"} ({dn})' if dn else 'dotnet not found'), path=str(dn or ''), fix='' if good else f'Download missing tools, or install the .NET SDK {DOTNET_MAJOR} (dotnet.microsoft.com)'))
    adb = find_adb(cfg)
    out.append(Check('adb', bool(adb), str(adb) if adb else 'not found: the tool cannot install on a device (the APK and data pack are still made)', required=False, path=str(adb or ''),
                     fix='' if adb else 'Download missing tools, or install Android platform-tools'))
    k = keys or cfg.get('keys')
    out.append(Check('prod.keys', bool(k) and Path(k).is_file(), k if k and Path(k).is_file() else (f'no such file: {k}' if k else 'not set'), path=k or '',
                     fix='' if k and Path(k).is_file() else 'dump prod.keys from your own Switch and set its path'))
    where = Path(cfg.get('out') or Path.home())   # the output folder's drive, or the home folder's (a port's build folder goes beside the game files: the port checks that drive itself)
    where = next((p for p in (where, *where.parents) if p.exists()), Path.home())
    free = shutil.disk_usage(where).free / 2**30
    out.append(Check('Disk space', free >= DISK_GB, f'{free:.0f} GB free on {where.anchor}', required=False,
                     fix='' if free >= DISK_GB else f'a port takes about twice its game files on the game folder\'s drive (NEO: 7 GB); keep {DISK_GB} GB free'))
    return out


def require(cfg=None, keys=None, adb=False):
    """The checked tools for a port: {'ndk', 'dotnet', 'adb'}; ToolError naming every missing requirement"""
    rows = checks(cfg, keys)
    bad = [r for r in rows if not r.ok and (r.required or (adb and r.name == 'adb'))]
    if bad: raise ToolError('missing requirements: ' + '; '.join(f'{r.name}: {r.detail}' + (f' ({r.fix})' if r.fix else '') for r in bad))
    by = {r.name: r for r in rows}
    return {'ndk': Path(by['Android NDK'].path), 'dotnet': Path(by['.NET SDK'].path), 'adb': Path(by['adb'].path) if by['adb'].ok else None}
