# Unit tests of the port pipeline's own parts (python -m unittest discover tests). They need no game files, toolchains or device.
import json, os, re, shutil, sys, tempfile, unittest
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

    def test_default_package(self):
        d = profile_dir([('status', 'boots')])
        self.assertEqual(android_profile.load(d.name).package, 'org.nx2a.t0100000000aaa000')

    def test_bad_keys_are_refused(self):
        for rows in ([('status', 'great')], [('colour', 'red')], [('setting', 'volume=3')], [('overlay', 'q 0.1 0.2')], [('env', 'lower=1')], [('package', 'Not A Package')],
                     [('status', 'boots'), ('status', 'playable')]):
            d = profile_dir(rows)
            with self.assertRaises(android_profile.ProfileError, msg=rows): android_profile.load(d.name)

    def test_header_default_layout(self):   # the generated table is overlay_layout.h's table as it was written by hand before it moved to overlay_default.tsv
        h = android_profile.header(android_profile.load(NEO))
        rows = re.findall(r'\{(OV_\w+), ([\d.]+)f, ([\d.]+)f, ([\d.]+)f, (\w+), "([^"]*)"\}', h)
        self.assertEqual(len(rows), 16)
        self.assertEqual(rows[0], ('OV_STICK_L', '0.12', '0.70', '0.12', '0', ''))
        self.assertEqual(rows[2], ('OV_BUTTON', '0.935', '0.60', '0.050', 'DM_BTN_A', 'A'))
        self.assertEqual(rows[10][-1], '-')   # the minus button's label, not "no label"

    def test_header_overlay_move(self):
        d = profile_dir([('status', 'boots'), ('overlay', 'a 0.5 0.5 0.07')])
        h = android_profile.header(android_profile.load(d.name))
        self.assertIn('{OV_BUTTON, 0.5f, 0.5f, 0.07f, DM_BTN_A, "A"}', h)


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
        odyssey = '0100000000010000'
        t = Path(tempfile.mkdtemp(prefix='nx2a_test_exp_')); environ = dict(os.environ)
        try:
            port = pipeline.Port(pipeline.Options(folder=t, experimental=True)); port.env = {}; port.work = t / f'{odyssey}_build'; port.o.keys = None
            g, ap = port.experimental_profile(game.load(odyssey), odyssey)
            self.assertEqual(g.dir, (t / f'{odyssey}_build/profile/{odyssey}').resolve())
            self.assertEqual((ap.status, ap.package), ('experimental', f'org.nx2a.t{odyssey}'))
            self.assertFalse((REPO / f'nxnative/dm/games/{odyssey}/android.tsv').exists())
            self.assertFalse((g.dir / 'patches.py').exists())   # patch_nso.py finds the per-build cuts in dm/games
            self.assertEqual(os.environ.get('DM_GAMES_FIRST'), '1')
        finally:
            os.environ.clear(); os.environ.update(environ); shutil.rmtree(t, ignore_errors=True)


class Misc(unittest.TestCase):
    def test_apk_driver_libraries_and_missing_bundle(self):
        from types import SimpleNamespace as NS
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as td:
            t = Path(td); libs = t / 'driver'; libs.mkdir()
            for name in ('libadrenotools.so', 'libvulkan_freedreno.so'): (libs / name).write_bytes(b'ELF')
            p = pipeline.Port(pipeline.Options(folder=t, out=t, app_name='Test'))
            p.g = game.load(NEO); p.app_name = 'Test'; p.package = 'org.nx2a.test'; p.pkg_ver = 'v0'; p.display = '1.0'
            p.ap = NS(landscape=True); p.so = t / 'host.so'; p.logs = t; p.env = {}; p.icon = lambda: None
            def package(args, *a, **kw):
                Path(args[3]).write_bytes(b'APK')
            with patch.dict(os.environ, {'DM_VK_DRIVER_LIBS': str(libs)}), patch.object(pipeline.nsp, 'run', side_effect=package) as run:
                p.apk()
                args = run.call_args.args[0]
                self.assertEqual([args[i + 1].name for i, a in enumerate(args) if a == '--extra-so'], ['libadrenotools.so', 'libvulkan_freedreno.so'])
                self.assertTrue(p.apk_path.is_file())
            with patch.dict(os.environ, {'DM_VK_DRIVER_LIBS': ''}), patch.object(pipeline.nsp, 'run', side_effect=package) as run:
                p.apk()
                self.assertNotIn('--extra-so', run.call_args.args[0])
            with patch.dict(os.environ, {'DM_VK_DRIVER_LIBS': str(t / 'missing')}), patch.object(pipeline.nsp, 'run') as run:
                with self.assertRaises(pipeline.BuildError): p.apk()
                run.assert_not_called()

    def test_config_roundtrip(self):
        config.set_value('device', 'abc:5555'); self.assertEqual(config.load()['device'], 'abc:5555')
        with self.assertRaises(KeyError): config.set_value('nope', 'x')

    def test_version_code_grows_with_updates(self):
        self.assertLess(pipeline.version_code('v0'), pipeline.version_code('v65536'))
        self.assertEqual(pipeline.version_code('v196608'), 196609)

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


if __name__ == '__main__':
    unittest.main()
