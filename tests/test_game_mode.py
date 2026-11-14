"""Game Mode metadata and production native policy, without Android or game files."""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / 'nxnative/dm/android/apk'
ANDROID = ROOT / 'nxnative/dm/android'


def pool(data, offset):
    count, _, _, start, _ = struct.unpack_from('<IIIII', data, offset + 8)
    values = []
    for entry in struct.unpack_from(f'<{count}I', data, offset + 28):
        at = offset + start + entry
        size = struct.unpack_from('<H', data, at)[0]
        values.append(data[at + 2:at + 2 + size * 2].decode('utf-16-le'))
    return values


def chunks(data, start=8, end=None):
    end = len(data) if end is None else end
    while start < end:
        kind, header, size = struct.unpack_from('<HHI', data, start)
        if size < header or start + size > end: raise AssertionError('invalid resource chunk')
        yield kind, header, start, size
        start += size
    if start != end: raise AssertionError('invalid chunk boundary')


def elements(data):
    strings, ids, nodes = [], [], []
    for kind, _, at, size in chunks(data):
        if kind == 1: strings = pool(data, at)
        elif kind == 0x180: ids = struct.unpack_from(f'<{(size - 8) // 4}I', data, at + 8)
        elif kind == 0x102:
            name = struct.unpack_from('<I', data, at + 20)[0]
            count = struct.unpack_from('<H', data, at + 28)[0]
            attrs = {}
            for i in range(count):
                ns, key, _, _, _, typ, value = struct.unpack_from('<IIIHBBI', data, at + 36 + i * 20)
                attrs[strings[key]] = (typ, strings[value] if typ == 3 else value, ids[key] if key < len(ids) else 0)
            nodes.append((strings[name], attrs))
    return nodes


def resources(data):
    paths, out = [], {}
    for kind, header, at, size in chunks(data, 12):
        if kind == 1: paths = pool(data, at)
        elif kind == 0x200:
            package = struct.unpack_from('<I', data, at + 8)[0]
            for typ, h, pos, length in chunks(data, at + header, at + size):
                if typ != 0x201: continue
                type_id, _, _, count, start = struct.unpack_from('<BBHII', data, pos + 8)
                for i, offset in enumerate(struct.unpack_from(f'<{count}I', data, pos + h)):
                    if offset == 0xffffffff: continue
                    entry = pos + start + offset
                    value_size, _, value_type, value = struct.unpack_from('<HBBI', data, entry + 8)
                    if value_type != 3 or value_size != 8: raise AssertionError('expected file resource')
                    out.setdefault(package << 24 | type_id << 16 | i, []).append(paths[value])
    return out


def function(source, name):
    import re
    found = re.search(r'^(?:static )?(?:void|int|uint64_t) ' + name + r'\([^\n]*?\)\s*\{', source, re.M)
    if not found: raise AssertionError(f'missing production function {name}')
    start = found.start(); at = source.index('{', found.start()); depth = 1; end = at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[start:end]


class GameMode(unittest.TestCase):
    def compile_run(self, source, includes=()):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: self.skipTest('C compiler required')
        with tempfile.TemporaryDirectory(prefix='nx2a_game_mode_') as temp:
            c = Path(temp) / 'test.c'; exe = Path(temp) / 'test'; c.write_text(source, encoding='utf-8')
            result = subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', *['-I' + str(x) for x in includes], str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

    def test_packed_configuration_and_icon_resources(self):
        # Exercise production main but capture ZIP entries before signing; no signing key or APK is generated.
        for enabled, icon in ((enabled, icon) for enabled in (False, True) for icon in ('none', 'plain', 'adaptive')):
            with self.subTest(enabled=enabled, icon=icon), tempfile.TemporaryDirectory(prefix='nx2a_game_pack_') as temp:
                t = Path(temp); (t/'lib.so').write_bytes(b'synthetic library')
                for name in ('icon', 'fg', 'bg'): (t/(name+'.png')).write_bytes(name.encode())
                args = ['mkapk.py', str(t/'lib.so'), str(t/'out.apk')]
                if enabled: args += ['--game-mode-performance']
                if icon != 'none': args += ['--icon', str(t/'icon.png')]
                if icon == 'adaptive': args += ['--icon-fg', str(t/'fg.png'), '--icon-bg', str(t/'bg.png')]
                spec = importlib.util.spec_from_file_location('test_game_mkapk', APK/'mkapk.py'); module = importlib.util.module_from_spec(spec)
                with patch.object(sys, 'argv', args), patch.object(sys, 'path', [str(APK), *sys.path]): spec.loader.exec_module(module)
                captured = {}
                class Captured(Exception): pass
                def capture(entries): captured.update({name: data for name, data, _ in entries}); raise Captured
                with patch.object(module, 'gen_key', return_value={}), patch.object(module, 'cert', return_value=b''), patch.object(module, 'build_zip', side_effect=capture):
                    with self.assertRaises(Captured): module.main()
                nodes = elements(captured['AndroidManifest.xml'])
                app = next(attrs for tag, attrs in nodes if tag == 'application')
                self.assertEqual(app['appCategory'], (0x10, 0, 0x01010545))
                metadata = {attrs['name'][1]: attrs for tag, attrs in nodes if tag == 'meta-data'}
                res = resources(captured['resources.arsc']) if 'resources.arsc' in captured else {}
                if enabled:
                    self.assertIn('android.game_mode_config', metadata)
                    ref = metadata['android.game_mode_config']['resource']
                    self.assertEqual((ref[0], ref[2]), (1, 0x01010025))
                    self.assertEqual(metadata['com.android.app.gamemode.performance.enabled']['value'][1], 0xffffffff)
                    target = res[ref[1]]; self.assertEqual(len(target), 1)
                    config = elements(captured[target[0]])
                    self.assertEqual(config, [('game-mode-config', {'supportsPerformanceGameMode': (0x12, 0xffffffff, 0x01010657)})])
                else:
                    self.assertNotIn('android.game_mode_config', metadata)
                    self.assertNotIn('com.android.app.gamemode.performance.enabled', metadata)
                    self.assertNotIn('res/xml/game_mode_config.xml', captured)
                if icon != 'none':
                    self.assertIn('res/drawable-nodpi-v4/icon.png', res[app['icon'][1]])
                if icon == 'adaptive':
                    adaptive = next(x for x in res[app['icon'][1]] if x.endswith('.xml'))
                    for tag, attrs in elements(captured[adaptive]):
                        if tag in ('foreground', 'background'): self.assertIn(res[attrs['drawable'][1]][0], captured)

    def test_porter_only_opts_in_classified_nvn(self):
        sys.path.insert(0, str(ROOT))
        from porter import pipeline
        for gl in (False, True, None):
            with self.subTest(gl=gl), tempfile.TemporaryDirectory(prefix='nx2a_game_pipeline_') as temp:
                t = Path(temp); p = object.__new__(pipeline.Port)
                p.g = SimpleNamespace(); p.app_name = 'Synthetic'; p.pkg_ver = 'v0'; p.package = 'org.nx2a.probe'; p.display = '1.0'
                p.o = SimpleNamespace(out=t, vercode_offset=0, external_display=False, native_keyboard=False, vulkan_driver=None, debug=True)
                p.ap = SimpleNamespace(landscape=False); p.so = t/'host.so'; p.logs = t; p.env = {}; p.result = {}
                p.icon = p.theme = p.ui_font = lambda: []
                if gl is not None: p.gl = gl
                calls = []
                def run(args, *a, **kw): calls.append(args); p.apk_path.write_bytes(b'unsigned placeholder')
                with patch.object(pipeline, 'app_stem', return_value='Synthetic'), patch.object(pipeline.nsp, 'run', side_effect=run), patch.object(pipeline.paths, 'signing_key', return_value=t/'unused-key'), patch.object(pipeline, 'PROG'):
                    p.apk()
                self.assertEqual(len(calls), 1)
                self.assertEqual('--game-mode-performance' in calls[0], gl is False)

    def test_native_policy_and_explicit_overrides(self):
        source = (ANDROID/'perf_android.c').read_text(encoding='utf-8')
        self.assertTrue('void dm_perf_game_mode(' in source, 'missing Game Mode policy')
        start = source.index('static const int64_t STEPS[]')
        end = source.index('// The frame\'s replay', start)
        code = source[start:end]
        code += '\nstatic int64_t g_frame_cost; static unsigned g_held_back;\nstatic int shown_late(void) { return -1; }\n'
        code += function(source, 'adapt_frame')
        self.compile_run('''#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#define LOG(...) ((void)0)
#ifdef _WIN32
static int setenv(const char *k, const char *v, int o) { char s[256]; (void)o; snprintf(s, sizeof s, "%s=%s", k, v); return _putenv(s); }
static int unsetenv(const char *k) { char s[256]; snprintf(s, sizeof s, "%s=", k); return _putenv(s); }
#endif
static int allow_env, env_reads;
static char *policy_getenv(const char *key) {
    assert(allow_env && "environment read outside presenting-thread sync");
    env_reads++;
    return getenv(key);
}
#define getenv policy_getenv
''' + code + r'''
#undef getenv
static void sync_frame(void) {
    allow_env=1; game_mode_sync(); allow_env=0;
}
static void refresh_overrides(void) {
    int before=env_reads;
    for (int i=0;i<30;i++) sync_frame();
    assert(env_reads-before==2);
}
int main(void) {
    unsetenv("DM_ADAPTIVE_FPS"); unsetenv("DM_FPS_PIN");
    // Acquiring a frame may run on the guest thread, before the first present.
    g_step=2; assert(dm_present_period_adapt(16666667)==33333333);
    dm_perf_game_mode(2); sync_frame();
    assert(env_reads==2 && dm_present_period_adapt(16666667)==16666667);
    assert(dm_present_period_adapt(33333333)==33333333 && dm_present_period_adapt(0)==0);
    assert(!adaptive_enabled());
    for (int i=0;i<29;i++) sync_frame();
    assert(env_reads==2);
    sync_frame(); assert(env_reads==4);
    g_nwork=7; g_step=2;
    dm_perf_game_mode(1); sync_frame();
    assert(env_reads==4 && g_nwork==0 && g_step==0 && adaptive_enabled());
    g_step=1; assert(dm_present_period_adapt(16666667)==25000000);
    // Mode changes use the cached overrides immediately, between environment polls.
    dm_perf_game_mode(2); sync_frame(); assert(!adaptive_enabled() && env_reads==4);
    for (int i=0;i<100;i++) { sync_frame(); adapt_frame(50000000, 9000000000LL+i); }
    assert(g_step==0 && g_nwork==0);
    setenv("DM_ADAPTIVE_FPS","1",1); refresh_overrides();
    assert(adaptive_enabled());
    g_step=2; assert(dm_present_period_adapt(16666667)==33333333);
    setenv("DM_ADAPTIVE_FPS","0",1); refresh_overrides();
    assert(!adaptive_enabled() && dm_present_period_adapt(16666667)==16666667);
    unsetenv("DM_ADAPTIVE_FPS");
    for (int pin=0;pin<3;pin++) {
        const char *values[]={"60","40","30"};
        const uint64_t periods[]={16666667,25000000,33333333};
        setenv("DM_FPS_PIN",values[pin],1); refresh_overrides();
        assert(adaptive_enabled());
        adapt_frame(50000000, 9000000000LL);
        assert(dm_present_period_adapt(16666667)==periods[pin]);
    }
    setenv("DM_FPS_PIN","garbage",1); refresh_overrides();
    assert(!adaptive_enabled());
    dm_perf_game_mode(99); sync_frame(); assert(adaptive_enabled());

    return 0;
}
''')

    def test_profile_ceiling_survives_performance_mode(self):
        source = (ANDROID/'perf_android.c').read_text(encoding='utf-8')
        start = source.index('static const int64_t STEPS[]')
        code = source[start:source.index('// The frame\'s replay', start)]
        # The linked presentation backend supplies this weak hook from the profile.
        code = code.replace('__attribute__((weak)) int dm_fps_ceiling(void) { return 0; }',
                            'int dm_fps_ceiling(void) { return PROFILE_CEILING; }')
        code += '\nstatic int64_t g_frame_cost; static unsigned g_held_back;\nstatic int shown_late(void) { return -1; }\n'
        code += function(source, 'adapt_frame')
        for ceiling in (0, 30):
            with self.subTest(ceiling=ceiling):
                self.compile_run(f'#define PROFILE_CEILING {ceiling}\n' + r'''#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#define LOG(...) ((void)0)
#ifdef _WIN32
static int setenv(const char *k, const char *v, int o) { char s[256]; (void)o; snprintf(s, sizeof s, "%s=%s", k, v); return _putenv(s); }
static int unsetenv(const char *k) { char s[256]; snprintf(s, sizeof s, "%s=", k); return _putenv(s); }
#endif
''' + code + r'''
static void refresh(void) { for (int i=0;i<30;i++) game_mode_sync(); }
int main(void) {
    uint64_t expected = PROFILE_CEILING == 30 ? 33333333 : 16666667;
    unsetenv("DM_ADAPTIVE_FPS"); unsetenv("DM_FPS_PIN");
    game_mode_sync();
    assert(dm_present_period_adapt(16666667)==expected);
    dm_perf_game_mode(2); game_mode_sync();
    assert(!adaptive_enabled());
    assert(dm_present_period_adapt(16666667)==expected);
    assert(dm_present_period_adapt(50000000)==50000000);
    assert(dm_present_period_adapt(0)==0);
    dm_perf_game_mode(1); game_mode_sync();
    assert(adaptive_enabled() && dm_present_period_adapt(16666667)==expected);
    setenv("DM_ADAPTIVE_FPS","0",1); refresh();
    assert(!adaptive_enabled() && dm_present_period_adapt(16666667)==expected);
    // Preserve main's explicit diagnostic pin precedence while adaptation is enabled.
    unsetenv("DM_ADAPTIVE_FPS"); setenv("DM_FPS_PIN","60",1); refresh();
    adapt_frame(50000000,9000000000LL);
    assert(dm_present_period_adapt(16666667)==16666667);
    return 0;
}
''')

    def test_resume_queries_mode_and_recovers_from_service_failures(self):
        source = (ANDROID/'front_android.c').read_text(encoding='utf-8')
        query = function(source, 'activity_game_mode'); resume = function(source, 'on_resume')
        jdk = Path(os.environ.get('JAVA_HOME', '/Library/Java/JavaVirtualMachines/jdk-20.jdk/Contents/Home'))
        if not (jdk/'include/jni.h').exists(): self.skipTest('JDK JNI headers required')
        fixture = (ROOT/'tests/game_mode_jni_fixture.c').read_text(encoding='utf-8')
        platform = 'darwin' if sys.platform == 'darwin' else 'win32' if sys.platform == 'win32' else 'linux'
        self.compile_run(fixture.replace('/* PRODUCTION */', query + '\n' + resume), [jdk/'include', jdk/'include'/platform])
