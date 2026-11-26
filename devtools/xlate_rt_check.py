# The run-time shader translator (dm/android/xlate_rt) against the porter's bundle, on the build machine: each capture in a folder (<key>.bin as the app saves them,
# gfx_vk_shader.c vk_shader_missing) goes through the library's managed code (Runtime.cs TranslateCapture, the steps libdmsh_xlate_rt.so runs on the device), and its
# SPIR-V and reflection are compared with the bundle's entry of the same key, which shader_vk.py --capture made from the same file. Captures are game data: keep them in
# the build folder (android/captured_shaders), never in the repository.
#   python devtools/xlate_rt_check.py <captures folder> <shaders.dmsv>
# Exit 1 when an entry differs, failed, or is not in the bundle. Pokémon Sword's 46 opening captures: 46 the same (issue #490).
import argparse, os, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'nxnative/dm/android/tools'))
import dmsv   # noqa: E402

CHECK_CS = r'''
using System; using System.Collections.Generic; using System.IO;
var con = Console.Out;   // the library silences Console (its translator runs print to it)
foreach (var f in Directory.GetFiles(args[0], "*.bin")) {
    var b = File.ReadAllBytes(f); int nc = BitConverter.ToInt32(b, 4), nk = BitConverter.ToInt32(b, 8); var log = new List<string>();
    var ps = DmshRuntime.TranslateCapture(b[12..(12 + nc)], b[(12 + nc)..(12 + nc + nk)], new DmshRuntime.Options(), Path.Combine(args[1], "work"), log);
    foreach (var l in log) con.WriteLine($"{Path.GetFileName(f)}: {l}");
    if (ps.Count == 0) con.WriteLine($"{Path.GetFileName(f)}: FAILED");
    foreach (var p in ps) { File.WriteAllBytes(Path.Combine(args[1], $"{p.Key:x16}.spv"), p.Spv); File.WriteAllBytes(Path.Combine(args[1], $"{p.Key:x16}.refl"), p.Refl); }
}
'''


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('captures'); ap.add_argument('bundle'); ap.add_argument('--dotnet', default=shutil.which('dotnet') or 'dotnet')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='xlate_rt_check_') as t:
        t = Path(t); proj, out = t / 'check', t / 'out'; proj.mkdir(); out.mkdir()
        (proj / 'check.csproj').write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework></PropertyGroup>'
                                           f'<ItemGroup><ProjectReference Include="{REPO / "nxnative/dm/android/xlate_rt/dmsh_xlate_rt.csproj"}" /></ItemGroup></Project>', encoding='utf-8')
        (proj / 'Check.cs').write_text(CHECK_CS, encoding='utf-8')
        subprocess.run([a.dotnet, 'build', proj, '-c', 'Release', '-o', t / 'bin', '-nologo', '-v', 'quiet', '--artifacts-path', t / 'artifacts'], check=True)
        r = subprocess.run([a.dotnet, t / 'bin/check.dll', a.captures, out], capture_output=True, text=True, errors='replace')
        failed = [ln for ln in r.stdout.splitlines() if ln.endswith(': FAILED') or '[RED]' in ln]
        for ln in r.stdout.splitlines(): print(' ', ln)
        same, diff, missing = 0, [], []
        with dmsv.Bundle(a.bundle) as b:
            ix = {e[0]: e for e in b.entries}
            for f in sorted(out.glob('*.spv')):
                e = ix.get(int(f.stem, 16))
                if not e: missing.append(f.stem); continue
                if bytes(b.spv(e)) == f.read_bytes() and bytes(b.refl(e)) == f.with_suffix('.refl').read_bytes(): same += 1
                else: diff.append(f.stem)
    print(f'{same} the same as the bundle; {len(diff)} differ {" ".join(diff)}; {len(missing)} not in the bundle {" ".join(missing)}; {len(failed)} failure lines')
    return 1 if diff or missing or failed or r.returncode else 0


if __name__ == '__main__':
    sys.exit(main())
