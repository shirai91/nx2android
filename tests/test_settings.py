# The settings registry (nxnative/dm/settings.tsv, docs/ARCHITECTURE.md Settings) against the core, the profiles and the porter: every switch the core reads has a row,
# every row is read, and profiles and the porter set only switches of their scope with values of the row's type.
import re, subprocess, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DM = REPO / 'nxnative/dm'
sys.path.insert(0, str(DM / 'android/tools'))
import settings_registry   # noqa: E402
sys.path.insert(0, str(REPO / 'tests'))
import host_cc             # noqa: E402


def core_sources():
    for p in sorted(DM.rglob('*')):
        rel = p.relative_to(DM).as_posix()
        if p.suffix in ('.c', '.h', '.inc', '.m') and not rel.startswith(('third_party/', 'games/')) and '/tests/' not in f'/{rel}' and p.is_file():
            yield rel, p.read_text(encoding='utf-8', errors='replace')


class SettingsRegistry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reg = settings_registry.load()
        cls.src = dict(core_sources())

    def test_rows_parse_and_are_sorted(self):
        names = [l.split('\t', 1)[0] for l in settings_registry.PATH.read_text(encoding='utf-8').splitlines() if l.startswith('DM_')]
        self.assertEqual(names, sorted(names), 'settings.tsv rows are kept in name order (fewer merge conflicts)')
        self.assertGreater(len(self.reg), 300)

    def test_every_switch_the_core_reads_or_sets_has_a_row(self):
        missing = {}
        for rel, s in self.src.items():
            for n in re.findall(r'"(DM_[A-Z0-9_]+)"', s):   # getenv, setenv and the helpers that take a switch's name (hle_fs.c envdir, hle_sdk16.c font_first)
                if n not in self.reg: missing.setdefault(n, rel)
        self.assertFalse(missing, 'switches with no row in nxnative/dm/settings.tsv (add one: type, default, scope, doc): ' + ', '.join(f'{n} ({f})' for n, f in sorted(missing.items())))

    def test_every_row_is_read_somewhere(self):
        text = '\n'.join(self.src.values())
        dead = [n for n in self.reg if f'"{n}"' not in text]
        self.assertFalse(dead, f'rows no core source names (remove them with the code that read them): {dead}')

    def test_ios_flag_matches_the_sources(self):
        for n, s in self.reg.items():
            files = [rel for rel, t in self.src.items() if f'"{n}"' in t]
            only_m = bool(files) and all(f.endswith('.m') for f in files)
            self.assertEqual(s.ios, only_m, f'{n}: ",ios" in its scope says whether only the iOS host reads it ({files})')

    def test_profiles_set_only_profile_switches(self):
        bad = []
        for a in sorted((DM / 'games').glob('*/android.tsv')):
            rows = [v for l in a.read_text(encoding='utf-8').splitlines() if l.startswith('env\t') for v in l.split('\t', 1)[1].split()]
            bad += settings_registry.check_env(rows, ('profile',), f'{a.parent.name} ')
        self.assertFalse(bad, '\n'.join(bad))

    def test_porter_writes_only_profile_or_porter_switches(self):
        rows = []
        for p in sorted((REPO / 'porter').glob('*.py')):
            rows += [m for m in re.findall(r"""['"](DM_[A-Z0-9_]+)=""", p.read_text(encoding='utf-8'))]
        bad = settings_registry.check_env([f'{n}=' for n in rows], ('profile', 'porter', 'app'), 'porter ')
        bad = [b for b in bad if 'want' not in b]   # values are made at run time
        self.assertFalse(bad, '\n'.join(bad))

    def test_check_env_catches_typos_scope_and_type(self):
        bad = settings_registry.check_env(['DM_VK_CB_DRAWS=32', 'DM_VK_FDZ=2', 'DM_NO_SUCH=1', 'HOME=/x'], ('profile',))
        self.assertEqual(len(bad), 2, bad)
        debug = next(n for n, s in self.reg.items() if s.scope == 'debug')
        self.assertTrue(settings_registry.check_env([f'{debug}=1'], ('profile',)))

    def test_android_build_forces_the_runtime_header(self):
        b = (DM / 'android/build.py').read_text(encoding='utf-8')
        self.assertIn("'-include', str(DM / 'android/dm_settings.h')", b)
        h = (DM / 'android/dm_settings.h').read_text(encoding='utf-8')
        self.assertLess(h.index('#include <stdlib.h>'), h.index('#define getenv'), 'stdlib.h comes before the getenv macro, or its declaration is rewritten')


RUNTIME = r"""
#include <stdio.h>
#include <string.h>
static const char *read_cb(void) { return getenv("DM_VK_CB_DRAWS"); }   /* one call site: one cache slot */
int main(void) {
    if (read_cb()) { puts("set at start"); return 1; }
    setenv("DM_VK_CB_DRAWS", "32", 1);
    const char *a = read_cb(), *b = read_cb();
    if (!a || strcmp(a, "32") || a != b) { puts("cached value"); return 1; }
    setenv("DM_VK_CB_DRAWS", "32", 1);   /* the same value: no new generation needed, the same string */
    if (read_cb() != a) { puts("same value re-read"); return 1; }
    setenv("DM_VK_CB_DRAWS", "0", 1);
    if (strcmp(read_cb(), "0")) { puts("a change is seen"); return 1; }
    if (strcmp(a, "32")) { puts("an old value stays readable"); return 1; }
    setenv("DM_VK_CB_DRAWS", "64", 0);   /* overwrite 0 keeps the value */
    if (strcmp(read_cb(), "0")) { puts("overwrite 0"); return 1; }
    unsetenv("DM_VK_CB_DRAWS");
    if (read_cb()) { puts("unset"); return 1; }
    setenv("DM_NO_SUCH_SETTING", "1", 1); setenv("DM_NO_SUCH_SETTING", "2", 1);   /* logged once */
    if (!dm_setting_known("DM_VK_CB_DRAWS") || dm_setting_known("DM_NO_SUCH_SETTING")) { puts("known"); return 1; }
    const char *h = "HOME_X"; setenv(h, "y", 1); if (strcmp(getenv(h), "y")) { puts("a name in a variable"); return 1; }
    puts("settings runtime ok"); return 0;
}
"""


@unittest.skipUnless(host_cc.posix(), 'a POSIX host C compiler is required')
class SettingsRuntime(unittest.TestCase):
    def test_cache_follows_setenv_and_flags_unknown_names(self):
        names = sorted(settings_registry.load())
        with tempfile.TemporaryDirectory(prefix='nx2a-settings-') as tmp:
            t = Path(tmp)
            (t / 'dm_settings_names.h').write_text('static const char *const dm_settings_names[] = {\n' + ''.join(f'"{n}",\n' for n in names) + '};\n', encoding='utf-8')
            (t / 'main.c').write_text(RUNTIME, encoding='utf-8')
            inc = ['-include', str(DM / 'android/dm_settings.h'), '-I', str(t), '-I', str(DM / 'android')]
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-D_GNU_SOURCE', '-pthread'] + inc + [str(t / 'main.c'), str(DM / 'android/dm_settings.c'), '-o', str(t / 'run')], check=True)
            out = subprocess.run([str(t / 'run')], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            self.assertIn('settings runtime ok', out.stdout)
            self.assertEqual(out.stderr.count('DM_NO_SUCH_SETTING'), 1, out.stderr)


if __name__ == '__main__':
    unittest.main()
