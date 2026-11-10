"""Physical controller profiles: the table (dm/android/controllers.tsv), the player's own files (porter/controllers.py), the data pack's controller.tsv, and the app's
key mapping (front_android.c pad_load / key_bit) compiled from the production source. No device or game is needed."""
import os, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
os.environ.setdefault('NX2A_HOME', tempfile.mkdtemp(prefix='nx2a_test_home_'))

from porter import controllers   # noqa: E402

A, B, X, Y, L, R, ZL, ZR = 1, 2, 4, 8, 64, 128, 256, 512   # dm_input.h DM_BTN_*
KA, KB, KX, KY = 96, 97, 99, 100                             # AKEYCODE_BUTTON_A, _B, _X, _Y


class Table(unittest.TestCase):
    def test_builtin(self):   # every row parses and resolves; the generic ones are there, the standard one first (the GUI's default)
        t = controllers.builtin()
        self.assertEqual(list(t)[0], controllers.DEFAULT)
        for g in ('standard', 'nintendo', 'nintendo-label', 'playstation', 'ayn-thor', 'switch-pro', 'dualsense', 'xbox'): self.assertIn(g, t)
        for c in t.values():
            rows = {k: p for k, p, _ in c.rows()}
            self.assertEqual(sorted(rows[k] for k in (KA, KB, KX, KY)), sorted(controllers.FACE), c.id)   # each face key at one place, each place once
            head = c.pack_text(c.id).splitlines()
            self.assertTrue(head[0].startswith('# Written by nx2android') and f'--controller {c.id})' in head[0]); self.assertEqual(head[2:4], [f'name\t{c.name}', f'labels\t{c.labels}'])

    def test_places(self):   # Android's standard: A at the bottom; Nintendo letters by label: A on the right, X on top
        t = controllers.builtin()
        self.assertEqual([p for _, p, _ in t['standard'].rows()], ['south', 'east', 'west', 'north'])
        self.assertEqual([p for _, p, _ in t['switch-pro'].rows()], ['south', 'east', 'west', 'north'])   # AOSP's key layout: the bottom button (B) sends BUTTON_A
        self.assertEqual([p for _, p, _ in t['ayn-thor'].rows()], ['east', 'south', 'north', 'west'])
        text = t['ayn-thor'].pack_text('ayn-thor')
        self.assertIn('labels\tnintendo\n', text); self.assertIn('key\t96\teast\tBUTTON_A\n', text); self.assertIn('key\t100\twest\tBUTTON_Y\n', text)

    def test_resolve(self):
        self.assertEqual(controllers.resolve('')[0].id, 'standard'); self.assertEqual(controllers.resolve(None)[1], 'standard')
        with self.assertRaisesRegex(controllers.ControllerError, 'no controller profile'): controllers.resolve('no-such-pad')

    def test_own_file(self):   # base, name, labels, keys and map rows; maps win over the face keys; names, codes and KEYCODE_ prefixes
        d = Path(tempfile.mkdtemp(prefix='nx2a_test_pad_'))
        try:
            f = d / 'my thor.tsv'
            f.write_text('# mine\nbase\tayn-thor\nname\tMy Thor\nmap\tBUTTON_C\tZL\nmap\tKEYCODE_BUTTON_L2\tnone\nmap\t188\teast\nmap\tBUTTON_A\tA\n', encoding='utf-8')
            c, spec = controllers.resolve(str(f))
            self.assertEqual((c.name, c.labels, c.keys, spec), ('My Thor', 'nintendo', 'label', str(f.resolve())))
            rows = {k: t for k, t, _ in c.rows()}
            self.assertEqual((rows[98], rows[104], rows[188], rows[KA], rows[KB]), ('ZL', 'none', 'east', 'A', 'south'))
            for bad, why in (('map\tBUTTON_MODE\tA\n', 'pause menu'), ('map\tBUTTON_C\tC\n', 'unknown target'), ('map\tTRIGGER\tA\n', 'unknown key'),
                             ('labels\tplaystation\nkeys\tlabel\n', 'letters'), ('labels\tsega\n', 'labels'), ('map BUTTON_C ZL\n', 'tab-separated'), ('base\tnope\n', 'base')):
                f.write_text(bad, encoding='utf-8')
                with self.assertRaisesRegex(controllers.ControllerError, why): controllers.resolve(str(f))
            with self.assertRaisesRegex(controllers.ControllerError, 'cannot read'): controllers.resolve(str(d / 'missing.tsv'))
        finally: shutil.rmtree(d, ignore_errors=True)


class Port(unittest.TestCase):
    def test_cli(self):
        from porter import cli
        self.assertEqual(cli._parser().parse_args(['port', 'x', '--controller', 'ayn-thor']).controller, 'ayn-thor')
        self.assertEqual(cli._parser().parse_args(['port', 'x']).controller, '')
        self.assertEqual(cli._parser().parse_args(['controllers', '--json']).cmd, 'controllers')

    def test_bad_profile_fails_before_any_stage(self):
        try: from porter import pipeline
        except ImportError as e: self.skipTest(f'the pipeline needs requirements.txt: {e}')
        t = Path(tempfile.mkdtemp(prefix='nx2a_test_game_'))
        try:
            with self.assertRaisesRegex(pipeline.BuildError, 'controller'): pipeline.Port(pipeline.Options(t, controller='no-such-pad')).run()
        finally: shutil.rmtree(t, ignore_errors=True)

    def test_data_pack(self):   # always written: a profile an earlier install left on the device never outlives the port
        try: from porter import datapack, profiles
        except ImportError as e: self.skipTest(f'the pipeline needs requirements.txt: {e}')
        t = Path(tempfile.mkdtemp(prefix='nx2a_test_pack_'))
        try:
            (t / 'patched').mkdir(); (t / 'patched/main.patched.bin').write_bytes(b'x'); (t / 'romfs').mkdir(); (t / 'shaders.dmsv').write_bytes(b'DMSV')
            c, spec = controllers.resolve('ayn-thor')
            datapack.build(t / 'out-data', profile=profiles.load('010043B013C5C000'), version='v0', display='1.0', patched=t / 'patched', romfs=t / 'romfs',
                           shaders=t / 'shaders.dmsv', controller=c.pack_text(spec))
            self.assertEqual((t / 'out-data/controller.tsv').read_text(encoding='utf-8'), c.pack_text(spec))
        finally: shutil.rmtree(t, ignore_errors=True)


class App(unittest.TestCase):
    """front_android.c's pad_labels .. key_bit, compiled with stand-ins for the Android headers"""
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc: raise unittest.SkipTest('a C compiler is required')
        front = (REPO / 'nxnative/dm/android/front_android.c').read_text(encoding='utf-8')
        code = front[front.index('static int pad_labels(void)'):front.index('static int32_t axis(float v)')]
        harness = r'''
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
enum { AKEYCODE_DPAD_UP = 19, AKEYCODE_DPAD_DOWN, AKEYCODE_DPAD_LEFT, AKEYCODE_DPAD_RIGHT, AKEYCODE_BUTTON_A = 96, AKEYCODE_BUTTON_B, AKEYCODE_BUTTON_C, AKEYCODE_BUTTON_X,
       AKEYCODE_BUTTON_Y, AKEYCODE_BUTTON_Z, AKEYCODE_BUTTON_L1, AKEYCODE_BUTTON_R1, AKEYCODE_BUTTON_L2, AKEYCODE_BUTTON_R2, AKEYCODE_BUTTON_THUMBL, AKEYCODE_BUTTON_THUMBR,
       AKEYCODE_BUTTON_START, AKEYCODE_BUTTON_SELECT };
static _Atomic int g_set_labels = -1;
''' + code + r'''
int main(int argc, char **argv) {   // <controller.tsv or -> <labels 0/1> <key code> ...: prints each key's DM_BTN bits
    if (strcmp(argv[1], "-")) pad_load(argv[1]);
    atomic_store(&g_set_labels, atoi(argv[2]));
    for (int i = 3; i < argc; i++) printf("%llu\n", (unsigned long long)key_bit(atoi(argv[i])));
    return 0;
}
'''
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_pad_'); cls.addClassCleanup(cls.tmp.cleanup)
        c = Path(cls.tmp.name) / 'pad.c'; cls.exe = Path(cls.tmp.name) / 'pad'
        c.write_text(harness, encoding='utf-8')
        r = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I' + str(REPO / 'nxnative/dm/host'), str(c), '-o', str(cls.exe)], capture_output=True, text=True)
        if r.returncode: raise AssertionError(r.stdout + r.stderr)

    def keys(self, profile, labels, *codes, text=None):
        f = '-'
        if profile or text:
            f = Path(self.tmp.name) / 'controller.tsv'
            f.write_text(text if text is not None else (lambda c: c[0].pack_text(c[1]))(controllers.resolve(profile)), encoding='utf-8')
        r = subprocess.run([str(self.exe), str(f), str(labels), *map(str, codes)], capture_output=True, text=True, check=True)
        return [int(x) for x in r.stdout.split()], r.stderr

    def test_standard(self):   # no file (a pack made before the profiles) and the standard profile: as before, the bottom button is B by position, A by label
        for p in (None, 'standard', 'xbox', 'dualsense'):
            self.assertEqual(self.keys(p, 0, KA, KB, KX, KY)[0], [B, A, Y, X], p)
            self.assertEqual(self.keys(p, 1, KA, KB, KX, KY)[0], [A, B, X, Y], p)
        self.assertEqual(self.keys(None, 0, 102, 103, 104, 105, 108, 109)[0], [L, R, ZL, ZR, 1024, 2048])

    def test_nintendo_letters(self):   # position and label agree: A is the right button whichever key it sends
        for p in ('ayn-thor', 'nintendo-label', 'retroid', 'anbernic'):   # keys by label: BUTTON_A from the right (A) button
            for lab in (0, 1): self.assertEqual(self.keys(p, lab, KA, KB, KX, KY)[0], [A, B, X, Y], (p, lab))
        for p in ('switch-pro', 'ayn-thor-xbox', 'nintendo'):   # Android's standard keys: BUTTON_A from the bottom (B) button
            for lab in (0, 1): self.assertEqual(self.keys(p, lab, KA, KB, KX, KY)[0], [B, A, Y, X], (p, lab))

    def test_maps(self):   # a key to a Switch button, to nothing (the system's), to a face place; a line the app does not understand is skipped, red, and the rest load
        text = 'name\tmine\nlabels\tnintendo\nkey\t96\teast\nkey\t98\tZL\nkey\t104\tnone\nkey\t188\tsouth\tBUTTON_1\nkey\t99\tTRIGGER\nkey\t9999\tA\n'
        got, log = self.keys(None, 0, KA, 98, 104, 188, KX, text=text)
        self.assertEqual(got, [A, ZL, 0, B, Y])
        self.assertEqual(log.count('[RED]'), 2); self.assertIn('controller profile mine: Nintendo letters, 4 keys', log)


if __name__ == '__main__': unittest.main()
