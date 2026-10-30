# The port: a game folder (NSP / XCI, or NSZ / XCZ: porter.dumps) + prod.keys + the game's profile → <App>.apk and <App>-data/ (docs/ARCHITECTURE.md "The port").
# The steps every platform shares are upstream's (porter.upstream.nsp: scan, extraction, the shader split, stage reuse, progress); the Android ones are here:
# the rewrite for DM_TARGET=android, the Vulkan shader bundle, the host built with the player's NDK, the APK, the data pack and the install.
# Work files go where upstream's NSP script puts them, <game folder>/<title ID>_build/ (extract/, aoc/, shader/, logs/ are shared with an iOS build of the same folder),
# with the Android ones in its android/ folder.
import os, re, shutil, sys
from dataclasses import dataclass, field
from pathlib import Path

from porter import config, datapack, dumps, fpslocker, install, paths, profiles, toolchain
from porter.adb import Adb, AdbError
from porter.paths import DM, NXNATIVE
from porter.upstream import BuildError, PROG, android_profile, game, nacp, nsp, profile_guess, say

PY = sys.executable
# The letters of the button-icon font (make_ext_font.py knocks them out of the icons): a bold system font of the machine that ports, never shipped in the repository
LETTER_FONTS = [f'{os.environ.get("WINDIR", "C:/Windows")}/Fonts/{f}' for f in ('ARLRDBD.TTF', 'arialbd.ttf')] + [
    '/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', '/usr/share/fonts/TTF/DejaVuSans-Bold.ttf']
# The stand-in for the standard shared font: a system font of the machine that ports with what Nintendo's covers (each has U+25A1: Arial in WGL4's geometric shapes,
# Arial Unicode and DejaVu Sans the whole block), never shipped in the repository
STANDARD_FONTS = [f'{os.environ.get("WINDIR", "C:/Windows")}/Fonts/arial.ttf', '/System/Library/Fonts/Supplemental/Arial Unicode.ttf', '/Library/Fonts/Arial Unicode.ttf',
                  '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', '/usr/share/fonts/TTF/DejaVuSans.ttf', '/usr/share/fonts/dejavu/DejaVuSans.ttf']


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
    vercode_offset: int = 0            # added to the APK's versionCode: a rebuild of the same game version installs over a build with a higher one (version_code)
    jobs: int = field(default_factory=lambda: os.cpu_count() or 8)


def app_stem(g, name):   # the file name of the APK and the data pack: the profile's iOS IPA name without .ipa (NeoTheWorldEndsWithYou)
    ipa = g.get('ipa') or nsp.ipa_name(name, g.title_id)
    return ipa[:-4] if ipa.endswith('.ipa') else ipa


VERCODE_OFFSET_MAX = 65535   # Switch versions are 65536 apart (v65536, v131072): a rebuild of one, offset by this much at most, stays below the next one's port


def version_code(ver, offset=0):
    """The APK's versionCode: the Switch version number (v196608 → 196609), so an update of the game installs over the base game's port. Android installs an APK over one
    with the same versionCode, never over a higher one (INSTALL_FAILED_VERSION_DOWNGRADE): offset (porter port --vercode-offset) lifts a rebuild above a build already on
    the device (an earlier rebuild with an offset, a port of a later version) and keeps its saves, where an uninstall deletes them"""
    if not 0 <= offset <= VERCODE_OFFSET_MAX: raise BuildError(f'the versionCode offset must be 0 to {VERCODE_OFFSET_MAX} (the next game version\'s port must stay above it): {offset}')
    return nsp.vnum(ver) + 1 + offset


def inspect(folder, keys=None):
    """What a game folder holds, without extracting anything: the GUI's game card and `porter inspect`. keys: prod.keys (default: the configured one), which reads the title ID
    of a dump with neither a ticket nor a [title ID] tag (an untagged cartridge dump) from its NCA headers"""
    app, fam = nsp.scan_nsps(Path(folder), dumps.EXTS, keys or config.load().get('keys'))
    g, ap = profiles.by_title(app)
    files = [fam['base'], *(p for _, p in fam['updates']), *(p for _, _, p in fam['dlc'])]
    info = dict(title_id=f'{app:016X}', base=fam['base'].name, updates=[{'version': f'v{v}' if v is not None else None, 'file': p.name} for v, p in fam['updates']],
                dlc=[{'title_id': f'{t:016X}', 'file': p.name} for t, _, p in fam['dlc']], profile=None, android=None, versions=[], pack=None, supported=False, reason='',
                compressed=[p.name for p in files if dumps.compressed(p)])   # NSZ / XCZ: decompressed during the port into the build folder's tmp/, deleted once extracted
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
    if ap.status == 'wip': info['reason'] = f'{g.get("name", g.id)} is a work in progress: it plays, but parts of it are missing (the Games table in README.md says which)'
    if pack not in builds:
        info['reason'] = (info['reason'] + '; ' if info['reason'] else '') + f'{pack} is not a version the profile was brought up on ({", ".join(builds)}): it is packed from a derived profile, without that version\'s fixes'
    return info


class Port:
    def __init__(self, opts):
        self.o = opts
        self.cfg = config.load()
        self.result = {}
        self.unpacked = {}   # NSZ / XCZ → its NSP in <build folder>/tmp/dumps/, while the port needs it

    # ---- the whole port ----
    def run(self):
        o = self.o
        o.folder = Path(o.folder).expanduser().resolve()
        if not o.folder.is_dir(): raise BuildError(f'not a folder: {o.folder}')
        version_code('v0', o.vercode_offset)   # an offset out of range fails now, not after the shaders
        keys = o.keys or self.cfg.get('keys')
        if not keys: raise BuildError('no prod.keys: give -k / --keys, or set it once with `porter config keys <path>`')
        o.keys = Path(keys).expanduser().resolve()
        try: self.tools = toolchain.require(self.cfg, str(o.keys), adb=o.install)
        except toolchain.ToolError as e: raise BuildError(str(e)) from e
        o.out = Path(o.out or self.cfg.get('out') or o.folder).expanduser().resolve()
        # the chosen dotnet first on PATH, in this process too: Windows finds a program by the parent's PATH, not the one passed to the child (upstream runs `dotnet` by name)
        os.environ['PATH'] = os.pathsep.join([str(self.tools['dotnet'].parent), os.environ.get('PATH', '')])
        os.environ['DOTNET_ROOT'] = str(toolchain.dotnet_root(self.tools['dotnet']))   # where nx_extract.exe's apphost looks for the runtime (else the system's, which may lack .NET 10)
        # PYTHONUTF8: the engine's tools write their tables with open() and no encoding, which is the ANSI code page on Windows (shader_split.py's "→" failed)
        self.env = dict(os.environ, J=str(o.jobs), PYTHONIOENCODING='utf-8', PYTHONUTF8='1', DOTNET_CLI_TELEMETRY_OPTOUT='1')

        try:
            self.inputs()
            PROG.plan(1 + len(self.versions) + bool(self.fam['dlc']) + bool(o.fps60) + 6 + bool(o.install))   # nx_extract, the versions, the DLC, 60 fps, rewrite / split / Vulkan shaders / host / APK / data, install (unpack adds its own)
            self.extract()
        finally: self.drop_unpacked()   # a failed run leaves no decompressed dumps behind either
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
        self.app, self.fam = nsp.scan_nsps(o.folder, dumps.EXTS, o.keys)
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
        Viractal 2.7 of 1.4), less what an earlier port left there, plus a margin; an NSZ / XCZ counts as decompressed, and once more for its NSP while it is extracted"""
        files = [f for f in self.o.folder.iterdir() if f.is_file() and f.suffix.lower()[1:] in dumps.EXTS]
        game, temp = sum(dumps.size(f) for f in files), sum(dumps.size(f) for f in files if dumps.compressed(f))
        built = sum(f.stat().st_size for f in self.work.rglob('*') if f.is_file()) if self.work.is_dir() else 0
        need, free = max(0, 2 * game + temp - built) + 2**30, shutil.disk_usage(self.o.folder).free
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
                ups = [p for _, p in sorted(self.fam['updates'], key=lambda u: u[0] or 0)[-1:]]   # the survey reads the base game and the newest update
                m = self.unpack([self.fam['base'], *ups])
                fam = dict(self.fam, base=m[self.fam['base']], updates=[(v, m.get(p, p)) for v, p in self.fam['updates']])
                facts = nsp.survey(self.o.folder, self.app, fam, self.o.keys, NXNATIVE, PY, self.env)
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
        need = sum(dumps.size(p) * (2 if dumps.compressed(p) else 1) for p in {p for b, u in picks.values() for p in (b, u) if p}) / 2**30 + 6
        if nsp.free_gb(self.work) < need and not all((ex / v).exists() for v in self.versions): raise BuildError(f'less than {need:.0f} GB free on the volume of {self.work}')
        # NSZ / XCZ: only what a stage will read is decompressed, all in one stage before nx_extract runs (a version nsp.extract reuses reads nothing)
        fresh = lambda v: (ex / v / 'ExeFS/main').exists() and (ex / v / 'RomFS' / g.get('romfs_marker')).exists()   # nsp.extract's reuse test
        zdlc = [p for _, _, p in self.fam['dlc'] if dumps.compressed(p)]
        # extract_aoc keys its reuse on the names, sizes and times of the NSPs it reads, decompressed ones here: with NSZ / XCZ DLC, porter keys it on those files instead
        dkey = nsp.fingerprint(sorted((t, p.name, p.stat().st_size, p.stat().st_mtime_ns) for t, _, p in self.fam['dlc']),
                               ('files', [NXNATIVE / 'dm/tools/nx_extract' / f for f in ('Program.cs', 'nx_extract.csproj')]))
        dkf = self.work / 'aoc.dumps.key'
        dlc_reused = bool(zdlc) and (self.work / 'aoc').is_dir() and nsp.reused(dkf, dkey) is not None
        m = self.unpack([p for v in self.versions if not fresh(v) for p in picks[v] if p] + ([] if dlc_reused else zdlc))
        picks = {v: tuple(m.get(p, p) if p else None for p in bu) for v, bu in picks.items()}
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
        if dlc_reused:
            PROG.stage('extract DLC'); self.aoc = self.work / 'aoc'; PROG.end(f'{len(self.fam["dlc"])} DLC, reused (inputs unchanged)')
        else:
            self.aoc = nsp.extract_aoc(nx, o.keys, NXNATIVE, self.work, self.app, dict(self.fam, dlc=[(t, v, m.get(p, p)) for t, v, p in self.fam['dlc']]), self.logs)
            if zdlc: dkf.write_text(f'{dkey}\n')
            else: nsp.rmtree(dkf)
        self.drop_unpacked()   # everything that reads them has run, and the shader stages that follow need the room
        self.base = next((self.dirs[v] for v in self.versions if picks[v][1] is None), self.dirs[self.versions[0]])
        self.app_name = o.app_name or nacp.name(self.base / 'control.nacp') or self.g.get('name', self.g.id)
        self.display = nacp.version(self.game / 'control.nacp') or nacp.version(self.base / 'control.nacp') or '1.0'
        lang_in = o.language or self.cfg.get('language')
        want = nsp.language_code(lang_in) if lang_in else 'en-US'
        if not want: raise BuildError(f'unknown game language {lang_in!r}')
        have = nacp.languages(self.game / 'control.nacp') or nacp.languages(self.base / 'control.nacp')
        self.lang = nsp.game_language(want, bool(lang_in), have)
        say(f'app: {self.app_name} {self.display} ({self.package}), game language {self.lang}')

    # ---- NSZ / XCZ: decompressed for the stages that read the game files (porter.dumps) into <build folder>/tmp/dumps/, deleted once extracted ----
    def unpack(self, paths):
        """paths' NSZ / XCZ → NSP, in one stage (a file decompressed earlier in this run is kept); returns {path: the file to read}"""
        todo = [p for p in dict.fromkeys(paths) if dumps.compressed(p) and p not in self.unpacked]
        if todo:
            d = self.work / 'tmp/dumps'; d.mkdir(parents=True, exist_ok=True)
            sizes = {p: dumps.size(p) for p in todo}; total, n = sum(sizes.values()), [0]
            if nsp.free_gb(d) < total / 2**30 + 1: raise BuildError(f'decompressing {", ".join(p.name for p in todo)} needs {total / 2**30:.1f} GB on the volume of {d}, which has {nsp.free_gb(d):.1f} GB free')
            PROG.add(1); PROG.stage('decompress NSZ / XCZ', unit='B', total=total)
            def done(k): n[0] += k; PROG.set(done=n[0])
            for p in todo:
                say(f'decompressing {p.name} ({sizes[p] / 2**30:.2f} GB) → {d}')
                self.unpacked[p] = dumps.decompress(p, d / dumps.unpacked_name(p), done)
            PROG.end(f'{len(todo)} file{"s" if len(todo) > 1 else ""}, {nsp.amount(total, "B")}, NCAs checked against their SHA-256 (not in ticketless dumps, whose NCA headers were rewritten)')
        return {p: self.unpacked.get(p, p) for p in paths}

    def drop_unpacked(self):
        if getattr(self, 'work', None): nsp.rmtree(self.work / 'tmp/dumps')
        self.unpacked = {}

    def check_graphics(self):
        """What the Android host can run: NVN games whose shaders are all in their files. The other kinds need a step upstream runs on a Mac (booting the game in the Mac host)
        or a graphics layer the Android host does not have yet"""
        src = profile_guess.build_sources(self.game / 'ExeFS', self.game / 'RomFS', self.g.words('modules'))
        gfx = src['graphics'][0]
        if gfx != 'nvn': raise BuildError(f'{self.app_name} renders with {gfx} ({src["graphics"][1]}): the Android host replays NVN only so far')
        # Game Freak's GFPAKs with Oodle entries: decoded by the game's own decoder on the Android device (device_oodle); Trinity and PKZ packs still need the Mac host
        self.oodle_why = src['oodle_packs'][1] if src['oodle_packs'][0] and 'GFPAK' in src['oodle_packs'][1] else ''
        if self.oodle_why:
            try: Adb(self.tools['adb'], self.o.device or self.cfg.get('device'), log=say).pick()
            except (AdbError, KeyError) as e: raise BuildError(f"{self.app_name}'s shaders are in Oodle-compressed archives ({self.oodle_why}): the port decodes them with the game's own decoder on "
                                                               f'the Android device, so connect it (adb) and port again ({e})') from e
        for k in ('glslc_capture', 'ue_decode', 'ue_legacy') + (() if self.oodle_why else ('oodle_packs',)):
            if src[k][0]: raise BuildError(f'{self.app_name} needs a step that boots the game in the Mac host ({src[k][1]}), which a Windows port cannot run yet')

    # ---- the game's own Oodle decoder on the device, for the shader split (Pokémon Legends: Arceus's GFPAKs) ----
    ODEV = '/data/local/tmp/nx2a-oodle'   # dmhost, the rewritten modules and the job, removed afterwards
    def device_oodle(self):
        """The Mac host's Oodle tool mode runs in the Android host as well (dm/host/hle_oodle.c DM_OODLE_JOB): the host built as an executable (build.py --exe) runs on the device
        with the rewritten modules and decodes with the game's own OodleLZ_Decompress, the same ARM64 code the Switch runs. Every Oodle entry of the GFPAKs goes in one job file
        (1.6 GB for Pokémon Legends: Arceus), pushed and decoded there in one run; only the entries that hold a BNSH shader come back (DM_OODLE_JOB_BNSH), written as the split's
        decoded folder (shader_split.py gfpak_decoded). Streaming the entries through an adb forward instead reset the connection whenever both directions were busy.
        → the split's environment"""
        import struct
        adb = self.oodle_adb = Adb(self.tools['adb'], self.o.device or self.cfg.get('device'), log=say); adb.pick()
        out = self.aw / ('host-debug' if self.o.debug else 'host')   # the host stage's own folder: its objects serve both
        PROG.set(step="building the host as a tool for the game's Oodle decoder")
        nsp.run([PY, DM / 'android/build.py', '--game', self.g.id, '--ndk', self.tools['ndk'], '--out', out, '--exe', '--jobs', str(self.o.jobs)] + ([] if self.o.debug else ['--release']),
                self.logs / 'android_oodle_host.log', env=self.env)
        PROG.set(step="the game's Oodle entries in a job file")
        tmp = self.work / 'tmp/oodle'; nsp.rmtree(tmp); tmp.mkdir(parents=True)
        recs, job = [], tmp / 'job.bin'   # recs: (label, archive size, entry index) of each record, in order
        with open(job, 'wb') as jf:
            jf.write(b'DMOJ' + struct.pack('<I', 0))
            for v in self.versions:
                rom = self.dirs[v] / 'RomFS'
                for p in sorted(rom.rglob('*.gfpak')):
                    d = p.read_bytes()
                    if d[:8] != b'GFLXPACK': continue
                    n, _, fo = struct.unpack_from('<IIQ', d, 0x10); label = '/' + p.relative_to(rom).as_posix()
                    for i in range(n):
                        ct, usz, csz, off = struct.unpack_from('<2xHII4xQ', d, fo + 0x18 * i)
                        if ct == 3: jf.write(struct.pack('<II', csz, usz) + d[off:off + csz]); recs.append((label, len(d), i))
            jf.seek(4); jf.write(struct.pack('<I', len(recs)))
        PROG.set(step=f"the game's Oodle decoder on the device: {len(recs):,} entries, {job.stat().st_size / 2**30:.1f} GB")
        adb.shell(f'rm -rf {self.ODEV}; mkdir -p {self.ODEV}/mods', check=False)
        adb.push_file(out / 'dmhost', f'{self.ODEV}/dmhost'); adb.push_tree(self.patched, f'{self.ODEV}/mods'); adb.push_file(job, f'{self.ODEV}/job.bin')
        nsp.rmtree(job)
        r = adb.shell(f'chmod 755 {self.ODEV}/dmhost; cd {self.ODEV} && DM_OODLE_JOB={self.ODEV}/job.bin DM_OODLE_JOB_BNSH=1 ./dmhost mods > job.log 2>&1; echo "exit $?"; '
                      f'grep -a -e DM_OODLE_JOB -e RED job.log | tail -5', timeout=3600)
        (self.logs / 'android_oodle_job.log').write_text(r, encoding='utf-8')
        if 'exit 0' not in r: raise BuildError(f"the game's Oodle decoder on the device failed (see {self.logs / 'android_oodle_job.log'})")
        adb.run('pull', f'{self.ODEV}/job.bin.out', str(tmp / 'job.out'), timeout=1800)
        b = (tmp / 'job.out').read_bytes(); nsp.rmtree(tmp / 'job.out')
        if b[:4] != b'DMOR' or struct.unpack_from('<I', b, 4)[0] != len(recs): raise BuildError("the game's Oodle decoder on the device answered for other entries than it was given")
        by, o = {}, 8   # label → (size, {index: bytes})
        for label, size, i in recs:
            n = struct.unpack_from('<I', b, o)[0]; o += 4
            ent = by.setdefault(label, (size, {}))[1]
            if n == 0xffffffff: continue
            if n == 0: raise BuildError(f"the game's Oodle decoder on the device failed on {label} entry {i}")
            ent[i] = b[o:o + n]; o += n
        dec = tmp / 'decoded'; dec.mkdir()
        for label, (size, ent) in by.items():
            body = b''.join(struct.pack('<II', i, len(x)) + x for i, x in sorted(ent.items()))
            (dec / (label.lstrip('/').replace('/', '__') + '.dec')).write_bytes(b'DGFD' + struct.pack('<QI', size, len(ent)) + body)
        say(f"shaders in Oodle-compressed archives ({self.oodle_why}): {len(recs):,} entries decoded by the game's own decoder on the device, "
            f"{sum(len(e) for _, e in by.values()):,} of them hold shaders")
        return {'DM_GFPAK_DECODED': str(dec)}
    def device_oodle_end(self):
        adb = getattr(self, 'oodle_adb', None)
        if not adb: return
        self.oodle_adb = None
        try: adb.shell(f'rm -rf {self.ODEV}', check=False)
        except AdbError as e: say(f'the Oodle decoder left on the device ({e}): {self.ODEV}')

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
        try: nsp.split_shaders(PY, NXNATIVE, [self.dirs[v] / 'RomFS' for v in sv], [self.dirs[v] / 'ExeFS/main' for v in sv], pkg, self.env, self.logs, self.g, bool(self.derived or self.provisional),
                               oodle=self.device_oodle if self.oodle_why else None,   # Oodle-compressed archives: the game's own decoder on the device
                               aoc=self.aoc)   # DLC with shaders of its own (Xenoblade Chronicles 3's archives)
        finally: self.device_oodle_end()
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
        code = version_code(self.pkg_ver, self.o.vercode_offset)
        if self.o.vercode_offset: say(f'versionCode {code} ({self.pkg_ver} + 1 + offset {self.o.vercode_offset})')
        nsp.run([PY, DM / 'android/apk/mkapk.py', self.so, self.apk_path, paths.signing_key(), '--package', self.package, '--label', self.app_name, '--lib', 'dmhost',
                 '--version-code', str(code), '--version-name', self.display]
                + (['--landscape'] if self.ap.landscape else []) + (['--icon', icon] if icon else []) + self.theme() + ([] if self.o.debug else ['--release']),
                self.logs / 'android_apk.log', env=self.env)
        self.result['apk'] = str(self.apk_path)
        PROG.end(f'{self.apk_path.name}, {self.apk_path.stat().st_size / 2**20:.1f} MB')

    def theme(self):
        """The on-screen gamepad's theme: only the game's own pack goes in the APK (dm/android/themes/themes.tsv maps title IDs to packs; a game with no row
        gets the ergo pack), as assets/overlay/ (theme.tsv and an atlas per layout), which the app reads at its first frame (dm/android/overlay_theme.c)"""
        pack = android_profile.theme(self.g.id)
        files = [pack / 'theme.tsv'] + [pack / f'{lay}.png' for lay in android_profile.LAYOUTS]
        missing = [f.name for f in files if not f.is_file()]
        if missing: say(f'[RED] the gamepad theme {pack.name} has no {", ".join(missing)}: the app draws its gamepad without a theme'); return []
        self.result['theme'] = pack.name; say(f'gamepad theme: {pack.name}')
        return [a for f in files for a in ('--asset', f'overlay/{f.name}={f}')]

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
        PROG.stage('data pack', step='shared-font stand-ins')
        font_env, font_files = self.button_font()
        std_env, std_files = self.standard_font()
        dst = self.o.out / f'{app_stem(self.g, self.app_name)}-data'
        man = datapack.build(dst, profile=self.ap, version=self.pkg_ver, display=self.display, patched=self.patched, romfs=self.game / 'RomFS', shaders=self.dmsv,
                             aoc=self.aoc, language=self.lang, package=self.package, env=self.fps_env + font_env + std_env,
                             files={**self.fps_files, **font_files, **std_files}, progress=lambda what: PROG.set(step=f'linking {what}'))
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

    def standard_font(self):
        """The stand-in for the system's standard shared font (hle_sdk16.c, DM_SHARED_FONT), copied from the porting machine. Without it the Android host falls back to the
        device's Roboto, which has no U+25A1: Link's Awakening looks that glyph up as its stand-in for missing characters while laying out its first text, got null and
        crashed (main+0x111e0d0). Returns (env.txt rows, pack files); none where the machine has no such font"""
        src = next((Path(p) for p in STANDARD_FONTS if Path(p).is_file()), None)
        if not src: say('[RED] no stand-in for the standard shared font on this computer: the app falls back to the device\'s Roboto, which lacks symbols some games need'); return [], {}
        d = self.aw / 'font'; ttf = d / 'standard.ttf'
        if not ttf.is_file() or ttf.stat().st_size != src.stat().st_size: d.mkdir(parents=True, exist_ok=True); shutil.copyfile(src, ttf)
        return [f'DM_SHARED_FONT={datapack.files_folder(self.package)}/standard.ttf'], {'standard.ttf': ttf}

    # ---- install (optional) ----
    def install(self):
        install.install(self.apk_path, self.data_path, self.tools['adb'], self.o.device or self.cfg.get('device'), replace_signer=self.o.replace_signer)


def emit_result(r):   # the last JSON event of a port (the GUI's done screen); a summary line in the other modes
    if PROG.mode == 'json': PROG._json({'ev': 'result', **r})
