# Unit tests of the port pipeline's own parts (python -m unittest discover tests). They need no game files, toolchains or device.
import hashlib, importlib.util, json, os, random, re, shutil, struct, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
os.environ['NX2A_HOME'] = tempfile.mkdtemp(prefix='nx2a_test_home_')

from porter import config, datapack, pipeline, profiles   # noqa: E402
from porter.upstream import android_profile, game          # noqa: E402

NEO = '010043B013C5C000'


def profile_dir(rows, gid='0100000000AAA000'):
    """A throwaway game profile with an android.tsv of these rows, found through DM_GAMES_DIR"""
    root = Path(tempfile.mkdtemp(prefix='nx2a_test_profiles_')); d = root / gid; d.mkdir()
    (d / 'game.tsv').write_text(f'title_id\t{gid}\nprogram_id\t{gid}\nname\tTest Game\nipa\tTestGame.ipa\nmodules\tmain sdk\n', encoding='utf-8')
    (d / 'android.tsv').write_text(''.join(f'{k}\t{v}\n' for k, v in rows), encoding='utf-8')
    os.environ['DM_GAMES_DIR'] = str(root)
    return d


class AndroidProfiles(unittest.TestCase):
    def tearDown(self): os.environ.pop('DM_GAMES_DIR', None)

    def test_neo(self):
        p = profiles.load(NEO)
        self.assertEqual(p.status, 'playable'); self.assertEqual(p.package, 'org.nx2a.neo'); self.assertTrue(p.landscape)
        self.assertEqual(p.settings, {'docked': '0', 'overlay': '1', 'rumble': '1'}); self.assertEqual(p.requires, ['bcn'])

    def test_only_android_profiles_listed(self):
        rows = profiles.all_entries()
        self.assertIn(NEO, [r.id for r in rows]); self.assertTrue(all(r.android for r in rows))
        self.assertGreater(len(profiles.all_entries(android_only=False)), len(rows))

    def test_fps_setting(self):   # the pause menu's Show FPS (front_android.c settings_load fps=)
        d = profile_dir([('status', 'boots'), ('setting', 'fps=1')])
        self.assertEqual(android_profile.load(d.name).settings, {'fps': '1'})
        with self.assertRaises(android_profile.ProfileError): android_profile.load(profile_dir([('status', 'boots'), ('setting', 'fps=2')]).name)

    def test_opacity_setting(self):   # the gamepad's opacity, 20..80 percent by 5 (the menu's slider, overlay_layout.h OV_OPACITY_MIN..MAX)
        self.assertEqual(android_profile.load(profile_dir([('status', 'boots'), ('setting', 'opacity=45')]).name).settings, {'opacity': '45'})
        for v in ('10', '85', '42'):
            with self.assertRaises(android_profile.ProfileError, msg=v): android_profile.load(profile_dir([('status', 'boots'), ('setting', f'opacity={v}')]).name)

    def test_default_package(self):
        d = profile_dir([('status', 'boots')])
        self.assertEqual(android_profile.load(d.name).package, 'org.nx2a.t0100000000aaa000')

    def test_bad_keys_are_refused(self):
        for rows in ([('status', 'great')], [('colour', 'red')], [('setting', 'volume=3')], [('overlay', 'tablet q 0.1 0.2')], [('env', 'lower=1')], [('package', 'Not A Package')],
                     [('status', 'boots'), ('status', 'playable')]):
            d = profile_dir(rows)
            with self.assertRaises(android_profile.ProfileError, msg=rows): android_profile.load(d.name)

    def test_header_default_layout(self):   # the ergonomic layouts (the themes' design), the tablet's then the phone's, the same controls in each
        h = android_profile.header(android_profile.load(NEO))
        rx = r'\{(OV_\w+), (-?[\d.]+)f, (-?[\d.]+)f, ([\d.]+)f, ([\d.]+)f, ([\d.]+)f, (\w+), "([^"]*)", "(\w+)"\}'
        tablet, phone = (re.findall(rx, h.split(f'#define DM_AOV_{n}')[1].split('#define')[0]) for n in ('TABLET', 'PHONE'))
        self.assertEqual((len(tablet), len(phone)), (16, 16)); self.assertIn('#define DM_AOV_N 16', h)
        self.assertEqual([r[0] for r in tablet], [r[0] for r in phone]); self.assertEqual([r[6] for r in tablet], [r[6] for r in phone])
        self.assertEqual(tablet[0], ('OV_STICK_L', '0.0', '0.2105', '0.5789', '0.2421', '0.2421', '0', '', 'stick_l'))   # 184 px of the 1216x760 design, 160 px from the left edge
        a = next(r for r in tablet if r[6] == 'DM_BTN_A'); self.assertEqual(a[1:3], ('1.0', '-0.1421'))   # from the right edge
        self.assertEqual(next(r for r in tablet if r[6] == 'DM_BTN_MINUS')[-2], '-')   # the minus button's label, not "no label"

    def test_header_overlay_move(self):   # a moved control: x and y fractions of the window, in the one layout named
        d = profile_dir([('status', 'boots'), ('overlay', 'phone a 0.5 0.5')])
        h = android_profile.header(android_profile.load(d.name))
        self.assertIn('{OV_BUTTON, 0.5f, 0.0f, 0.5f, 0.1456f, 0.1456f, DM_BTN_A, "A", "a"}', h.split('#define DM_AOV_PHONE')[1])
        self.assertNotIn('0.5f, 0.0f, 0.5f', h.split('#define DM_AOV_PHONE')[0])
        with self.assertRaises(android_profile.ProfileError): android_profile.load(profile_dir([('status', 'boots'), ('overlay', 'a 0.5 0.5')]).name)

    def test_theme_mapping(self):   # porter packs the game's own pack, the ergo pack for a game the table does not name
        self.assertEqual(android_profile.theme(NEO).name, 'neotwewy'); self.assertEqual(android_profile.theme(NEO.lower()).name, 'neotwewy')
        self.assertEqual(android_profile.theme('0100000000AAA000').name, 'ergo')
        rows = [l.split('\t') for l in (android_profile.THEMES / 'themes.tsv').read_text(encoding='utf-8').splitlines() if l and not l.startswith('#')]
        self.assertEqual(len({r[0] for r in rows}), len(rows))   # one pack per game
        for r in rows: self.assertRegex(r[0], r'^0100[0-9A-F]{12}$'); self.assertTrue((android_profile.THEMES / r[1] / 'theme.tsv').is_file(), r)

    def test_theme_packs(self):   # every pack: both layouts' atlases, and sprites only of the layout's controls, within the atlas, every control drawn idle (a button held too)
        from PIL import Image
        names = {r['name'] for r in android_profile.default_layout()}
        for pack in sorted(p for p in android_profile.THEMES.iterdir() if p.is_dir()):
            rows = [l.split('\t') for l in (pack / 'theme.tsv').read_text(encoding='utf-8').splitlines() if l and not l.startswith('#')]
            lays = {r[1]: r for r in rows if r[0] == 'layout'}
            self.assertEqual(set(lays), set(android_profile.LAYOUTS), pack.name)
            for lay, row in lays.items():
                im = Image.open(pack / row[4]); self.assertIn(im.mode, ('P', 'RGBA'), pack.name)   # what overlay_theme.c decodes
                sp = [r for r in rows if r[0] == 'sprite' and r[1] == lay]
                for r in sp:
                    self.assertIn(r[2], names, r); self.assertIn(r[4], ('idle', 'held', 'knob', 'decor'), r)
                    x, y, w, h = map(int, r[5:9]); self.assertTrue(x >= 0 and y >= 0 and x + w <= im.width and y + h <= im.height, (pack.name, r))
                drawn = {(r[2], r[3], r[4]) for r in sp}
                for n in names - {'dpad'}:
                    self.assertIn((n, '-', 'idle'), drawn, (pack.name, lay, n))
                    if n not in ('stick_l', 'stick_r'): self.assertIn((n, '-', 'held'), drawn, (pack.name, lay, n))
                for d in ('up', 'down', 'left', 'right'): self.assertTrue({('dpad', d, 'idle'), ('dpad', d, 'held')} <= drawn, (pack.name, lay, d))
                self.assertTrue({('stick_l', '-', 'knob'), ('stick_r', '-', 'knob')} <= drawn, (pack.name, lay))

    def test_apk_assets(self):   # mkapk.py --asset: the theme's files as assets/overlay/<name>, nothing of the other packs
        t = Path(tempfile.mkdtemp(prefix='nx2a_test_apk_'))
        try:
            so = t / 'libdmhost.so'; so.write_bytes(b'\x7fELF' + b'\0' * 60); pack = android_profile.theme(NEO)
            args = [a for f in ('theme.tsv', 'tablet.png', 'phone.png') for a in ('--asset', f'overlay/{f}={pack / f}')]
            import subprocess, zipfile
            subprocess.run([sys.executable, str(REPO / 'nxnative/dm/android/apk/mkapk.py'), str(so), str(t / 'a.apk'), str(t / 'k.json'), '--lib', 'dmhost'] + args,
                           check=True, capture_output=True)
            with zipfile.ZipFile(t / 'a.apk') as z:
                self.assertEqual(sorted(n for n in z.namelist() if n.startswith('assets/')), ['assets/overlay/phone.png', 'assets/overlay/tablet.png', 'assets/overlay/theme.tsv'])
                self.assertEqual(z.read('assets/overlay/tablet.png'), (pack / 'tablet.png').read_bytes())
        finally:
            shutil.rmtree(t, ignore_errors=True)


class DataPack(unittest.TestCase):
    def test_build(self):
        t = Path(tempfile.mkdtemp(prefix='nx2a_test_pack_'))
        try:
            (t / 'patched').mkdir(); (t / 'patched/main.patched.bin').write_bytes(b'x' * 10)
            (t / 'romfs/Data').mkdir(parents=True); (t / 'romfs/Data/a.bin').write_bytes(b'y' * 20)
            (t / 'aoc/1').mkdir(parents=True); (t / 'aoc/1/d.bin').write_bytes(b'z')
            (t / 'shaders.dmsv').write_bytes(b'DMSV')
            p = profiles.load(NEO)
            man = datapack.build(t / 'out-data', profile=p, version='v196608', display='1.03', patched=t / 'patched', romfs=t / 'romfs', shaders=t / 'shaders.dmsv',
                                 aoc=t / 'aoc', language='ja')
            o = t / 'out-data'
            self.assertTrue((o / 'mods/main.patched.bin').is_file()); self.assertTrue((o / 'RomFS/Data/a.bin').is_file()); self.assertTrue((o / 'aoc/1/d.bin').is_file())
            env = (o / 'env.txt').read_text()
            self.assertIn('DM_LANG=ja', env); self.assertIn('DM_AOC_DIR=/sdcard/Android/data/org.nx2a.neo/files/aoc', env)
            self.assertEqual((o / 'settings.txt').read_text(), 'docked=0\noverlay=1\nrumble=1\n')
            self.assertEqual((man['files'], man['bytes'], man['dlc'], man['requires']), (4, 35, [1], ['bcn']))   # the installer checks requires
            self.assertEqual(datapack.read_manifest(o)['package'], 'org.nx2a.neo')
            datapack.build(t / 'touch-data', profile=p, version='v196608', display='1.03', patched=t / 'patched', romfs=t / 'romfs', shaders=t / 'shaders.dmsv',
                           settings=pipeline.TOUCH_SETTINGS)   # --touch: the gamepad starts hidden, the profile's other settings stay
            self.assertEqual((t / 'touch-data/settings.txt').read_text(), 'docked=0\noverlay=0\nrumble=1\n')
            bare = android_profile.load(profile_dir([('status', 'experimental')]).name); os.environ.pop('DM_GAMES_DIR', None)   # no setting rows: --touch alone still writes the file
            datapack.build(t / 'bare-data', profile=bare, version='v196608', display='1.03', patched=t / 'patched', romfs=t / 'romfs', shaders=t / 'shaders.dmsv', settings=pipeline.TOUCH_SETTINGS)
            self.assertEqual((t / 'bare-data/settings.txt').read_text(), 'overlay=0\n')
        finally:
            shutil.rmtree(t, ignore_errors=True)

    @unittest.skipUnless(any(Path(p).is_file() for p in pipeline.LETTER_FONTS), 'no letter font on this machine')
    def test_button_font(self):   # the Nintendo extension stand-in: made once, in the pack, named in env.txt (Super Mario Bros. Wonder's "Press L+R" drew '?' without it)
        try: from fontTools.ttLib import TTFont
        except ImportError: self.skipTest('no fontTools')
        from types import SimpleNamespace
        t = Path(tempfile.mkdtemp(prefix='nx2a_test_font_'))
        try:
            port = SimpleNamespace(aw=t / 'android', logs=t / 'logs', env=dict(os.environ, PYTHONUTF8='1'), package='org.nx2a.test')
            env, files = pipeline.Port.button_font(port)
            self.assertEqual(env, ['DM_SHARED_FONT_EXT=/sdcard/Android/data/org.nx2a.test/files/nintendo_ext.ttf']); self.assertEqual(list(files), ['nintendo_ext.ttf'])
            cmap = TTFont(files['nintendo_ext.ttf']).getBestCmap()
            self.assertTrue({0xE0E0, 0xE0E1, 0xE0E4, 0xE0E5, 0xE0EF, 0xE100} <= set(cmap))   # A, B, L, R, +, stick
            made = files['nintendo_ext.ttf'].stat().st_mtime_ns
            self.assertEqual(pipeline.Port.button_font(port)[1]['nintendo_ext.ttf'].stat().st_mtime_ns, made)   # reused while the tool and the letters are unchanged
        finally:
            shutil.rmtree(t, ignore_errors=True)


class Experimental(unittest.TestCase):
    def test_ios_profile_copied_with_experimental_android_tsv(self):   # an iOS-only profile is copied out of the repository, never edited in place
        ios_only = '010028600EBDA000'   # Super Mario 3D World + Bowser's Fury: an iOS profile without android.tsv or patches.py
        t = Path(tempfile.mkdtemp(prefix='nx2a_test_exp_')); environ = dict(os.environ)
        try:
            port = pipeline.Port(pipeline.Options(folder=t, experimental=True)); port.env = {}; port.work = t / f'{ios_only}_build'; port.o.keys = None
            g, ap = port.experimental_profile(game.load(ios_only), ios_only)
            self.assertEqual(g.dir, (t / f'{ios_only}_build/profile/{ios_only}').resolve())
            self.assertEqual((ap.status, ap.package), ('experimental', f'org.nx2a.t{ios_only.lower()}'))
            self.assertFalse((REPO / f'nxnative/dm/games/{ios_only}/android.tsv').exists())
            self.assertFalse((g.dir / 'patches.py').exists())   # patch_nso.py finds the per-build cuts in dm/games
            self.assertEqual(os.environ.get('DM_GAMES_FIRST'), '1')
        finally:
            os.environ.clear(); os.environ.update(environ); shutil.rmtree(t, ignore_errors=True)


class Misc(unittest.TestCase):
    def test_config_roundtrip(self):
        config.set_value('device', 'abc:5555'); self.assertEqual(config.load()['device'], 'abc:5555')
        with self.assertRaises(KeyError): config.set_value('nope', 'x')

    def test_version_code_grows_with_updates(self):
        self.assertLess(pipeline.version_code('v0'), pipeline.version_code('v65536'))
        self.assertEqual(pipeline.version_code('v196608'), 196609)

    def test_version_code_offset(self):   # a rebuild lifted above an earlier one, still below the next game version's port
        self.assertEqual(pipeline.version_code('v589824', 1), 589826)
        self.assertLess(pipeline.version_code('v65536', pipeline.VERCODE_OFFSET_MAX), pipeline.version_code('v131072'))
        for bad in (-1, pipeline.VERCODE_OFFSET_MAX + 1):
            with self.assertRaises(pipeline.BuildError): pipeline.version_code('v65536', bad)

    def test_install_downgrade_names_the_offset(self):   # a refused downgrade says what is installed and how to lift the rebuild above it
        from porter.adb import Adb, AdbError
        a = Adb('adb')
        def run(*x, **_):
            if x[0] == 'install': raise AdbError('adb install: Failure [INSTALL_FAILED_VERSION_DOWNGRADE: Downgrade detected: Update version code 589825 is older than current 589826]')
            return '    versionCode=589826 minSdk=29 targetSdk=34\n'
        a.run = run
        with self.assertRaises(AdbError) as e: a.install('x.apk', 'org.nx2a.test')
        self.assertIn('(589826)', str(e.exception)); self.assertIn('--vercode-offset', str(e.exception))

    def test_derived_alias_headroom(self):   # issue #26: a derived profile of a game with virtual address memory gets 8 GB more alias region, the heap unchanged
        d = profile_dir([('status', 'playable')])
        try:
            t = d / 'game.tsv'; base = t.read_text(encoding='utf-8')
            t.write_text(base + '# memory\nmem_reserve\t0x140000000\nmem_alias\t0x118000000\nmem_stack\t0x10000000\nsys_resource\t0x1000000\n', encoding='utf-8')
            self.assertEqual(pipeline.alias_headroom(t), (0x118000000, 0x318000000))
            g = game.load(d.name)
            self.assertEqual((g.int('mem_reserve'), g.int('mem_alias'), g.int('mem_stack')), (0x340000000, 0x318000000, 0x10000000))
            self.assertIn('# porter: 8 GB more alias region', t.read_text(encoding='utf-8'))
            vam = base + 'mem_reserve\t0x140000000\nmem_alias\t0x118000000\nmem_stack\t0x10000000\nsys_resource\t0x1000000\n'
            t.write_text(vam, encoding='utf-8'); self.assertEqual(pipeline.alias_headroom(t, 16), (0x118000000, 0x518000000))   # --alias-headroom 16
            self.assertIn('# porter: 16 GB more alias region', t.read_text(encoding='utf-8'))
            t.write_text(vam, encoding='utf-8'); self.assertIsNone(pipeline.alias_headroom(t, 0)); self.assertEqual(t.read_text(encoding='utf-8'), vam)   # 0: as it is
            for rows in ('sys_resource\t0x0\n', ''):   # no virtual address memory, or a profile older than the key (heap games): as it is
                t.write_text(base + 'mem_reserve\t0x100000000\nmem_alias\t0x10000000\nmem_stack\t0x10000000\n' + rows, encoding='utf-8')
                self.assertIsNone(pipeline.alias_headroom(t)); self.assertIsNone(pipeline.alias_headroom(t, 16))
                self.assertEqual(game.load(d.name).int('mem_alias'), 0x10000000)
        finally: shutil.rmtree(d.parent, ignore_errors=True); os.environ.pop('DM_GAMES_DIR', None)

    def test_alias_headroom_option(self):   # --alias-headroom: parsed, checked before any stage, and kept as None (automatic) when not given
        from porter import cli
        self.assertEqual(cli._parser().parse_args(['port', 'x', '--alias-headroom', '16']).alias_headroom, 16)
        self.assertIsNone(cli._parser().parse_args(['port', 'x']).alias_headroom)
        for ok in (None, 0, pipeline.ALIAS_HEADROOM_MAX): self.assertEqual(pipeline.alias_headroom_gb(ok), ok)
        t = Path(tempfile.mkdtemp(prefix='nx2a_test_game_'))
        try:
            for bad in (-1, pipeline.ALIAS_HEADROOM_MAX + 1):
                with self.assertRaises(pipeline.BuildError) as e: pipeline.Port(pipeline.Options(folder=t, alias_headroom=bad)).run()
                self.assertIn('--alias-headroom', str(e.exception))
        finally: shutil.rmtree(t, ignore_errors=True)

    def test_mem_profile(self):   # --alias-headroom on a version the profile was brought up on: a copy in profile-mem/ in front, without patches.py; none without the option
        d = profile_dir([('status', 'playable')]); environ = dict(os.environ); t = Path(tempfile.mkdtemp(prefix='nx2a_test_build_'))
        try:
            (d / 'game.tsv').write_text((d / 'game.tsv').read_text(encoding='utf-8') + 'mem_reserve\t0x140000000\nmem_alias\t0x118000000\nmem_stack\t0x10000000\nsys_resource\t0x1000000\n',
                                        encoding='utf-8')
            (d / 'patches.py').write_text('CUTS = {}\n', encoding='utf-8')
            port = pipeline.Port(pipeline.Options(folder=t, alias_headroom=16)); port.env = {}; port.work = t; port.g = game.load(d.name)
            port.mem_profile()
            self.assertEqual(port.g.dir, (t / 'profile-mem' / d.name).resolve())
            self.assertEqual((port.g.int('mem_alias'), port.g.int('mem_reserve')), (0x518000000, 0x540000000))
            self.assertFalse((port.g.dir / 'patches.py').exists()); self.assertEqual(os.environ.get('DM_GAMES_FIRST'), '1')
            shutil.rmtree(t / 'profile-mem')   # as extract() does before it
            port.o.alias_headroom = None; port.g = game.load(d.name); port.mem_profile()
            self.assertFalse((t / 'profile-mem').exists()); self.assertEqual(port.g.dir, d.resolve())
        finally:
            os.environ.clear(); os.environ.update(environ); shutil.rmtree(d.parent, ignore_errors=True); shutil.rmtree(t, ignore_errors=True)

    def test_lost_patches(self):   # issue #26: a derived module of another build than the tested one, which has cuts, is named; one with cuts of its own, or no cuts lost, is not
        d = Path(tempfile.mkdtemp(prefix='nx2a_test_exefs_'))
        try:
            for m, bid in (('main', 'aa' * 16), ('sdk', 'bb' * 16), ('subsdk0', 'cc' * 16)): (d / m).write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid))
            builds = {'v65536': {'display': '1.1.0', 'main': '11' * 16, 'sdk': '22' * 16, 'subsdk0': 'cc' * 16}}
            table = {'22' * 16: ('dm/tools/sdk_patches.py', ['AddressRegionManager constructor: node area limit', 'AddressRegionManager constructor: length']), '33' * 16: ('x', ['y'])}
            lost = pipeline.lost_patches(builds, d, ['main', 'sdk', 'subsdk0'], table)
            self.assertEqual(len(lost), 1); self.assertTrue(lost[0].startswith('sdk build bbbbbbbb… is not the tested 22222222…'))
            self.assertIn('2 registered patches (dm/tools/sdk_patches.py: AddressRegionManager constructor)', lost[0])
            table['bb' * 16] = ('dm/tools/sdk_patches.py', ['z'])   # the derived build has cuts of its own
            self.assertEqual(pipeline.lost_patches(builds, d, ['main', 'sdk', 'subsdk0'], table), [])
            self.assertIn('6075151627016eef437c1312f2e3ed56', pipeline.registered_patches())   # the real files load
        finally: shutil.rmtree(d, ignore_errors=True)

    def test_app_stem(self):
        self.assertEqual(pipeline.app_stem(game.load(NEO), 'NEO'), 'NeoTheWorldEndsWithYou')

    def test_disk_space(self):   # twice the game files, less what the build folder holds, plus 1 GB
        from types import SimpleNamespace as NS
        d = Path(tempfile.mkdtemp(prefix='nx2a_test_game_')); (d / 'g.nsp').write_bytes(b'\0' * 2**20); work = d / 'build'
        p = NS(o=NS(folder=d), work=work); real = shutil.disk_usage
        try:
            shutil.disk_usage = lambda _: NS(free=2**30 + 2**20)
            with self.assertRaises(pipeline.BuildError): pipeline.Port.check_space(p)
            work.mkdir(); (work / 'x').write_bytes(b'\0' * 2**20); pipeline.Port.check_space(p)   # a build folder already there counts
        finally: shutil.disk_usage = real


def _hfs0(files):   # an HFS0 of (name, data): the XCI's partitions (hash fields left zero: nothing here reads them)
    strs = b''.join(n.encode() + b'\0' for n, _ in files); ent, off = b'', 0
    for i, (n, d) in enumerate(files):
        ent += struct.pack('<QQII8x32s', off, len(d), sum(len(m.encode()) + 1 for m, _ in files[:i]), 0, b''); off += len(d)
    return struct.pack('<4sII4x', b'HFS0', len(files), len(strs)) + ent + strs + b''.join(d for _, d in files)


@unittest.skipUnless(importlib.util.find_spec('Crypto'), 'no pycryptodome')
class Dumps(unittest.TestCase):   # porter/dumps.py: NSZ / XCZ → NSP; an NCZ made here as nsz makes it, from an NCA of a plain section and an AES-CTR one
    def setUp(self):
        from Crypto.Cipher import AES
        from porter import dumps; self.dumps = dumps
        rnd = random.Random(7); b = lambda n: bytes(rnd.getrandbits(8) for _ in range(n))
        self.key, self.ctr = b(16), b(8) + b'\0' * 8
        head, plain = b(0x4000), b(0x2000) + bytes(0x18000)   # the CTR part compresses (zeros), so zstd has something to do
        self.nca = head + plain[:0x2000] + AES.new(self.key, AES.MODE_CTR, nonce=self.ctr[:8], initial_value=0x6000 >> 4).encrypt(plain[0x2000:])
        self.name = hashlib.sha256(self.nca).hexdigest()[:32]
        secs = struct.pack('<8sQ', b'NCZSECTN', 2) + struct.pack('<QQQQ16s16s', 0x4000, 0x2000, 1, 0, bytes(16), bytes(16)) + struct.pack('<QQQQ16s16s', 0x6000, 0x18000, 3, 0, self.key, self.ctr)
        z = dumps.zstd_mod()
        self.solid = head + secs + z.compress(plain)
        bs = 1 << 14; blocks = [plain[i:i + bs] for i in range(0, len(plain), bs)]
        comp = [z.compress(k) if i else k for i, k in enumerate(blocks)]   # the first block stored as it is (random data: compressing does not shrink it)
        self.block = head + secs + struct.pack('<8sBBBBIQ', b'NCZBLOCK', 2, 1, 0, 14, len(blocks), len(plain)) + struct.pack(f'<{len(blocks)}I', *map(len, comp)) + b''.join(comp)
        self.d = Path(tempfile.mkdtemp(prefix='nx2a_test_dumps_'))

    def nsz(self, ncz, name='g.nsz'):
        files = [(self.name + '.ncz', ncz), ('0100000000aaa0000000000000000000.tik', b't' * 0x2c0)]
        hdr = self.dumps._pfs0_header([n for n, _ in files], [len(d) for _, d in files])
        (self.d / name).write_bytes(hdr + b''.join(d for _, d in files)); return self.d / name

    def check(self, src):
        self.assertEqual(self.dumps.size(src), len(self.dumps._pfs0_header([self.name + '.nca', 'x' * 36], [0, 0])) + len(self.nca) + 0x2c0)
        out = self.dumps.decompress(src, self.d / self.dumps.unpacked_name(src))
        self.assertEqual(out.stat().st_size, self.dumps.size(src)); self.assertEqual(out.stat().st_mtime_ns, src.stat().st_mtime_ns)
        with open(out, 'rb') as f: got = {n: (f.seek(o), f.read(s))[1] for n, o, s in self.dumps.entries(out)}
        self.assertEqual(got[self.name + '.nca'], self.nca); self.assertEqual(len(got), 2)

    def test_solid_and_block(self):
        self.check(self.nsz(self.solid)); self.check(self.nsz(self.block, 'b.nsz'))

    def test_first_section_in_the_header(self):   # nsz 5.0 starts the first section at 0xc00, inside the header it keeps as it is (a DLC NSZ it made, 2026-10-29)
        s = bytearray(self.solid); s[0x4000 + 16:0x4000 + 32] = struct.pack('<QQ', 0xc00, 0x2000 + 0x4000 - 0xc00)
        self.check(self.nsz(bytes(s)))

    def test_xcz_is_its_secure_partition(self):
        secure = _hfs0([(self.name + '.ncz', self.block), ('0100000000aaa0000000000000000000.tik', b't' * 0x2c0)])
        root = _hfs0([('update', _hfs0([])), ('secure', secure)])
        x = bytearray(0x200); x[0x100:0x104] = b'HEAD'; x[0x130:0x138] = struct.pack('<Q', 0x200)
        (self.d / 'g.xcz').write_bytes(bytes(x) + root); self.check(self.d / 'g.xcz')

    def test_damage_is_found(self):   # a changed byte: a zstd error, or an NCA whose SHA-256 is not its name; either way a BuildError and no NSP
        for ncz, at in ((self.solid, 0x4000 + 16 + 0x80 + 40), (self.block, -1), (self.block, 0x4000 + 16 + 0x80 + 24 + 100)):
            bad = bytearray(ncz); bad[at] ^= 1
            with self.assertRaises(pipeline.BuildError): self.dumps.decompress(self.nsz(bytes(bad)), self.d / 'out.nsp')
            self.assertFalse((self.d / 'out.nsp').exists())

    def test_scan_takes_compressed_dumps(self):   # upstream's scan_nsps with porter's kinds: the NSZ's ticket names its title ID
        self.nsz(self.solid, 'Game [v0].nsz')
        app, fam = pipeline.nsp.scan_nsps(self.d, self.dumps.EXTS)
        self.assertEqual(app, 0x0100000000AAA000); self.assertEqual(fam['base'].suffix, '.nsz')


@unittest.skipUnless(importlib.util.find_spec('Crypto'), 'no pycryptodome')
class UntaggedXci(unittest.TestCase):   # issue #28: a cartridge dump named by its release ("hr-bdlwa.xci") has no ticket and no [title ID]: the content meta NCA's header names it
    BASE = 0x01009D901BC56000

    def setUp(self):
        rnd = random.Random(28); self.hk = bytes(rnd.getrandbits(8) for _ in range(32))
        self.d = Path(tempfile.mkdtemp(prefix='nx2a_test_xci_'))
        self.keys = self.d / 'prod.keys'; self.keys.write_text(f'header_key = {self.hk.hex()}\n')

    def nca(self, tid, ctype):   # an NCA header (0xc00 bytes) whose second sector is encrypted as Nintendo does: AES-XTS, the sector number big-endian as the tweak
        from Crypto.Cipher import AES
        p = bytearray(0x200); p[:4] = b'NCA3'; p[5] = ctype; struct.pack_into('<Q', p, 0x10, tid)
        data, t = AES.new(self.hk[:16], AES.MODE_ECB), int.from_bytes(AES.new(self.hk[16:], AES.MODE_ECB).encrypt((1).to_bytes(16, 'big')), 'little')
        c = b''
        for i in range(0, 0x200, 16):
            tb = t.to_bytes(16, 'little'); c += bytes(a ^ b for a, b in zip(data.encrypt(bytes(a ^ b for a, b in zip(p[i:i + 16], tb))), tb))
            t = (t << 1 & (1 << 128) - 1) ^ (0x87 if t >> 127 else 0)
        return bytes(0x200) + c + bytes(0x800)

    def xci(self, name, ncas, key_area=False):
        root = _hfs0([('update', _hfs0([])), ('secure', _hfs0(ncas))])
        x = bytearray(0x200); x[0x100:0x104] = b'HEAD'; x[0x130:0x138] = struct.pack('<Q', 0x200)
        (self.d / name).write_bytes(bytes(0x1000 if key_area else 0) + bytes(x) + root)

    def test_scan_reads_the_meta_nca(self):   # an update's program NCA carries the base game's title ID: only the meta NCA's counts, and the smallest of them is the base game
        self.xci('hr-bdlwa.xci', [('a.nca', self.nca(self.BASE + 0x800, 0)), ('b.cnmt.nca', self.nca(self.BASE + 0x800, 1)), ('c.nca', self.nca(self.BASE, 0)), ('d.cnmt.nca', self.nca(self.BASE, 1))])
        app, fam = pipeline.nsp.scan_nsps(self.d, ('nsp', 'xci'), self.keys)
        self.assertEqual(app, self.BASE); self.assertEqual(fam['base'].name, 'hr-bdlwa.xci')
        self.xci('hr-bdlwa.xci', [('c.nca', self.nca(self.BASE + 0x800, 0)), ('d.cnmt.nca', self.nca(self.BASE + 0x800, 1))], key_area=True)
        self.assertEqual(pipeline.nsp.title_of(self.d / 'hr-bdlwa.xci', self.hk), self.BASE + 0x800)

    def test_without_keys_the_error_says_how(self):
        self.xci('hr-bdlwa.xci', [('d.cnmt.nca', self.nca(self.BASE, 1))])
        with self.assertRaisesRegex(pipeline.BuildError, r'hr-bdlwa\.xci: no ticket .*no prod\.keys.*\[01009D901BC56000\]\[v0\]'): pipeline.nsp.scan_nsps(self.d, ('nsp', 'xci'))
        self.keys.write_text(f'header_key = {bytes(32).hex()}\n')   # another key: the header does not decrypt to NCA3
        with self.assertRaisesRegex(pipeline.BuildError, 'no content meta NCA'): pipeline.nsp.scan_nsps(self.d, ('nsp', 'xci'), self.keys)


class FpsLocker(unittest.TestCase):   # porter/fpslocker.py: FPSLocker-Warehouse configs → the host's rows (hle_fpslock.c DM_FPS_LOCKS) and the rewriter's cuts
    BID = 'd49165c05ead8ae140f189aa97934f39'
    UNITY = ('ALL_FPS:\n  # Vsync\n  -\n    type: evaluate_write\n    address: [MAIN, 0x7A642D8]\n    value_type: int32\n    value: VSYNC_TARGET\n'
             '  -\n    type: evaluate_write\n    address: [MAIN, 0x7A642E4]\n    value_type: float\n    value: "1 / FPS_LOCK_TARGET"\n')   # the shape of Viractal 1.4.5's config

    def rows(self, text, image=b''):
        from porter import fpslocker
        tsv, cuts = fpslocker.convert(text, image, self.BID, 'test')
        return [l.split('\t') for l in tsv.splitlines() if not l.startswith('#')], cuts

    def test_unity_vsync_and_frame_time(self):   # 2 vsyncs / 1/30 s at 30; 1 vsync / 1/62 s at 60 (FPS_LOCK_TARGET is the target + 2 at the refresh rate)
        rows, cuts = self.rows(self.UNITY)
        self.assertEqual(rows[0], ['build', self.BID]); self.assertEqual(cuts, [])
        self.assertEqual(rows[1][1:], ['int32', '0x7a642d8', '2', '1', '-'])
        self.assertEqual(rows[2][1:3], ['float', '0x7a642e4']); self.assertAlmostEqual(float(rows[2][3]), 1 / 30); self.assertAlmostEqual(float(rows[2][4]), 1 / 62)

    def test_expressions(self):
        from porter.fpslocker import evaluate
        self.assertEqual(evaluate('TruncDec(FRAMETIME_TARGET, 2)', 30), 33.33); self.assertEqual(evaluate('2^3', 60), 8)
        self.assertEqual(evaluate('VSYNC_TARGET', 60), 1); self.assertEqual(evaluate('floor(FPS_TARGET / 7)', 60), 8)
        with self.assertRaises(Exception): evaluate('__import__("os")', 60)

    def test_pointers_lists_and_compares(self):
        rows, _ = self.rows('ALL_FPS:\n  -\n    type: compare\n    compare_address: [MAIN, 0x10, 0x8]\n    compare_type: "!="\n    compare_value_type: int8\n    compare_value: 0\n'
                            '    address: [MAIN, 0x20, 0x30, -4]\n    value_type: int32\n    value: [2, 2]\n  -\n    type: block\n    what: timing\n')
        self.assertEqual(rows[1][1:], ['int32', '0x20,0x30,-0x4', '2,2', '2,2', 'int8 0x10,0x8 != 0'])
        self.assertEqual(len(rows), 2)   # block timing is FPSLocker's own frame delay: dropped

    def test_refused_whole(self):   # entries work together: one the host cannot apply refuses the config
        from porter import fpslocker
        for text in (self.UNITY + 'DECLARATIONS:\n  -\n    type: variable\n    name: x\n    value_type: float\n    default_value: 0\n',
                     self.UNITY + 'MASTER_WRITE:\n  -\n    type: asm_a64\n    main_offset: 0x10\n    instructions: [[b.ne, :goto1], [nop], :goto1, [nop]]\n',   # labels parse
                     'ALL_FPS:\n  -\n    type: write\n    address: [HEAP, 0x10]\n    value_type: int32\n    value: 1\n',
                     'ALL_FPS:\n  -\n    type: compare\n    compare_address: [MAIN, 0x10]\n    compare_type: "=="\n    compare_value_type: int8\n    compare_value: 1\n    value_type: refresh_rate\n    value: 30\n',
                     'ALL_FPS: [\n'):
            with self.assertRaises(fpslocker.Refused): fpslocker.convert(text, b'', self.BID)

    def test_master_write_bytes_are_cuts(self):   # the original bytes come from main's image, so the rewriter can assert them
        image = bytes(range(256)) * 4
        _, cuts = self.rows(self.UNITY + 'MASTER_WRITE:\n  -\n    type: bytes\n    main_offset: 0x10\n    value_type: uint8\n    value: [0x1F, 0x20, 0x03]\n', image)
        self.assertEqual(cuts, [(0x10, '101112', '1f2003', 'FPSLocker-Warehouse MASTER_WRITE entry 1')])
        from porter import fpslocker
        ns = {}; exec(fpslocker.patches_py(self.BID, cuts), ns); self.assertEqual(ns['CUTS'][self.BID][0][:3], (0x10, '101112', '1f2003'))

    def test_url(self):
        from porter import fpslocker
        self.assertTrue(fpslocker.url('0100fd501f95a000', self.BID).endswith('/patches/0100FD501F95A000/D49165C05EAD8AE1.yaml'))


class Saves(unittest.TestCase):   # porter/saves.py: the save folders on the device, a .zip or folder to import (no device: the shell answers are made up)
    class FakeAdb:
        def __init__(self, sizes, times, packages=('org.nx2a.a',)): self.sizes, self.times, self.packages, self.cmds = sizes, times, packages, []
        def remote_sizes(self, rdir): return dict(self.sizes)
        def shell(self, script, **kw):
            self.cmds.append(script)
            if script.startswith('pm list packages'): return ''.join(f'package:{p}\n' for p in self.packages)
            if 'stat -c "%Y %n"' in script: return ''.join(f'{t} {n}\n' for n, t in self.times.items())
            return ''

    def test_listing(self):   # newest first; a device save counts with its own; hidden folders (an export still copying) and the request files left out
        from porter import saves
        adb = self.FakeAdb({'20261030-100000/a.dat': 10, '20261030-100000.device/d.dat': 5, '20261029-090000/a.dat': 7, 'result.txt': 40, '.x.part/a.dat': 9, 'console/sub/b.bin': 3},
                           {'20261030-100000': 200, '20261030-100000.device': 200, '20261029-090000': 100, 'console': 300})
        rows = saves.listing(adb, 'org.nx2a.a')
        self.assertEqual([r['name'] for r in rows], ['console', '20261030-100000', '20261029-090000'])
        self.assertEqual((rows[1]['files'], rows[1]['bytes'], rows[1]['device']), (2, 15, True))
        self.assertEqual((rows[0]['files'], rows[0]['bytes'], rows[0]['device']), (1, 3, False))

    def test_pick_app(self):
        from porter import saves
        from porter.upstream import BuildError
        self.assertEqual(saves.pick_app(self.FakeAdb({}, {}), ''), 'org.nx2a.a')
        two = self.FakeAdb({}, {}, ('org.nx2a.a', 'org.nx2a.b'))
        self.assertEqual(saves.pick_app(two, 'org.nx2a.b'), 'org.nx2a.b')
        with self.assertRaises(BuildError): saves.pick_app(two, '')
        with self.assertRaises(BuildError): saves.pick_app(two, 'org.nx2a.c')

    def test_zip(self):   # a .zip of one folder imports that folder; one of loose files imports them; a path out of the archive is refused
        import zipfile
        from porter import saves
        from porter.upstream import BuildError
        t = Path(tempfile.mkdtemp(prefix='nx2a_test_saves_'))
        with zipfile.ZipFile(t / 'one.zip', 'w') as z: z.writestr('MySave/main.dat', b'x'); z.writestr('MySave/sub/b.dat', b'y')
        with zipfile.ZipFile(t / 'loose.zip', 'w') as z: z.writestr('main.dat', b'x'); z.writestr('other.dat', b'y')
        with zipfile.ZipFile(t / 'bad.zip', 'w') as z: z.writestr('../evil.dat', b'x')
        u = Path(tempfile.mkdtemp(dir=t)); self.assertEqual(saves._unpacked(t / 'one.zip', u), u / 'MySave')
        u = Path(tempfile.mkdtemp(dir=t)); self.assertEqual(sorted(p.name for p in saves._unpacked(t / 'loose.zip', u).iterdir()), ['main.dat', 'other.dat'])
        with self.assertRaises(BuildError): saves._unpacked(t / 'bad.zip', Path(tempfile.mkdtemp(dir=t)))
        self.assertEqual(saves._unpacked(t, None), t)   # a folder: as it is


if __name__ == '__main__':
    unittest.main()
