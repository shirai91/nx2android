# A port on the device brought to this nx2android checkout's host (python -m porter host …), without the game's files: libdmhost.so is built again from the repository's
# profile (dm/android/build.py, as the port's host stage builds it: a debug build for a debuggable app) and put in place of the installed APK's by
# nxnative/dm/android/apk/apkdriver.py --host (with the overlay's font, assets/font/, which an APK ported before it came lacks), signed with the player's key (the same signer: it installs over the app, its saves stay). The data on the device is not touched.
# The game's rewritten modules on the device (mods/) must still fit the host: mods/build_src.sha256 holds the hashes of the rewriter and the patch files that made them
# (patch_nso.py), and any of them changed since (the rewriter, the SDK's patches, the game's own) needs a port again from the game's files. Other games' patch files do not matter,
# being keyed by their own build IDs. The host is built from the repository's profile, so the port must have been made from it too: its version one the profile registers and the
# hash of the game_profile.h it was built with (nx2a-data.json profile_h, which porter port writes) this checkout's, or this checkout's without defines added since (ADDED_DEFINES) that are at their off value. A derived profile (a version the profile does not register, with
# its automatic alias headroom) or --alias-headroom compiled another DM_GAME_MEM_ALIAS into libdmhost.so, and the repository's would give the game a smaller alias region. Nor does a change in sdk_patches.py for other SDK builds: the cuts and observation gates it and the game's patches.py give the build IDs of
# the port's modules are checked against those mods/<module>.cuts.tsv and .obs.tsv list. A changed shader translator or data pack is not carried over either: a port again does that.
import hashlib, json, os, re, shlex, subprocess, sys, tempfile
from pathlib import Path

from porter import datapack, paths, toolchain
from porter.adb import AdbError
from porter.driver import installed_apk
from porter.paths import DM
from porter.upstream import BuildError, say


def module_tables(cuts_tsv, obs_tsv):
    """A rewritten module's build ID, cuts and observation gates from its mods/<module>.cuts.tsv and .obs.tsv (patch_nso.py). → {'build_id', 'cuts', 'obs'}"""
    bid, rows = '', {'cuts': [], 'obs': []}
    for kind, text in (('cuts', cuts_tsv), ('obs', obs_tsv)):
        for ln in text.splitlines():
            f = ln.rstrip('\r').split('\t')
            if f[0] == '# BUILD_ID' and len(f) > 1: bid = bid or f[1].strip().lower()
            elif len(f) >= 3 and not f[0].startswith('#'): rows[kind].append((int(f[0], 16), f[1].strip().lower(), f[2].strip().lower()))
    return {'build_id': bid, **rows}


def _tables(path):   # CUTS, OBS and FRAME_STEP of a patch file, as patch_nso.py load_patches reads them
    import importlib.util
    spec = importlib.util.spec_from_file_location(f'nx2a_host_{abs(hash(str(path)))}', path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return {k: getattr(m, k, {}) for k in ('CUTS', 'OBS', 'FRAME_STEP')}


def sdk_patches_fit(modules, title_id, extra, tools=DM / 'tools'):
    """True when this checkout's sdk_patches.py, with the game's own patches.py, gives each of the port's modules the cuts and observation gates it was rewritten with.
    modules: module_tables() of every module; extra: the port had patch files outside the repository (--fps60 cuts after the registered ones of main)."""
    if not modules or any(not m['build_id'] for m in modules): return False
    files = [tools / 'sdk_patches.py'] + [p for p in [DM / 'games' / title_id / 'patches.py'] if p.is_file()]
    try: tables = [_tables(p) for p in files]
    except Exception: return False
    for m in modules:
        bid = m['build_id']
        if any(bid in t['FRAME_STEP'] for t in tables[:1]): return False   # a frame step the SDK's patches now give it
        cuts = [(o, ob.lower(), nb.lower()) for t in tables for o, ob, nb, _ in t['CUTS'].get(bid, [])]
        obs = [(o, str(n).lower(), str(w).lower()) for t in tables for o, n, w, _ in t['OBS'].get(bid, [])]
        if m['obs'] != obs: return False
        if m['cuts'] != cuts and not (extra and m['cuts'][:len(cuts)] == cuts): return False
    return True


def stale_sources(record, title_id, tools=DM / 'tools', modules=None):
    """The rewriter's sources that changed since the port: record is mods/build_src.sha256 ("<sha256>  <path from dm/tools>" per line); another game's patches.py is left out,
    and so is a file outside the repository (a --fps60 port's patches.py in its build folder). A changed sdk_patches.py is left out too when modules (module_tables() of the
    port's modules) show it gives them what they were rewritten with (sdk_patches_fit). → [path]"""
    out = []
    for ln in record.splitlines():
        h, _, rel = ln.strip().partition('  ')
        if not h or not rel: continue
        p = (tools / rel).resolve()
        if p.name == 'patches.py' and p.parent.parent == (DM / 'games').resolve() and p.parent.name.upper() != title_id.upper(): continue
        if DM.resolve() not in p.parents: continue
        if not p.is_file() or hashlib.sha256(p.read_bytes()).hexdigest() != h: out.append(rel)
    if 'sdk_patches.py' in out and modules is not None:
        extra = any(DM.resolve() not in (tools / ln.strip().partition('  ')[2]).resolve().parents for ln in record.splitlines() if ln.strip().partition('  ')[2])
        if sdk_patches_fit(modules, title_id, extra, tools): out.remove('sdk_patches.py')
    own = DM / 'games' / title_id / 'patches.py'
    if own.is_file() and not any(Path(ln.strip().partition('  ')[2]).name == 'patches.py' and title_id.upper() in ln.upper() for ln in record.splitlines()):
        out.append(Path(os.path.relpath(own, tools)).as_posix())   # patches registered for the game since the port: its modules on the device lack them
    return out


TITLE_ID = re.compile(r'[0-9A-Fa-f]{16}')


def clean_env():   # the environment without DM_GAMES_DIR / DM_GAMES_FIRST: the host and its profile header come from the repository's profile only
    return {k: v for k, v in os.environ.items() if k not in ('DM_GAMES_DIR', 'DM_GAMES_FIRST')}


def profile_header(title_id):
    """the game_profile.h (bytes) the repository's profile of title_id makes (dm/games/game.py header, as dm/android/build.py makes it)"""
    with tempfile.TemporaryDirectory(prefix='nx2a_profile_') as t:
        h = Path(t) / 'game_profile.h'
        r = subprocess.run([sys.executable, str(DM / 'games/game.py'), 'header', title_id, str(h)], capture_output=True, text=True, errors='replace', env=clean_env())
        if r.returncode or not h.is_file(): raise BuildError(f'{title_id}: the profile header did not build: ' + ((r.stderr.strip() or r.stdout.strip()).splitlines() or ['no output'])[-1])
        return h.read_bytes()


def profile_hash(title_id):
    """sha256 of the game_profile.h the repository's profile of title_id makes"""
    return hashlib.sha256(profile_header(title_id)).hexdigest()


# Header defines added to dm/games/game.py after profile_h began to be recorded, in the order they were added, each with its value when the key is off. They change no layout:
# a port made before one was added and a profile that leaves it off differ only by that line, so check_profile accepts the older hash. A new define that is off by default is appended here.
ADDED_DEFINES = (('DM_GAME_TEX_CPU_UNCACHED', '0'), ('DM_GAME_FPS_CEILING', '0'))


def older_hashes(header):
    """[(names, sha256)] of header without the k most recently added defines (k = 1, 2, ...), as long as each of those is at its default value in header"""
    lines, out = header.splitlines(keepends=True), []
    for k in range(1, len(ADDED_DEFINES) + 1):
        names = ADDED_DEFINES[-k:]
        drop = {f'#define {n} {v}'.encode() for n, v in names}
        if not all(any(ln.strip() == d for ln in lines) for d in drop): break
        out.append(([n for n, _ in names], hashlib.sha256(b''.join(ln for ln in lines if ln.strip() not in drop)).hexdigest()))
    return out


def check_profile(package, manifest, title_id):
    """Refuses (BuildError) a port not made from this checkout's profile of title_id: its version unregistered, or its profile header's hash absent or another (a header that lacks only ADDED_DEFINES at their off value is accepted)"""
    sys.path.insert(0, str(DM / 'games'))
    try: import game
    finally: sys.path.pop(0)
    builds = game.load(title_id).builds()
    ver = manifest.get('version', '')
    if ver not in builds:
        raise BuildError(f'{package}: {ver or "no version"} is not a version the profile of {title_id} registers ({", ".join(builds) or "none"}): the port came from a derived profile '
                         f'with its own alias region; port the game again')
    if not manifest.get('profile_h'):
        raise BuildError(f'{package}: no profile_h in nx2a-data.json (a data pack made before nx2android recorded the profile the host was built from): port the game again')
    header = profile_header(title_id)
    if manifest['profile_h'] == hashlib.sha256(header).hexdigest(): return
    for names, h in older_hashes(header):
        if manifest['profile_h'] == h:
            say(f'{package}: the host on the device was built before {", ".join(names)} existed in the profile header; they are off (default) in this checkout\'s profile of {title_id}, so the update goes on')
            return
    raise BuildError(f'{package}: the host on the device was built from another profile of {title_id} than this checkout\'s (another layout, --alias-headroom or a derived profile): '
                     f'port the game again')


def update(adb, package, jobs=os.cpu_count() or 8):
    """package's libdmhost.so built again from this checkout and installed. → {'package', 'title_id', 'debug', 'host'}"""
    files = datapack.files_folder(package)
    try: manifest = json.loads(adb.read_text(f'{files}/nx2a-data.json') or '{}')
    except ValueError: manifest = {}
    if not isinstance(manifest, dict): manifest = {}
    title_id = manifest.get('title_id', '')
    if not title_id: raise BuildError(f'{package}: no title ID in {files}/nx2a-data.json (a data pack made before nx2android wrote one): port the game again')
    if not isinstance(title_id, str) or not TITLE_ID.fullmatch(title_id): raise BuildError(f'{package}: {title_id!r} in {files}/nx2a-data.json is not a title ID (16 hexadecimal digits)')
    title_id = title_id.upper()
    if not (DM / 'games' / title_id / 'game.tsv').is_file():
        raise BuildError(f'{package}: no profile of {title_id} in this checkout (an experimental port\'s profile stays in its build folder): port the game again')
    check_profile(package, manifest, title_id)
    record = adb.read_text(f'{files}/mods/build_src.sha256')
    if not record.strip(): raise BuildError(f'{package}: no mods/build_src.sha256 on the device: port the game again')
    names = sorted({n.strip().rsplit('/', 1)[-1][:-len('.cuts.tsv')] for n in adb.shell(f'ls {shlex.quote(files)}/mods', check=False).split() if n.strip().endswith('.cuts.tsv')})
    modules = [module_tables(adb.read_text(f'{files}/mods/{n}.cuts.tsv'), adb.read_text(f'{files}/mods/{n}.obs.tsv')) for n in names]
    stale = stale_sources(record, title_id, modules=modules)
    if stale: raise BuildError(f'{package}: the game\'s code on the device was rewritten with other sources than this checkout\'s ({", ".join(stale)}): port the game again from its files')
    debug = 'DEBUGGABLE' in adb.shell(f'dumpsys package {shlex.quote(package)} | grep -m1 flags=', check=False)
    ndk, _ = toolchain.find_ndk()
    if not ndk: raise BuildError('no Android NDK found: python -m porter tools --download')
    with tempfile.TemporaryDirectory(prefix='nx2a_host_') as t:
        say(f'{package}: building the host of {title_id} ({"debug" if debug else "release"} build)')
        r = subprocess.run([sys.executable, str(DM / 'android/build.py'), '--game', title_id, '--ndk', str(ndk), '--out', t, '--lib', '--jobs', str(jobs)] + ([] if debug else ['--release']),
                           capture_output=True, text=True, errors='replace', env=clean_env())
        so = Path(t) / 'libdmhost.so'
        if r.returncode or not so.is_file(): raise BuildError(f'{package}: the host did not build: ' + ((r.stderr.strip() or r.stdout.strip()).splitlines() or ['no output'])[-1])
        src, out = Path(t) / 'installed.apk', Path(t) / 'host.apk'
        adb.run('pull', installed_apk(adb, package), src, timeout=600)
        from porter.pipeline import ui_font_assets   # the overlay's font: an APK ported before it came has none, and the new host looks for it
        fonts = [a for n, f in ui_font_assets().items() for a in ('--asset', f'{n}={f}')]
        r = subprocess.run([sys.executable, str(DM / 'android/apk/apkdriver.py'), str(src), str(out), str(paths.signing_key()), '--host', str(so)] + fonts,
                           capture_output=True, text=True, errors='replace')
        if r.returncode: raise BuildError((r.stderr.strip() or r.stdout.strip()).splitlines()[-1])
        size = so.stat().st_size
        adb.shell(f'am force-stop {shlex.quote(package)}', check=False)
        try: adb.install(out, package)
        except AdbError as e: raise BuildError(str(e)) from e
    say(f'{package}: host of this checkout installed ({size / 2**20:.1f} MB); the game\'s data on the device is unchanged')
    return {'package': package, 'title_id': title_id, 'debug': debug, 'host': size}
