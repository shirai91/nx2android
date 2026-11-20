# The shader corpus: every game's Switch shaders that a port on this PC has split (<main checkout>/.gamedump/<App>/<title ID>_build/shader/pkg), translated by the
# translator of this checkout and compared with a reference bundle, shader by shader. It answers "which games' shaders does this translator change touch?" before
# a merge, without a device: a translator fix found with one game changes others' SPIR-V too (the AstOptimizer reorder that made FFR's fog flicker took 8 hours).
# Game shaders are game data: the corpus and its bundles stay in .gamedump (git-ignored), never in the repository or CI (CI runs the synthetic shader tests).
#   python devtools/shader_corpus.py list                      the games with a split shader package, and their reference
#   python devtools/shader_corpus.py record [--game <title ID>] [--sample N]
#                                                              the reference: each game's shaders translated by this checkout's translator (N: an even sample of N shaders)
#   python devtools/shader_corpus.py check  [--game ...] [--sample N]
#                                                              the same shaders translated again, compared with the reference: unchanged, changed, failed now
# A check before and after a translator change: record on main, switch to the change, check. Exit 1 when a shader's SPIR-V changed or a shader failed.
import argparse, json, os, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / 'nxnative/dm/android/tools'))


def gamedump():
    common = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, check=True).stdout.strip()
    return Path(common).parent / '.gamedump'


def games():
    """{title ID: shader package folder} of the ports on this PC"""
    out = {}
    for pkg in sorted(gamedump().glob('*/*_build/shader/pkg')):
        tid = pkg.parents[1].name.split('_')[0]
        if len(tid) == 16 and any(pkg.iterdir()): out.setdefault(tid, pkg)
    return out


def corpus_dir(tid): return gamedump() / 'ShaderCorpus' / tid


def translate(pkg, tid, out, sample, xlate, dotnet, jobs):
    env = dict(os.environ, DM_GAME=tid, PYTHONUTF8='1')
    cmd = [sys.executable, str(REPO / 'nxnative/dm/android/tools/shader_vk.py'), str(pkg), str(out), '--jobs', str(jobs), '--xlate', str(xlate), '--dotnet', str(dotnet),
           '--report', str(out) + '.report.json']
    if sample: cmd += ['--keys', ','.join(sample)]
    t = time.time(); r = subprocess.run(cmd, capture_output=True, text=True, env=env, encoding='utf-8', errors='replace')
    if r.returncode or not Path(out).is_file(): raise SystemExit(f'{tid}: shader_vk.py failed ({r.returncode}): {(r.stderr or r.stdout).strip()[-800:]}')
    return time.time() - t


def bundle(path):
    """{key: SPIR-V bytes} of a bundle"""
    import dmsv
    with dmsv.Bundle(path) as b: return {f'{e[0]:016x}': bytes(b.spv(e)) for e in b.entries}


def sample_keys(all_keys, n):
    if not n or n >= len(all_keys): return None
    s = sorted(all_keys); return [s[i * len(s) // n] for i in range(n)]


def tools():
    from porter import config, pipeline, toolchain
    cfg = config.load(); dotnet = toolchain.find_dotnet(cfg)
    if not dotnet: raise SystemExit('dotnet not found (porter doctor)')
    logs = gamedump() / 'ShaderCorpus' / 'logs'; logs.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PYTHONUTF8='1', DOTNET_CLI_TELEMETRY_OPTOUT='1')
    dll, key = pipeline.build_translator(dotnet, env, logs)
    return dll, dotnet, key


def run(cmd, a):
    gs = games()
    if a.game: gs = {t: p for t, p in gs.items() if t == a.game.upper()}
    if not gs: raise SystemExit('no game with a split shader package in .gamedump (port one first)' if not a.game else f'{a.game}: no split shader package in .gamedump')
    if cmd == 'list':
        for t, p in gs.items():
            r = corpus_dir(t) / 'ref.json'
            print(f'{t}  {p.parents[2].name:28} ' + (json.loads(r.read_text(encoding="utf-8"))['note'] if r.exists() else 'no reference'))
        return 0
    xlate, dotnet, xkey = tools(); bad = 0
    for t, p in gs.items():
        d = corpus_dir(t); d.mkdir(parents=True, exist_ok=True)
        ref = d / 'ref.dmsv'; meta = d / 'ref.json'
        if cmd == 'check' and not ref.exists(): print(f'{t}: no reference (record first)'); bad += 1; continue
        sample = json.loads(meta.read_text(encoding='utf-8')).get('sample') if cmd == 'check' else None
        out = ref if cmd == 'record' else d / 'now.dmsv'
        if cmd == 'record' and a.sample:   # the keys of the whole package, from a full report kept in the corpus, else from the existing bundle a port made
            built = p.parents[1] / 'android/shaders.dmsv'
            keys = json.loads((d / 'keys.json').read_text(encoding='utf-8')) if (d / 'keys.json').exists() else (sorted(bundle(built)) if built.exists() else None)
            if keys is None: raise SystemExit(f'{t}: --sample needs the keys of the package: record once without it, or port the game first')
            (d / 'keys.json').write_text(json.dumps(keys), encoding='utf-8'); sample = sample_keys(keys, a.sample)
        secs = translate(p, t, out, sample, xlate, dotnet, a.jobs)
        got = bundle(out)
        if cmd == 'record':
            note = f'{len(got):,} shaders' + (f' (a sample of {len(sample):,})' if sample else '') + f', translator {xkey[:12]}, {time.strftime("%Y-%m-%d %H:%M")}'
            meta.write_text(json.dumps({'note': note, 'sample': sample, 'translator': xkey}, indent=1), encoding='utf-8')
            print(f'{t}: reference {note} ({secs:.0f} s)'); continue
        want = bundle(ref)
        changed = sorted(k for k in want if k in got and got[k] != want[k]); failed = sorted(k for k in want if k not in got); new = sorted(k for k in got if k not in want)
        bad += bool(changed or failed)
        print(f'{t}: {len(want):,} shaders: {len(want) - len(changed) - len(failed):,} unchanged, {len(changed):,} changed, {len(failed):,} failed now, {len(new):,} new ({secs:.0f} s)')
        for k in changed[:20]: print(f'  changed {k}')
        for k in failed[:20]: print(f'  failed  {k}')
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description='translate the split shaders of the ports on this PC and compare them with a reference')
    ap.add_argument('cmd', choices=('list', 'record', 'check'))
    ap.add_argument('--game', default=''); ap.add_argument('--sample', type=int, default=0); ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    a = ap.parse_args(argv)
    if hasattr(sys.stdout, 'reconfigure'): sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    return run(a.cmd, a)


if __name__ == '__main__':
    sys.exit(main())
