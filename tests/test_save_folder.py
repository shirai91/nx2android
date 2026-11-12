"""The save file directory and saves as .zip files (SaveBridge.java, SaveZip.java, saves_android.c, porter's java_bridge), checked without a device."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from porter import cli, pipeline

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'nxnative/dm/android'
JAVA = ANDROID / 'java/org/nx2a'
NO_PREBUILT = Path('/no/prebuilt/classes.dex')


class JavaBridgeBuild(unittest.TestCase):
    """porter builds the app's Java whenever it can, and a port that does not ask for Java never fails for it"""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
    def tearDown(self): self.tmp.cleanup()

    def sdk(self):
        for name in ('platforms/android-35/android.jar', 'build-tools/35.0.0/d8', 'build-tools/35.0.0/d8.bat'):
            p = self.root / 'sdk' / name; p.parent.mkdir(parents=True, exist_ok=True); p.touch()
        return [self.root / 'sdk']

    def bridge(self, roots, javac='javac', prebuilt=NO_PREBUILT, run=None, **options):
        port = pipeline.Port(pipeline.Options(folder=self.root, **options)); port.aw = self.root / 'out'; port.logs = self.root / 'logs'; port.env = {}
        with mock.patch.object(pipeline.toolchain, '_sdk_roots', return_value=roots), mock.patch.object(pipeline.shutil, 'which', return_value=javac), \
             mock.patch.object(pipeline, '_jdk_env', side_effect=lambda j, env: dict(env or {})), mock.patch.object(pipeline, 'PREBUILT_DEX', prebuilt), \
             mock.patch.object(pipeline, '_run_java', **({'side_effect': run} if run else {})) as java:
            return port.java_bridge(), java

    def test_sdk_builds_the_save_bridge_activity(self):
        args, java = self.bridge(self.sdk())
        self.assertEqual(args[0], '--dex'); self.assertEqual(args[-2:], ['--activity', 'org.nx2a.GameActivity'])
        javac = [str(x) for x in java.call_args_list[0].args[0]]
        self.assertEqual(javac[javac.index('-encoding') + 1], 'UTF-8')   # (the sources are UTF-8 whatever the PC's code page)
        self.assertTrue(any(x.endswith('SaveBridge.java') for x in javac) and any(x.endswith('SaveZip.java') for x in javac))

    def test_no_sdk_and_no_prebuilt_dex_stays_code_less(self):
        args, java = self.bridge([], javac=None)
        self.assertEqual(args, []); java.assert_not_called()

    def test_a_bundles_prebuilt_dex_needs_no_sdk(self):
        dex = self.root / 'classes.dex'; dex.write_bytes(b'dex')
        args, _ = self.bridge([], javac=None, prebuilt=dex)
        self.assertEqual(args, ['--dex', dex, '--activity', 'org.nx2a.GameActivity'])
        args, _ = self.bridge([], javac=None, prebuilt=dex, native_keyboard=True)
        self.assertEqual(args[-1], 'org.nx2a.KeyboardGameActivity')

    def test_no_java_option(self):
        self.assertTrue(cli._parser().parse_args(['port', '/game', '--no-java']).no_java)
        self.assertFalse(pipeline.Options(folder=Path('/game')).no_java)
        args, java = self.bridge(self.sdk(), no_java=True)
        self.assertEqual(args, []); java.assert_not_called()

    def test_a_failed_build_only_fails_ports_that_ask_for_java(self):
        def fail(*a, **k): raise pipeline.BuildError('d8 exit code 1')
        args, _ = self.bridge(self.sdk(), run=fail)
        self.assertEqual(args, [])
        with self.assertRaises(pipeline.BuildError): self.bridge(self.sdk(), run=fail, native_keyboard=True)


class BridgeContract(unittest.TestCase):
    """saves_android.c finds SaveBridge's methods by name and JNI descriptor, and both sides number the answers alike"""
    def test_native_lookups_match_the_java_methods(self):
        c, java = (ANDROID / 'saves_android.c').read_text(encoding='utf-8'), (JAVA / 'SaveBridge.java').read_text(encoding='utf-8')
        types = {'String': 'Ljava/lang/String;', 'int': 'I', 'boolean': 'Z', 'void': 'V', 'byte[]': '[B'}
        found = re.findall(r'GetMethodID\(E, bc, "(\w+)", "([^"]+)"\)', c)
        self.assertEqual(len(found), 6)
        for name, desc in found:
            m = re.search(r'public (?:synchronized )?(\S+) ' + name + r'\(([^)]*)\)', java)
            self.assertTrue(m, f'SaveBridge.{name} is not public')
            args = ''.join(types[a.split()[0]] for a in m.group(2).split(',') if a.strip())
            self.assertEqual(desc, f'({args}){types[m.group(1)]}', name)
        self.assertIn('"saveBridge", "()Lorg/nx2a/SaveBridge;"', c)
        for activity in ('GameActivity', 'ExternalActivity'):
            source = (JAVA / f'{activity}.java').read_text(encoding='utf-8')
            self.assertIn('public SaveBridge saveBridge()', source); self.assertIn('saves.onActivityResult(', source)
        self.assertIn('private static native void done(int kind, int code, byte[] message);', java)
        self.assertIn('Java_org_nx2a_SaveBridge_done(JNIEnv *E, jclass cls, jint kind, jint code, jbyteArray message)', c)

    def test_answers_are_numbered_alike(self):
        h, java = (ANDROID / 'saves_android.h').read_text(encoding='utf-8'), (JAVA / 'SaveBridge.java').read_text(encoding='utf-8')
        self.assertIn('enum { DM_SAVES_EXPORT = 1, DM_SAVES_IMPORT, DM_SAVES_FOLDER };', h)
        self.assertIn('static final int EXPORT = 1, IMPORT = 2, FOLDER = 3;', java)
        self.assertIn('enum { DM_SAVES_OK, DM_SAVES_CANCELLED, DM_SAVES_LOAD, DM_SAVES_DEFAULT, DM_SAVES_FAILED = -1 };', h)
        self.assertIn('static final int OK = 0, CANCELLED = 1, LOAD = 2, DEFAULT = 3, FAILED = -1;', java)

    def test_keyboard_stays_opt_in(self):   # GameActivity is now every Java port's: the keyboard is its subclass's, chosen by --native-keyboard
        self.assertNotIn('readKeyboard', (JAVA / 'GameActivity.java').read_text(encoding='utf-8'))
        keyboard = (JAVA / 'KeyboardGameActivity.java').read_text(encoding='utf-8')
        self.assertIn('extends GameActivity', keyboard); self.assertIn('public byte[] readKeyboard(', keyboard)


ZIP_CHECK = r'''package org.nx2a;
import java.util.*;
public final class ZipCheck {
    static void ok(boolean b, String what) { if (!b) throw new AssertionError(what); }
    public static void main(String[] a) {
        String T = "010069401ADB8000", P = T + "/";
        SaveZip.Layout l = SaveZip.scan(Arrays.asList(P, P + "UCGLOBALDATA.DAT", P + "UCSAVEFILE00_AUTOSAVE.DAT", P + "UCSAVEFILE01.DAT"), T);
        ok(P.equals(l.prefix) && l.files == 3 && l.others.isEmpty() && l.unsafe == null, "the export's own layout");
        String W = "Unicorn Overlord save data - 2026-11-11 21_52/" + T.toLowerCase(Locale.US) + "/";
        l = SaveZip.scan(Arrays.asList(W, W + "UCSAVEFILE01.DAT"), T);
        ok(W.equals(l.prefix) && l.files == 1, "a zip made of the folder that holds the save, its name's case changed");
        l = SaveZip.scan(Arrays.asList("0100000000010000/", "0100000000010000/save.bin"), T);
        ok(l.prefix == null && l.others.equals(new TreeSet<>(Arrays.asList("0100000000010000"))), "another game's save: refused, its title ID named");
        ok(SaveZip.scan(Arrays.asList("UCSAVEFILE01.DAT"), T).prefix == null, "loose files: refused");
        ok(SaveZip.scan(Arrays.asList("a/b/" + P + "X"), T).prefix == null, "deeper than one folder: refused");
        ok(SaveZip.scan(Arrays.asList(P + "../escape"), T).unsafe != null, "a path that leaves the folder");
        l = SaveZip.scan(Arrays.asList("__MACOSX/" + P + "._X", P + "X", T + ".device/Y"), T);
        ok(P.equals(l.prefix) && l.files == 1 && l.others.isEmpty(), "macOS's extra folder, and the device save beside the save");
        ok("x/y".equals(SaveZip.inside(P + "x/y", P)) && SaveZip.inside(P, P) == null && SaveZip.inside(P + "a/../b", P) == null && SaveZip.inside("other/x", P) == null, "inside");
        ok((T + ".device/").equals(SaveZip.devicePrefix(P)) && "z".equals(SaveZip.inside(T + ".device/z", SaveZip.devicePrefix(P))), "the device save");
        ok("a/b".equals(SaveZip.norm("\\a\\b")), "a Windows zip's names");
    }
}
'''


class SaveZipRules(unittest.TestCase):
    @unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'a JDK is needed')
    def test_where_the_save_is_in_a_zip(self):
        with tempfile.TemporaryDirectory() as t:
            check = Path(t) / 'ZipCheck.java'; check.write_text(ZIP_CHECK, encoding='utf-8')
            # Java 8 class files: a JRE 8 on PATH beside a newer javac runs them too
            subprocess.run(['javac', '-encoding', 'UTF-8', '-source', '8', '-target', '8', '-nowarn', '-d', t, str(JAVA / 'SaveZip.java'), str(check)], check=True, capture_output=True)
            r = subprocess.run(['java', '-ea', '-cp', t, 'org.nx2a.ZipCheck'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


FOLDER_DECIDE = r'''
#include <assert.h>
#include "saves_android.h"
int main(void) {
    assert(dm_folder_decide(0, 0, 5, 0, 0) == DM_FOLDER_PUSH);   // no save of this game in the folder yet: the app's goes there
    assert(dm_folder_decide(1, 7, 7, 0, 0) == DM_FOLDER_KEEP);   // the same save
    assert(dm_folder_decide(1, 7, 8, 0, 0) == DM_FOLDER_LOAD);   // a folder just chosen, another save in it: the folder's (the app's backed up first)
    assert(dm_folder_decide(1, 7, 8, 1, 7) == DM_FOLDER_PUSH);   // only the app's changed since the last sync (a session that ended before its sync)
    assert(dm_folder_decide(1, 9, 7, 1, 7) == DM_FOLDER_LOAD);   // another device changed the folder
    assert(dm_folder_decide(1, 9, 8, 1, 7) == DM_FOLDER_LOAD);   // both changed: the folder wins, the app's is backed up
    return 0;
}
'''


class FolderDecision(unittest.TestCase):
    def test_which_save_wins_at_the_start(self):
        cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc: self.skipTest('a C compiler is needed')
        with tempfile.TemporaryDirectory() as t:
            c, exe = Path(t) / 'decide.c', Path(t) / 'decide'
            c.write_text(FOLDER_DECIDE, encoding='utf-8')
            r = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', f'-I{ANDROID}', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
