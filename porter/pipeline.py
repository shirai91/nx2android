# The port: a game folder (NSP / XCI) + prod.keys + the game's profile → <App>.apk and <App>-data/ (docs/ARCHITECTURE.md "The port").
# The steps every platform shares are upstream's (porter.upstream.nsp: scan, extraction, the shader split, stage reuse, progress); the Android ones are here:
# the rewrite for DM_TARGET=android, the Vulkan shader bundle, the host built with the player's NDK, the APK, the data pack and the install.
# Work files go where upstream's NSP script puts them, <game folder>/<title ID>_build/ (extract/, aoc/, shader/, logs/ are shared with an iOS build of the same folder),
# with the Android ones in its android/ folder.
import os, re, shutil, sys
from dataclasses import dataclass, field
from pathlib import Path

from porter import config, datapack, fpslocker, install, paths, profiles, toolchain
from porter.adb import Adb, AdbError
from porter.paths import DM, NXNATIVE
from porter.upstream import BuildError, PROG, android_profile, game, nacp, nsp, profile_guess, say

PY = sys.executable
# The letters of the button-icon font (make_ext_font.py knocks them out of the icons): a bold system font of the machine that ports, never shipped in the repository
LETTER_FONTS = [f'{os.environ.get("WINDIR", "C:/Windows")}/Fonts/{f}' for f in ('ARLRDBD.TTF', 'arialbd.ttf')] + [
    '/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', '/usr/share/fonts/TTF/DejaVuSans-Bold.ttf']


@dataclass
class Options:
    folder: Path                       # the game's folder: the base game, its updates and DLC
    keys: Path = None                  # prod.keys
    out: Path = None                   # where the APK and the data pack go (default: the game folder)
    version: str = ''                  # v<N> to pack (default: the newest in the folder)
    language: str = ''                 # a code or a name (default: English)
    app_name: str = ''                 # the launcher label (default: the game's name in its control data)
    install: bool = False              # install on the device after packing
    device: str = ''                   # adb serial
    replace_signer: bool = False       # uninstall an app of the same package signed with another key
    debug: bool = False                # a development build: debug information, debuggable, DM_CTL
    experimental: bool = False         # a game not ported to Android yet: a provisional profile from upstream's survey (or a copy of its iOS profile), status experimental
    package: str = ''                  # the app's package instead of the profile's (a test install beside another build of the game)
    fps60: bool = False                # 60 fps: the profile's own frame-rate cap, else the build's FPSLocker-Warehouse config (porter/fpslocker.py)
    jobs: int = field(default_factory=lambda: os.cpu_count() or 8)


def app_stem(g, name):   # the file name of the APK and the data pack: the profile's iOS IPA name without .ipa (NeoTheWorldEndsWithYou)
    ipa = g.get('ipa') or nsp.ipa_name(name, g.title_id)
    return ipa[:-4] if ipa.endswith('.ipa') else ipa


def version_code(ver):   # the APK's versionCode: the Switch version number (v196608 → 196609), so an update of the game installs over the base game's port
    return nsp.vnum(ver) + 1


def inspect(folder):
    """What a game folder holds, without extracting anything: the GUI's game card and `porter inspect`"""
    try: app, fam = nsp.scan_nsps(Path(folder))
    except BuildError:
        if any(Path(folder).glob('*.ns[zZ]')) or any(Path(folder).glob('*.xc[zZ]')):   # how dumps are often shared: zstd-compressed NCAs the extractor does not read
            raise BuildError('this folder has only NSZ / XCZ files: decompress them to NSP / XCI first (nsz -D with your prod.keys), then choose the folder with the NSP files') from None
        raise
    g, ap = profiles.by_title(app)
    info = dict(title_id=f'{app:016X}', base=fam['base'].name, updates=[{'version': f'v{v}' if v is not None else None, 'file': p.name} for v, p in fam['updates']],
                dlc=[{'title_id': f'{t:016X}', 'file': p.name} for t, _, p in fam['dlc']], profile=None, android=None, versions=[], pack=None, supported=False, reason='')
    info['experimental'] = not (g and ap)   # a port can still be tried: porter port --experimental
    if not g:
        info['reason'] = f'no game profile for {app:016X}: this game is not supported yet (an experimental port surveys it and guesses a profile; it may not start)'
        return info
    builds = g.builds()
    have = nsp.nsp_versions(fam, builds)
    pack = max(have, key=nsp.vnum)
    info.update(profile=g.id, name=g.get('name', g.id), versions=sorted(have, key=nsp.vnum), pack=pack,
                registered={v: b.get('display', '') for v, b in builds.items()}, derived=pack not in builds)
    if not ap:
        info['reason'] = f'{g.get("name", g.id)} has a game profile (iOS) but is not ported to Android yet (an experimental port tries it with that profile)'
        return info
    info.update(android=profiles.entry(g, ap).as_dict(), supported=True)
    if ap.status == 'wip': info['reason'] = f'{g.get("name", g.id)} is a work in progress: it plays, but parts of it are missing (docs/PORT_STATUS.md says which)'
    if pack not in builds:
        info['reason'] = (info['reason'] + '; ' if info['reason'] else '') + f'{pack} is not a version the profile was brought up on ({", ".join(builds)}): it is packed from a derived profile, without that version\'s fixes'
    return info


class Port:
    def __init__(self, opts):
        self.o = opts
        self.cfg = config.load()
        self.result = {}

    # ---- the whole port ----
    def run(self):
        o = self.o
        o.folder = Path(o.folder).expanduser().resolve()
        if not o.folder.is_dir(): raise BuildError(f'not a folder: {o.folder}')
        keys = o.keys or self.cfg.get('keys')
        if not keys: raise BuildError('no prod.keys: give -k / --keys, or set it once with `porter config keys <path>`')
        o.keys = Path(keys).expanduser().resolve()
        try: self.tools = toolchain.require(self.cfg, str(o.keys), adb=o.install)
        except toolchain.ToolError as e: raise BuildError(str(e)) from e
        o.out = Path(o.out or self.cfg.get('out') or o.folder).expanduser().resolve()
        # the chosen dotnet first on PATH, in this process too: Windows finds a program by the parent's PATH, not the one passed to the child (upstream runs `dotnet` by name)
        os.environ['PATH'] = os.pathsep.join([str(self.tools['dotnet'].parent), os.environ.get('PATH', '')])
        os.environ['DOTNET_ROOT'] = str(self.tools['dotnet'].parent)   # where nx_extract.exe's apphost looks for the runtime (else the system's, which may lack .NET 10)
        # PYTHONUTF8: the engine's tools write their tables with open() and no encoding, which is the ANSI code page on Windows (shader_split.py's "→" failed)
        self.env = dict(os.environ, J=str(o.jobs), PYTHONIOENCODING='utf-8', PYTHONUTF8='1', DOTNET_CLI_TELEMETRY_OPTOUT='1')

        self.inputs()
        PROG.plan(1 + len(self.versions) + bool(self.fam['dlc']) + bool(o.fps60) + 6 + bool(o.install))   # nx_extract, the versions, the DLC, 60 fps, rewrite / split / Vulkan shaders / host / APK / data, install
        self.extract()
        self.check_graphics()
        self.fps_unlock()
        self.rewrite()
        if o.install: self.pull_captures()
        self.shaders()
        self.host()
        self.apk()
        self.data()
        if o.install: self.install()
        nsp.rmtree(self.work / 'tmp')
        self.result.update(title_id=self.g.id, name=self.app_name, package=self.package, version=self.pkg_ver, display=self.display, language=self.lang,
                           status=self.ap.status, derived=bool(self.derived), installed=bool(o.install))
        say(f'done: {self.result["apk"]} and {self.result["data"]}')
        if self.derived: say(f'[RED] {self.derived}: the port is built from a derived profile and may not run as the profile\'s tested version does')
        return self.result

    # ---- inputs: the folder's games and versions, the profile, the version to pack ----
    def inputs(self):
        o = self.o
        say(f'game folder: {o.folder}')
        self.app, self.fam = nsp.scan_nsps(o.folder)
        tid = f'{self.app:016X}'
        self.work = o.folder / f'{tid}_build'
        g, ap = profiles.by_title(self.app)
        self.provisional = False
        if not (g and ap):
            what = f'no game profile for {tid}' if not g else f'{g.get("name", g.id)} ({g.id}) has a game profile but is not ported to Android yet (no android.tsv in {g.dir})'
            if not o.experimental: raise BuildError(f'{what}: --experimental tries it anyway (profiles: `porter profiles`)')
            g, ap = self.experimental_profile(g, tid)
        self.g, self.ap = g, ap
        self.package = o.package or ap.package
        if not re.fullmatch(r'[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+', self.package): raise BuildError(f'not a package name: {self.package}')
        say(f'game profile: {g.id} {g.get("name", "")} (Android: {ap.status}' + (f', tested on {ap.tested}' if ap.tested else '') + ')')
        builds, need = g.builds(), g.words('shader_versions')
        have = nsp.nsp_versions(self.fam, builds)
        self.derived = ''
        if self.provisional:   # the version the survey saw
            self.pkg_ver = o.version or g.get('package')
            if self.pkg_ver not in builds: raise BuildError(f'the version to pack must be one the provisional profile registers: {", ".join(builds)}')
        else:
            self.pkg_ver = o.version or max(have, key=nsp.vnum)
            if self.pkg_ver not in have: raise BuildError(f'the version to pack ({self.pkg_ver}) is not in the folder, which holds {", ".join(sorted(have, key=nsp.vnum))}')
            if self.pkg_ver not in builds: self.derived = f'{self.pkg_ver} is not a version game profile {g.id} registers ({", ".join(f"{v} {b["display"]}" for v, b in builds.items())})'
            elif not all(v in have for v in need): self.derived = f'the folder has no {", ".join(v for v in need if v not in have)}, whose files the profile\'s shader tables were made from'
            if self.derived: say(f'{self.derived}: packing {self.pkg_ver} from a profile derived from it')
        self.versions = [self.pkg_ver] if self.derived else need + ([self.pkg_ver] if self.pkg_ver not in need else [])
        self.have = have
        self.check_space()
        self.aw = self.work / 'android'; self.logs = self.work / 'logs'
        for d in (self.aw, self.logs, self.work / 'tmp'): d.mkdir(parents=True, exist_ok=True)
        # DM_GAME picks the profile for every tool; WORK keeps their temporary folders inside the build folder (the shader translator writes thousands of files)
        self.env.update(DM_GAME=g.id, DM_ROOT=str(self.work), WORK=str(self.work / 'tmp'))

    def check_space(self):
        """Stop before extracting when the game folder's drive cannot hold the port: a build folder takes about 1 to 2 times the game files (NEO 7.0 GB of 6.8,
        Viractal 2.7 of 1.4), less what an earlier port left there, plus a margin"""
        game = sum(f.stat().st_size for f in self.o.folder.iterdir() if f.is_file() and f.suffix.lower() in ('.nsp', '.xci'))
        built = sum(f.stat().st_size for f in self.work.rglob('*') if f.is_file()) if self.work.is_dir() else 0
        need, free = max(0, 2 * game - built) + 2**30, shutil.disk_usage(self.o.folder).free
        if free < need: raise BuildError(f'not enough disk space on {self.o.folder.anchor}: the port needs about {need / 2**30:.1f} GB more, {free / 2**30:.1f} GB is free')

    def use_profile_dir(self, pdir, first):   # every tool finds the game's profile in pdir (game.py DM_GAMES_DIR; first: before dm/games)
        dirs = [str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)]
        self.env['DM_GAMES_DIR'] = os.environ['DM_GAMES_DIR'] = os.pathsep.join(dirs)
        if first: self.env['DM_GAMES_FIRST'] = os.environ['DM_GAMES_FIRST'] = '1'

    def experimental_profile(self, g, tid):
        """A profile for a game not ported to Android, in <build folder>/profile/<id>/ (outside the repository): upstream's survey and provisional profile when the game has
        none (scripts/build_ipa_from_nsp.py survey / write_profile, dm/games/profile_guess.py), or a copy of its iOS profile; plus an android.tsv with status experimental.
        A game brought up this way gets its profile into dm/games by hand (docs/ARCHITECTURE.md "Adding a game")"""
        pdir = self.work / 'profile'; d = pdir / tid
        if g is None:
            if (d / 'game.tsv').is_file(): say(f'no game profile for {tid}: reusing the provisional profile from the last run ({d}; delete it to survey again)')
            else:
                facts = nsp.survey(self.o.folder, self.app, self.fam, self.o.keys, NXNATIVE, PY, self.env)
                facts['survey'] = f'<NSP folder>/{tid}_survey/survey.md'
                nsp.write_profile(d, self.app, facts)
            self.provisional = True
            say('[RED] provisional game profile: values guessed from the survey, the game may not start')
        elif not (d / 'game.tsv').is_file() or (g.dir / 'game.tsv').stat().st_mtime > (d / 'game.tsv').stat().st_mtime:   # the iOS profile as it is now
            nsp.rmtree(d); shutil.copytree(g.dir, d, ignore=shutil.ignore_patterns('patches.py', '__pycache__'))   # patches.py stays in dm/games, where patch_nso.py finds it
        if not (d / 'android.tsv').is_file():
            (d / 'android.tsv').write_text(f'# Written by porter for an experimental port: the game has not been brought up on Android\nstatus\texperimental\n'
                                          f'package\torg.nx2a.t{tid.lower()}\norientation\tlandscape\n', encoding='utf-8')
        self.use_profile_dir(pdir, first=True)
        g = game.load(tid)
        return g, android_profile.AndroidProfile(g)

    # ---- extract: nx_extract, each version the shaders come from, the DLC ----
    def extract(self):
        o, g = self.o, self.g
        picks = nsp.pick_nsps(self.fam, self.have, self.versions)
        ex = self.work / 'extract'; ex.mkdir(parents=True, exist_ok=True)
        need = sum({p.stat().st_size for b, u in picks.values() for p in (b, u) if p}) / 2**30 + 6
        if nsp.free_gb(self.work) < need and not all((ex / v).exists() for v in self.versions): raise BuildError(f'less than {need:.0f} GB free on the volume of {self.work}')
        PROG.stage('build nx_extract')
        nx = nsp.build_nx_extract(NXNATIVE, self.work, self.logs, self.env)
        PROG.end()
        self.dirs = {ver: nsp.extract(nx, o.keys, ex, ver, *picks[ver], None, self.logs, g) for ver in self.versions}
        bad = [] if self.derived else [f'{v}: {x}' for v in self.versions for x in nsp.build_mismatch(self.dirs[v], v, g)]
        if bad and self.provisional: raise BuildError(f'not the builds the provisional profile registers: {"; ".join(bad)}')
        if bad:
            self.derived = f'not the builds game profile {g.id} registers: {"; ".join(bad)}'
            say(f'{self.derived}: packing {self.pkg_ver} from a profile derived from it')
            self.versions, self.dirs = [self.pkg_ver], {self.pkg_ver: self.dirs[self.pkg_ver]}
        self.game = self.dirs[self.pkg_ver]
        if self.derived:   # the derived profile (android.tsv included) goes into the build folder, and every tool takes it first
            pdir = self.work / 'profile-derived'   # not profile/: an experimental port's copy there is the profile it derives from
            disp = nsp.derive_profile(g, pdir / g.id, self.pkg_ver, self.game, g.words('modules'))
            self.use_profile_dir(pdir, first=True)
            self.g = game.load(g.id); self.ap = android_profile.AndroidProfile(self.g)
            say(f'derived game profile for {self.pkg_ver} ({disp}): {self.g.dir}')
        self.aoc = nsp.extract_aoc(nx, o.keys, NXNATIVE, self.work, self.app, self.fam, self.logs)
        self.base = next((self.dirs[v] for v in self.versions if picks[v][1] is None), self.dirs[self.versions[0]])
        self.app_name = o.app_name or nacp.name(self.base / 'control.nacp') or self.g.get('name', self.g.id)
        self.display = nacp.version(self.game / 'control.nacp') or nacp.version(self.base / 'control.nacp') or '1.0'
        lang_in = o.language or self.cfg.get('language')
        want = nsp.language_code(lang_in) if lang_in else 'en-US'
        if not want: raise BuildError(f'unknown game language {lang_in!r}')
        have = nacp.languages(self.game / 'control.nacp') or nacp.languages(self.base / 'control.nacp')
        self.lang = nsp.game_language(want, bool(lang_in), have)
        say(f'app: {self.app_name} {self.display} ({self.package}), game language {self.lang}')

    def check_graphics(self):
        """What the Android host can run: NVN games whose shaders are all in their files. The other kinds need a step upstream runs on a Mac (booting the game in the Mac host)
        or a graphics layer the Android host does not have yet"""
        src = profile_guess.build_sources(self.game / 'ExeFS', self.game / 'RomFS', self.g.words('modules'))
        gfx = src['graphics'][0]
        if gfx != 'nvn': raise BuildError(f'{self.app_name} renders with {gfx} ({src["graphics"][1]}): the Android host replays NVN only so far')
        for k in ('glslc_capture', 'ue_decode', 'oodle_packs', 'ue_legacy'):
            if src[k][0]: raise BuildError(f'{self.app_name} needs a step that boots the game in the Mac host ({src[k][1]}), which a Windows port cannot run yet')

    # ---- 60 fps (--fps60): the profile's own cap (fps_target: frame-step stubs or hle_fpslock.c's rows), else the community's FPSLocker config of the build ----
    def fps_unlock(self):
        self.fps_env, self.fps_files = [], {}
        self.env.pop('DM_PATCHES_EXTRA', None)
        if not self.o.fps60: return
        PROG.stage('60 fps', step='the game profile')
        if self.g.get('fps_target', '0') != '0':
            self.fps_env = ['DM_FPS_TARGET=60']; PROG.end('the game profile\'s own frame-rate cap (fps_target), at 60'); return
        img, bid = fpslocker.nso_image(self.game / 'ExeFS/main')
        src = fpslocker.url(self.g.id, bid); d = self.aw / 'fpslock'; d.mkdir(exist_ok=True); cache = d / f'{bid[:16].upper()}.yaml'
        PROG.set(step='fetching the FPSLocker-Warehouse config')
        try: text = fpslocker.fetch(self.g.id, bid)
        except OSError as e:
            if not cache.is_file(): say(f'[RED] 60 fps: {src} could not be fetched ({e}): the game keeps its own frame rate'); PROG.end('not fetched: the game keeps its own frame rate'); return
            text = cache.read_text(encoding='utf-8'); say(f'[RED] 60 fps: {src} could not be fetched ({e}): using the copy fetched before')
        if text is None:
            nsp.rmtree(cache); say(f'60 fps: FPSLocker-Warehouse has no config for main build {bid[:16].upper()}: the game keeps its own frame rate'); PROG.end('no config for this build'); return
        cache.write_text(text, encoding='utf-8')
        try: tsv, cuts = fpslocker.convert(text, img, bid, src)
        except fpslocker.Refused as e: say(f'[RED] 60 fps: {src} cannot be applied ({e}): the game keeps its own frame rate'); PROG.end('config not applicable: the game keeps its own frame rate'); return
        (d / 'fpslock.tsv').write_text(tsv, encoding='utf-8', newline='\n')
        if cuts:   # code bytes: the rewriter's cuts, applied before it rewrites main (patch_nso.py DM_PATCHES_EXTRA; part of the rewrite's reuse key)
            (d / 'patches.py').write_text(fpslocker.patches_py(bid, cuts, src), encoding='utf-8', newline='\n'); self.env['DM_PATCHES_EXTRA'] = str(d / 'patches.py')
        # FPSLocker paces the presents for its target itself, whatever interval the game asks for, so its configs leave the game's interval alone: presents follow the display (present_interval display)
        self.fps_env = ['DM_FPS_TARGET=60', f'DM_FPS_LOCKS={datapack.files_folder(self.package)}/fpslock.tsv', 'DM_PRESENT_INTERVAL=display']
        self.fps_files = {'fpslock.tsv': d / 'fpslock.tsv'}
        n = tsv.count('\n') - 3
        say(f'60 fps: FPSLocker-Warehouse config {src}, by masagrator and contributors: {n} memory write(s)' + (f', {len(cuts)} code cut(s)' if cuts else ''))
        PROG.end(f'{n} memory write(s)' + (f', {len(cuts)} code cut(s)' if cuts else ''))

    # ---- rewrite: patch_nso.py for the Android host's thread slots ----
    def rewrite(self):
        self.patched = self.aw / f'patched_{self.pkg_ver}'   # beside, not over, an iOS build's patched_<version>: the rewrite's reuse key does not include the target
        nsp.rewrite(PY, NXNATIVE, self.game / 'ExeFS', self.patched, dict(self.env, DM_TARGET='android'), self.logs, self.g)
        if self.derived or self.provisional:   # the keys that need the rewritten modules (upstream's provisional steps)
            nsp.provisional_prep(PY, NXNATIVE, self.env, self.logs, self.g)
            self.g = nsp.refine_after_rewrite(self.g, self.game, self.patched)
            if self.provisional and profile_guess.heap_check_needed(self.game / 'ExeFS') and not nsp.heap_checked(self.g):
                say('[RED] no heap check: it boots the game in the Mac host; the provisional profile keeps its guessed heap keys')

    # ---- shaders: upstream's split, then SPIR-V for the Vulkan backend ----
    def shaders(self):
        sv = self.versions
        pkg = self.work / 'shader' / 'pkg'
        nsp.split_shaders(PY, NXNATIVE, [self.dirs[v] / 'RomFS' for v in sv], [self.dirs[v] / 'ExeFS/main' for v in sv], pkg, self.env, self.logs, self.g, bool(self.derived or self.provisional),
                         aoc=self.aoc)   # DLC with shaders of its own (Xenoblade Chronicles 3's archives)
        self.dmsv = self.aw / 'shaders.dmsv'; kf = self.aw / 'shaders.dmsv.key'; cap = self.aw / 'captured_shaders'
        PROG.stage('Vulkan shaders', step='building the translator')
        xlate, xkey = self.translator()
        PROG.set(step='checking the inputs')
        key = nsp.fingerprint(nsp.package_key(pkg), ('files', [DM / 'android/tools/shader_vk.py'] + nsp.tool_files(NXNATIVE)), ('files', nsp.profile_files(self.g)), xkey,
                              ('files', sorted(cap.glob('*.bin'))))
        if self.dmsv.is_file() and (note := nsp.reused(kf, key)) is not None:
            PROG.end(f'{note}, reused (inputs unchanged)'); return
        tmp = self.dmsv.with_name('shaders.dmsv.partial'); nsp.rmtree(tmp); nsp.rmtree(kf)
        def line(s):
            if m := re.match(r'translating (\d+) shaders', s): PROG.set(step=f'translating {int(m.group(1)):,} shaders')
        nsp.run([PY, DM / 'android/tools/shader_vk.py', pkg, tmp, '--jobs', str(self.o.jobs), '--dotnet', self.tools['dotnet'], '--xlate', xlate, '--capture', cap],
                self.logs / 'android_shaders.log', env=self.env, on_line=line)
        m = re.search(r'^(\d+) of (\d+) shaders', (self.logs / 'android_shaders.log').read_text(errors='replace'), re.M)
        if not m or m.group(1) == '0': raise BuildError(f'no shader translated (see {self.logs / "android_shaders.log"})')
        note = f'{int(m.group(1)):,} of {int(m.group(2)):,} shaders, {tmp.stat().st_size / 2**20:,.0f} MB'
        nsp.swap_in(tmp, self.dmsv); kf.write_text(f'{key}\n{note}\n')
        PROG.end(note)
        for d in (self.work / 'tmp').glob('shvk_*'): nsp.rmtree(d)   # the translator's work folders

    def pull_captures(self):
        """Shaders whose code no game file holds: the app saves each one the bundle lacks on the device (gfx_vk_shader.c vk_shader_missing), and they are
        pulled here into <build folder>/android/captured_shaders, kept from port to port, for shader_vk.py to translate into the bundle (Xenoblade Chronicles 3: effect and model shaders).
        Only for an install, which has the device; a device that cannot be reached leaves the captures already pulled"""
        adb = Adb(self.tools['adb'], self.o.device or self.cfg.get('device'), log=say)
        try:
            adb.pick()
            got = adb.pull_new(f'{datapack.files_folder(self.package)}/captured_shaders', self.aw / 'captured_shaders', pattern=lambda n: re.fullmatch(r'[0-9a-f]{16}\.bin', n))
        except AdbError as e: say(f'shaders captured on the device not pulled ({e})'); return
        if got: say(f'{len(got)} shader(s) missing from the bundle, captured on the device: {" ".join(n[:16] for n in got)}')

    def translator(self):
        """The shader translator (dm/gfx/tools/dmsh_xlate over dm/third_party/ryujinx) built from the repository's source with the player's .NET SDK, once for every game,
        in <user dir>/tools/dmsh_xlate: (dmsh_xlate.dll, the source's key). Not the iOS chain's prebuilt dm/gfx/tools/dmsh_xlate_bin, which only a Mac can rebuild
        (its lock fingerprints Apple's Metal compiler): Android takes a translator fix as soon as it is in the source"""
        src = DM / 'gfx/tools/dmsh_xlate'; ryu = DM / 'third_party/ryujinx'
        files = sorted([*src.glob('*.cs'), *src.glob('*.csproj'), *ryu.glob('*.props')] + [p for p in (ryu / 'src').rglob('*') if p.suffix in ('.cs', '.csproj', '.props')
                                                                                         and not {'bin', 'obj'} & set(p.relative_to(ryu).parts)])
        key = nsp.fingerprint(('files', files), ('cmd', [str(self.tools['dotnet']), '--version']), env=self.env)
        out = paths.user_dir() / 'tools/dmsh_xlate'; kf = out.with_name('dmsh_xlate.key'); dll = out / 'dmsh_xlate.dll'
        if dll.is_file() and nsp.reused(kf, key) is not None: return dll, key
        tmp = out.with_name('dmsh_xlate.partial'); nsp.rmtree(tmp); nsp.rmtree(kf)
        nsp.run([self.tools['dotnet'], 'build', src, '-c', 'Release', '-o', tmp, '-nologo', '-v', 'quiet'], self.logs / 'android_translator_build.log', env=self.env)
        if not (tmp / 'dmsh_xlate.dll').is_file(): raise BuildError(f'the translator built without dmsh_xlate.dll (see {self.logs / "android_translator_build.log"})')
        nsp.swap_in(tmp, out); kf.write_text(f'{key}\n')
        return dll, key

    # ---- host: the Android host built for this profile with the player's NDK ----
    def host(self):
        PROG.stage('host', step=f'NDK {self.tools["ndk"].name}')
        out = self.aw / ('host-debug' if self.o.debug else 'host')
        nsp.run([PY, DM / 'android/build.py', '--game', self.g.id, '--ndk', self.tools['ndk'], '--out', out, '--lib', '--jobs', str(self.o.jobs)] + ([] if self.o.debug else ['--release']),
                self.logs / 'android_host.log', env=self.env)
        self.so = out / 'libdmhost.so'
        if not self.so.is_file(): raise BuildError(f'build.py made no {self.so}')
        PROG.end(f'{self.so.stat().st_size / 2**20:.1f} MB')

    # ---- apk: the host as a NativeActivity app, signed with the player's key ----
    def apk(self):
        PROG.stage('APK')
        stem = app_stem(self.g, self.app_name)
        self.apk_path = self.o.out / f'{stem}.apk'; self.o.out.mkdir(parents=True, exist_ok=True)
        icon = self.icon()
        driver_dir = os.environ.get('DM_VK_DRIVER_LIBS')
        driver_libs = sorted(Path(driver_dir).glob('*.so')) if driver_dir else []
        if driver_dir and not driver_libs: raise BuildError(f'no driver libraries in {driver_dir}')
        nsp.run([PY, DM / 'android/apk/mkapk.py', self.so, self.apk_path, paths.signing_key(), '--package', self.package, '--label', self.app_name, '--lib', 'dmhost',
                 '--version-code', str(version_code(self.pkg_ver)), '--version-name', self.display]
                + (['--landscape'] if self.ap.landscape else []) + (['--icon', icon] if icon else []) + ([] if self.o.debug else ['--release'])
                + [arg for so in driver_libs for arg in ('--extra-so', so)],
                self.logs / 'android_apk.log', env=self.env)
        self.result['apk'] = str(self.apk_path)
        PROG.end(f'{self.apk_path.name}, {self.apk_path.stat().st_size / 2**20:.1f} MB')

    def icon(self):   # the game's icon (nx_extract saves the control data's English one as icon.jpg) as a 256 px PNG; None without Pillow or an icon
        src = next((d / 'icon.jpg' for d in (self.game, self.base) if (d / 'icon.jpg').exists()), None)
        if not src: say('[RED] no game icon in the extraction: the app gets the system\'s default icon'); return None
        try: from PIL import Image
        except ImportError: say('[RED] no Pillow (pip install pillow): the app gets the system\'s default icon'); return None
        dst = self.aw / 'icon.png'
        Image.open(src).convert('RGB').resize((256, 256), Image.LANCZOS).save(dst)
        return dst

    # ---- data: the files the app reads from its files folder ----
    def data(self):
        PROG.stage('data pack', step='button-icon font')
        font_env, font_files = self.button_font()
        dst = self.o.out / f'{app_stem(self.g, self.app_name)}-data'
        man = datapack.build(dst, profile=self.ap, version=self.pkg_ver, display=self.display, patched=self.patched, romfs=self.game / 'RomFS', shaders=self.dmsv,
                             aoc=self.aoc, language=self.lang, package=self.package, env=self.fps_env + font_env, files={**self.fps_files, **font_files},
                             progress=lambda what: PROG.set(step=f'linking {what}'))
        self.data_path = dst; self.result['data'] = str(dst)
        PROG.end(f'{man["files"]:,} files, {man["bytes"] / 2**30:.1f} GB')

    def button_font(self):
        """The stand-in for the system's Nintendo extension shared font: the controller-button icons in game text (U+E0E0 = A; Super Mario Bros. Wonder's menus showed '?' without it), which
        the host serves for shared-font type 5 (hle_sdk16.c, DM_SHARED_FONT_EXT). make_ext_font.py draws it, as the Mac and iOS builds do, made again when the tool or the letters change.
        Returns (env.txt rows, pack files); none without fontTools or a letter font, and the icons draw as '?'"""
        letters = next((Path(p) for p in LETTER_FONTS if Path(p).is_file()), None)
        try: import fontTools   # noqa: F401 (make_ext_font.py runs with this Python)
        except ImportError: letters, why = None, 'no fontTools (pip install fonttools)'
        else: why = 'no bold system font for the letters'
        if not letters: say(f'[RED] {why}: the button icons in the game\'s text draw as "?"'); return [], {}
        d = self.aw / 'font'; ttf, kf = d / 'nintendo_ext.ttf', d / 'nintendo_ext.key'
        key = nsp.fingerprint(('files', [DM / 'tools/make_ext_font.py', letters]))
        if not (ttf.is_file() and nsp.reused(kf, key) is not None):
            nsp.rmtree(kf); d.mkdir(parents=True, exist_ok=True)
            nsp.run([PY, DM / 'tools/make_ext_font.py', ttf, letters], self.logs / 'make_ext_font.log', env=self.env)
            kf.write_text(f'{key}\n')
        return [f'DM_SHARED_FONT_EXT={datapack.files_folder(self.package)}/nintendo_ext.ttf'], {'nintendo_ext.ttf': ttf}

    # ---- install (optional) ----
    def install(self):
        install.install(self.apk_path, self.data_path, self.tools['adb'], self.o.device or self.cfg.get('device'), replace_signer=self.o.replace_signer)


def emit_result(r):   # the last JSON event of a port (the GUI's done screen); a summary line in the other modes
    if PROG.mode == 'json': PROG._json({'ev': 'result', **r})
