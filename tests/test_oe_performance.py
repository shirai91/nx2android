"""Resolve both SDK performance-configuration overloads without a guest apm session."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PerformanceConfiguration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            raise unittest.SkipTest('a C compiler is required')
        cls.temp = tempfile.TemporaryDirectory(prefix='nx2a-oe-perf-')
        cls.addClassCleanup(cls.temp.cleanup)
        oe = (ROOT / 'nxnative/dm/host/hle_oe.c').read_text(encoding='utf-8')
        setters = oe[oe.index('HLE_FORCE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi"'):
                     oe.index('// Scripted state changes')]
        header = (ROOT / 'nxnative/dm/host/hle.h').read_text(encoding='utf-8')
        registration = header[header.index('typedef struct { const char *name; void *fn; const char *why; }'):
                              header.index('// Same as above,')]
        loader = (ROOT / 'nxnative/dm/host/loader.c').read_text(encoding='utf-8')
        lookup = loader[loader.index('static void *force_lookup('):loader.index('// Generic host:')]
        names = re.findall(r'HLE_FORCE\("[^"]+",\s*"[^"]+",\s*void,\s*(\w+),', setters)
        entries = ', '.join('hle_force_' + name for name in names)
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) do { printf(__VA_ARGS__); putchar('\n'); } while (0)
// The platform linker normally collects these entries from its swforce section.
#define DM_SECTION(name) aligned(8)
REGISTRATION
SETTERS
static const hle_force_ent *g_force;
static unsigned g_nforce, *g_force_hits;
static struct { int imports; } g_flag;
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    assert(!strcmp(kind, "FORCE")); assert(name); return fn;
}
LOOKUP
int main(int argc, char **argv) {
    assert(argc == 2);
    hle_force_ent entries[] = { ENTRIES };
    g_force = entries; g_nforce = sizeof entries / sizeof *entries;
    g_force_hits = calloc(g_nforce, sizeof *g_force_hits); assert(g_force_hits);
    int configs[] = {0x20004, 0x20002, 0x10000, -1};
    if (!strcmp(argv[1], "both")) {
        void (*set)(int) = (void (*)(int))force_lookup("_ZN2nn2oe27SetPerformanceConfigurationEi");
        assert(set && "one-argument performance call must not enter the guest SDK");
        for (unsigned i = 0; i < sizeof configs / sizeof *configs; ++i) set(configs[i]);
    } else {
        assert(!strcmp(argv[1], "mode"));
        void (*set)(int, int) = (void (*)(int, int))force_lookup("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi");
        assert(set);
        for (int mode = 0; mode < 2; ++mode)
            for (unsigned i = 0; i < sizeof configs / sizeof *configs; ++i) set(mode, configs[i]);
    }
    assert(!force_lookup("_ZN2nn2oe27SetPerformanceConfigurationEv"));
    unsigned hits = 0;
    for (unsigned i = 0; i < g_nforce; ++i) hits += g_force_hits[i];
    assert(hits == 1); free(g_force_hits);
}
'''
        for marker, value in [('REGISTRATION', registration), ('SETTERS', setters),
                              ('LOOKUP', lookup), ('ENTRIES', entries)]:
            harness = harness.replace(marker, value)
        source = Path(cls.temp.name) / 'check.c'
        cls.exe = Path(cls.temp.name) / 'check'
        source.write_text(harness, encoding='utf-8')
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(cls.exe)],
                       check=True, capture_output=True, text=True)

    def check_call(self, mode, expected):
        run = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout.splitlines(), expected)

    def test_both_modes_overload_avoids_guest_session(self):
        self.check_call('both', [f'oe::SetPerformanceConfiguration cfg={cfg} (ignored)'
                                for cfg in ['0x20004', '0x20002', '0x10000', '0xffffffff']])

    def test_per_mode_overload_keeps_its_abi(self):
        self.check_call('mode', [f'oe::SetPerformanceConfiguration mode={mode} cfg={cfg} (ignored)'
                                for mode in [0, 1]
                                for cfg in ['0x20004', '0x20002', '0x10000', '0xffffffff']])
