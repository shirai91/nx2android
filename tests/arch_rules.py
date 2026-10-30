# docs/ARCHITECTURE.md's rules, checked on the committed tree (tests/test_architecture.py runs it; also `python tests/arch_rules.py`).
import ast, re, subprocess, sys
from pathlib import Path

R = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent).resolve()
files = subprocess.run(['git', '-C', str(R), 'ls-files'], capture_output=True, text=True, check=True).stdout.splitlines()
fails, notes = [], []
def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ''))
    if not ok: fails.append(name)

def strip_c(s): return re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', s, flags=re.S))
def strip_py(s): return re.sub(r'#[^\n]*', '', s)
profiles = sorted(p.name for p in (R / 'nxnative/dm/games').iterdir() if p.is_dir() and re.fullmatch(r'0100[0-9A-F]{12}', p.name))
names = set()
for pid in profiles:
    g = R / 'nxnative/dm/games' / pid / 'game.tsv'
    for line in g.read_text(encoding='utf-8', errors='replace').splitlines():
        k, _, v = line.partition('\t')
        if k == 'name' and len(v) > 3: names.add(v.strip())
pkgs = set()
for pid in profiles:
    a = R / 'nxnative/dm/games' / pid / 'android.tsv'
    if a.exists(): pkgs |= {l.split('\t')[1].strip() for l in a.read_text(encoding='utf-8').splitlines() if l.startswith('package\t')}
GAME_RX = re.compile(r'0100[0-9A-Fa-f]{12}|\bNEO\b|Viractal|' + '|'.join(re.escape(k) + r'\b' for k in pkgs))   # profiles' own packages, not generic defaults

# 1. Core, Android runtime: no title IDs, game names or per-game branches in code
hits = []
for f in files:
    if not f.startswith('nxnative/dm/android/') or not f.endswith(('.c', '.h', '.py', '.m')): continue
    s = (R / f).read_text(encoding='utf-8', errors='replace')
    code = strip_c(s) if f.endswith(('.c', '.h', '.m')) else strip_py(s)
    code = re.sub(r'"[^"\n]*"', lambda m: m.group(0) if GAME_RX.search(m.group(0)) or any(n in m.group(0) for n in names) else '""', code)
    for m in GAME_RX.finditer(code): hits.append(f'{f}: {m.group(0)}')
    for n in names:
        if n in code: hits.append(f'{f}: {n}')
check('dm/android has no game-specific code', not hits, '; '.join(hits[:5]))

# 2. Core, shared: no title IDs in code outside the profiles (upstream's own per-game rules live in patches.py / sdk_patches.py)
hits = []
for f in files:
    if not re.match(r'nxnative/dm/(host|gfx|tools)/', f) or not f.endswith(('.c', '.h', '.m')): continue
    code = strip_c((R / f).read_text(encoding='utf-8', errors='replace'))
    # system titles (0100000000000000..0100000000001fff: firmware programs, system data such as hle_fs.c's MiiModel 0100000000000802, applets) are not games
    if re.search(r'DM_ANDROID|__ANDROID__', code) and re.search(r'0100(?!00000000[01])[0-9A-Fa-f]{12}', code): hits.append(f)
check('shared core: no title-ID branch in Android-guarded code', not hits, ', '.join(hits))
diff = subprocess.run(['git', '-C', str(R), 'log', '--format=%h', '--', 'nxnative/dm/host', 'nxnative/dm/gfx', 'nxnative/dm/tools', 'nxnative/dm/third_party'], capture_output=True, text=True).stdout.split()
notes.append(f'{len(diff)} commits touch the shared core since the subtree')

# 3. Game profiles: data only, android.tsv keys from the documented set
KEYS = {'status', 'tested', 'package', 'orientation', 'requires', 'setting', 'env', 'overlay'}
STATUS = {'playable', 'wip', 'boots', 'experimental'}
bad = []
for f in files:
    m = re.match(r'nxnative/dm/games/(0100[0-9A-F]{12})/(.+)', f)
    if not m: continue
    if f.endswith(('.c', '.h', '.m', '.cs', '.sh')) or (f.endswith('.py') and m.group(2) != 'patches.py'): bad.append(f)
check('profiles hold data only (code only as patches.py)', not bad, ', '.join(bad))
atsv = [f for f in files if f.endswith('/android.tsv') and f.startswith('nxnative/dm/games/')]
bad = []
for f in atsv:
    rows = [l.split('\t', 1) for l in (R / f).read_text(encoding='utf-8').splitlines() if l and not l.startswith('#')]
    for r in rows:
        if r[0] not in KEYS: bad.append(f'{f}: key {r[0]}')
    st = [r[1] for r in rows if r[0] == 'status']
    if len(st) != 1 or st[0] not in STATUS: bad.append(f'{f}: status {st}')
    if st in (['playable'], ['wip']) and not any(r[0] == 'tested' for r in rows): bad.append(f'{f}: {st[0]} without tested')
check(f'android.tsv keys and status valid ({len(atsv)} profiles)', not bad, '; '.join(bad))
ap = (R / 'nxnative/dm/android/tools/android_profile.py').read_text(encoding='utf-8')
missing = [k for k in KEYS if f"'{k}'" not in ap]
check('android_profile.py reads every documented key', not missing, ', '.join(missing))

# 4. Port pipeline: no UI
def imports(path):
    try: t = ast.parse((R / path).read_text(encoding='utf-8'))
    except SyntaxError as e: return {f'<syntax error {e}>'}
    out = set()
    for n in ast.walk(t):
        if isinstance(n, ast.Import): out |= {a.name for a in n.names}
        elif isinstance(n, ast.ImportFrom) and n.module: out.add(n.module)
    return out
porter = [f for f in files if f.startswith('porter/') and f.endswith('.py')]
ui = [f for f in porter if any(i.split('.')[0] in ('PySide6', 'PyQt5', 'PyQt6', 'tkinter', 'gui') for i in imports(f))]
check('porter/ imports no UI', not ui, ', '.join(ui))
dev = [f for f in porter if any(i.split('.')[0] == 'devtools' for i in imports(f))]
check('porter/ needs nothing from devtools/', not dev, ', '.join(dev))

# 5. GUI: no pipeline logic: it reaches the pipeline only through `python -m porter` (runner.py), never upstream or the engine's tools
guif = [f for f in files if f.startswith('gui/') and f.endswith('.py')]
bad = []
for f in guif:
    imps = imports(f); s = (R / f).read_text(encoding='utf-8')
    if any(i.startswith(('porter.upstream', 'porter.pipeline', 'porter.install', 'porter.datapack')) for i in imps): bad.append(f'{f}: imports pipeline internals')
    if re.search(r'nxnative|patch_nso|shader_split|build_ipa_from_nsp|mkapk|build\.py', re.sub(r"'[^'\n]*'|\"[^\"\n]*\"", "''", strip_py(s))): bad.append(f'{f}: names an engine tool')
check('gui/ holds no pipeline logic', not bad, '; '.join(bad))
runner = (R / 'gui/runner.py').read_text(encoding='utf-8')
check("gui/runner.py drives 'python -m porter'", "'-m', 'porter'" in runner or '"-m", "porter"' in runner)

# 6. CLI covers what the GUI does: every porter subcommand the GUI runs exists in cli.py
cli = (R / 'porter/cli.py').read_text(encoding='utf-8')
subs = set(re.findall(r"add_parser\('([a-z]+)'", cli))
gui_calls = set()
for f in guif: gui_calls |= set(re.findall(r"\[\s*'(doctor|profiles|inspect|port|install|devices|tools|config)'", (R / f).read_text(encoding='utf-8')))
gui_calls |= set(re.findall(r"call_json\(\s*\[?\s*'([a-z]+)'", ''.join((R / f).read_text(encoding='utf-8') for f in guif)))
check('every GUI action is a CLI subcommand', gui_calls <= subs, f'GUI uses {sorted(gui_calls)}; CLI has {sorted(subs)}')

# 7. Stages: the documented ones are the pipeline's, in order
pipe = (R / 'porter/pipeline.py').read_text(encoding='utf-8')
for st in ('inputs', 'extract', 'rewrite', 'shaders', 'host', 'apk', 'data', 'install'):
    if not re.search(rf'def {st}\(self', pipe): check(f'stage {st} in pipeline.py', False)
check('all documented stages exist in pipeline.py', all(re.search(rf'def {st}\(self', pipe) for st in ('inputs', 'extract', 'rewrite', 'shaders', 'host', 'apk', 'data', 'install')))
check('host stage: --release unless --debug', "['--release']" in pipe and 'host-debug' in pipe)
check('porter imports upstream build_ipa_from_nsp (not a copy)', any('build_ipa_from_nsp' in (R / f).read_text(encoding='utf-8') for f in porter)
      and not any(Path(f).name == 'build_ipa_from_nsp.py' for f in files if not f.startswith('nxnative/')))

# 8. Toolchains: not shipped, checked by doctor
ship = [f for f in files if re.search(r'(^|/)(android-ndk|platform-tools|dotnet)(/|$)', f) or f.startswith('tools/')]
check('no toolchain committed', not ship, ', '.join(ship[:5]))
tc = (R / 'porter/toolchain.py').read_text(encoding='utf-8')
check('doctor checks NDK, .NET, adb, keys', all(w in tc for w in ('ndk', 'dotnet', 'adb', 'keys')))

# 9. Keys and game files never in the repository
leak = [f for f in files if re.search(r'\.(nsp|nsz|xci|nca|keys)$|prod\.keys|title\.keys|signing-key', f, re.I)]
check('no keys, game files or signing keys committed', not leak, ', '.join(leak))
gen = [f for f in files if Path(f).name in ('game_profile.h', 'android_profile.h')]
check('generated profile headers not committed', not gen, ', '.join(gen))

# 10. Repository layout
for d in ('docs/ARCHITECTURE.md', 'porter', 'gui', 'devtools', 'packaging/build_bundle.py', 'packaging/nx2android.spec', 'packaging/nx2android.py', 'packaging/INSTALL.md', 'tests', 'requirements.txt', 'probes', 'nxnative', 'RESEARCH.md'):
    if not (R / d).exists(): check(f'layout: {d}', False)
check('repository layout as documented', all((R / d).exists() for d in ('docs/ARCHITECTURE.md', 'porter', 'gui', 'devtools', 'packaging/build_bundle.py', 'packaging/nx2android.spec', 'packaging/nx2android.py', 'packaging/INSTALL.md', 'tests', 'requirements.txt', 'probes', 'nxnative', 'RESEARCH.md')))
check('no scripts/ folder left at the root', not any(f.startswith('scripts/') for f in files))
req = (R / 'requirements.txt').read_text(encoding='utf-8').lower()
check('requirements.txt has lz4, capstone, pillow, PySide6', all(m in req for m in ('lz4', 'capstone', 'pillow', 'pyside6')))

# 10b. Distribution: the launcher only dispatches (to the GUI, `-m porter`, or a tool's script), and the bundle takes only committed nxnative files
launcher = (R / 'packaging/nx2android.py').read_text(encoding='utf-8')
check('packaging/nx2android.py only dispatches', not ({i.split('.')[0] for i in imports('packaging/nx2android.py')} - {'multiprocessing', 'os', 'runpy', 'sys', 'ctypes', 'gui'}))
check('the bundle copies only committed nxnative files', "tracked('nxnative')" in (R / 'packaging/build_bundle.py').read_text(encoding='utf-8'))
check('frozen porter finds nxnative beside the executable', "frozen" in (R / 'porter/paths.py').read_text(encoding='utf-8'))

# 11. devtools: the device from device.json / NX2A_DEVICE, device.json not committed
dt = [f for f in files if f.startswith('devtools/') and f.endswith('.py')]
hard = [f for f in dt if re.search(r'192\.168\.\d+\.\d+|org\.nx2a\.neo\b', strip_py((R / f).read_text(encoding='utf-8')))]
check('devtools/ has no hard-coded device or package', not hard, ', '.join(hard))
check('devtools/device.json not committed', 'devtools/device.json' not in files)

for n in notes: print('note ', n)
print(f'\n{len(fails)} failed' if fails else '\nall checks passed')
sys.exit(1 if fails else 0)
