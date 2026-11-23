"""Erase (saves_android.c): the save is exported first, then erased at the next start, and nothing is erased when the export fails. The unit itself is built
on the host with stubs for what it takes from the rest of the app, so it needs a POSIX C compiler and a JDK's jni.h (as the other Android tests do)."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'nxnative/dm/android'

HARNESS = r'''
#include <assert.h>
#include <sys/types.h>
#include "saves_android.c"

uint64_t dm_fs_commit_count(void) { return 0; }
void dm_saves_event(int kind, int code, const char *message) { (void)kind; (void)code; (void)message; }

static char root[400], files[420], save[430], dev[440];
static void put(const char *dir, const char *name, const char *text) {
    char p[900]; mkdir(dir, 0777); snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); assert(f); fputs(text, f); fclose(f);
}
static void fresh_save(void) {   // a save with a file, a folder and a device save
    rm_tree(save); rm_tree(dev);
    put(save, "main.dat", "progress"); char sub[460]; snprintf(sub, sizeof sub, "%s/profile", save); put(sub, "slot0.dat", "slot zero"); put(dev, "device.dat", "device");
}
static int backups(char *first) {   // saves/<stamp>-before-erase folders (not their .device ones): how many, and the first's name
    DIR *d = opendir(g_dir); struct dirent *e; int n = 0; if (first) first[0] = 0;
    while (d && (e = readdir(d))) { size_t L = strlen(e->d_name);
        if (L > 13 && !strncmp(e->d_name + L - 13, "-before-erase", 13)) { if (first && !n) snprintf(first, 100, "%s", e->d_name); n++; } }
    if (d) closedir(d); return n;
}
static int result_has(const char *needle) {
    char p[700], buf[2000] = ""; snprintf(p, sizeof p, "%s/result.txt", g_dir);
    FILE *f = fopen(p, "r"); if (!f) return 0; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); return strstr(buf, needle) != NULL;
}
static void run_erase(void) { assert(dm_saves_request_erase() == 0); assert(dm_saves_start(files) == 0); }   // (0: the app does not end)

int main(int argc, char **argv) {
    assert(argc == 2);
    if (geteuid() == 0) { assert(setgid(65534) == 0 && setuid(65534) == 0); }   // a root could read the file the failure case makes unreadable
    snprintf(root, sizeof root, "%s", argv[1]); snprintf(files, sizeof files, "%s/files", root); snprintf(save, sizeof save, "%s/save", root); snprintf(dev, sizeof dev, "%s/save_device", root);
    mkdir(files, 0777); setenv("DM_SAVE", save, 1); dm_saves_paths(files);
    char bk[100];

    // 1. a save and a device save: exported first, then erased; the export holds every file; the result says so
    fresh_save();
    dm_save_entry cur; assert(dm_saves_current(&cur) == 1 && cur.files == 3 && cur.device == 1);
    run_erase();
    assert(!is_dir(save) && !is_dir(dev));
    assert(backups(bk) == 1);
    char p[900], q[900]; snprintf(p, sizeof p, "%s/%s/main.dat", g_dir, bk); assert(!access(p, R_OK));
    snprintf(p, sizeof p, "%s/%s/profile/slot0.dat", g_dir, bk); assert(!access(p, R_OK));
    snprintf(p, sizeof p, "%s/%s.device/device.dat", g_dir, bk); assert(!access(p, R_OK));
    assert(result_has("result=ok") && result_has("-before-erase") && result_has("files=3"));
    assert(dm_saves_current(&cur) == 0);   // nothing now

    // 2. the backup is a save Import save can take back: it is listed, with its device save
    dm_save_entry list[8]; int n = dm_saves_list(list, 8), found = 0;
    for (int i = 0; i < n; i++) if (!strcmp(list[i].name, bk)) { found = 1; assert(list[i].files == 3 && list[i].device == 1); }
    assert(found);

    // 3. nothing saved yet: nothing to export, nothing to erase, and no new backup; still ok
    run_erase();
    assert(backups(NULL) == 1 && result_has("result=ok") && result_has("files=0"));

    // 4. a save that cannot be exported (a file nobody can read): refused, and the save is still there
    fresh_save(); snprintf(p, sizeof p, "%s/main.dat", save); assert(!chmod(p, 0));
    run_erase();
    assert(is_dir(save) && is_dir(dev));
    assert(backups(NULL) == 1);   // (still only the first one: the failed export left no folder)
    assert(result_has("result=error") && result_has("nothing was erased"));
    assert(chmod(p, 0600) == 0); snprintf(q, sizeof q, "%s/profile/slot0.dat", save); assert(!access(q, R_OK) && !access(dev, R_OK));

    // 5. a request that ends the app leaves a save folder alone and erases nothing more: a second erase.txt without a save is still ok
    rm_tree(save); rm_tree(dev); run_erase(); assert(backups(NULL) == 1 && result_has("result=ok"));
    puts("ok"); return 0;
}
'''


def jni_dirs():
    home = os.environ.get('JAVA_HOME')
    if not home and shutil.which('javac'): home = str(Path(shutil.which('javac')).resolve().parents[1])
    inc = Path(home or '/nonexistent') / 'include'
    return [inc] + sorted(p for p in inc.glob('*') if p.is_dir()) if (inc / 'jni.h').exists() else []


class EraseSave(unittest.TestCase):
    @unittest.skipUnless(sys.platform.startswith('linux'), 'saves_android.c swaps the save with renameat2, which only Linux (and Android) has')
    def test_erase_exports_first_then_erases(self):
        cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc: self.skipTest('a C compiler is needed')
        jni = jni_dirs()
        if not jni: self.skipTest('JDK JNI headers required')
        with tempfile.TemporaryDirectory() as t:
            t = Path(t); stub = t / 'stub'; stub.mkdir()
            (stub / 'dm.h').write_text('#pragma once\n#include <stdio.h>\n#include <stdint.h>\n#define LOG(...) do { fprintf(stderr, "[dm] " __VA_ARGS__); fputc(10, stderr); } while (0)\n'
                                       '#define DM_GAME_ID "0100000000000001"\n', encoding='utf-8')
            c, exe, work = t / 'clear.c', t / 'clear', t / 'work'
            c.write_text(HARNESS, encoding='utf-8'); work.mkdir(); os.chmod(t, 0o777); os.chmod(work, 0o777)
            # (no -Werror: the unit's own warnings under another jni.h: the JDK's AttachCurrentThread takes void **, the NDK's JNIEnv **)
            r = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wno-unused-function', '-Wno-misleading-indentation', '-Wno-incompatible-pointer-types', f'-I{stub}', f'-I{ANDROID}', *[f'-I{d}' for d in jni], str(c), '-o', str(exe), '-lpthread'],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe), str(work)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('ok', r.stdout)


if __name__ == '__main__':
    unittest.main()
