"""Copied nxnative Maxwell texture corpus; target DMSV adapter only.

Source: 1f01e28c87da5233dfa14fb31fabf1535df33013 dm/android/tests/texture_ops_fixture.py
and dm/tools/tests/test_txq_type.py. No game assets required.
"""
import argparse, ast, collections, json, re, struct, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'nxnative/dm/android/tools'), str(ROOT / 'tests')]
import dmsv, spv_gather, spv_half, spv_precise, spv_texture_offsets
from test_shader_storage_flags import exporter
TABLE = (ROOT / 'nxnative/dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs').read_text()
SLOT_128, SAMPLER_1 = 0x168, 0x570

def base(name):
    match = re.search(r'Add\("([01x]{64})", InstName\.' + name + r'\s*,', TABLE)
    assert match, f'{name} is not in InstTable.cs'
    return int(match[1].replace('x', '0'), 2)

def ldc(rd, offset):                                            # LDC.32 Rd, c[2][offset], RZ
    return base('Ldc') | (4 << 48) | (2 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | rd

def handle(rd, texture, sampler):                               # Rd = c[2][texture] | c[2][sampler], and R10 + R11 as scratch
    return [ldc(10, texture), ldc(11, sampler), base('Lop') | (1 << 41) | (11 << 20) | (7 << 16) | (10 << 8) | rd]

def mov(rd, value):
    return base('Mov32i') | (value << 20) | (7 << 16) | (15 << 12) | rd

def shader(*parts):
    insts = [mov(0, 0), mov(1, 0), mov(2, 0)]
    for part in parts: insts += part if isinstance(part, list) else [part]
    insts.append(base('Exit') | (7 << 16) | 15)
    words = []
    for i, inst in enumerate(insts):
        if i % 3 == 0: words.append(0)                          # Maxwell scheduling word
        words.append(inst)
    return struct.pack('<%dQ' % len(words), *words)

VARIANTS = ([dict(op=op, lod=lod, offset=offset) for op in ('sample', 'fetch')
             for lod in (0, 1) for offset in (0, 1)] +
            [dict(op='gradient', offset=offset) for offset in (0, 1)] +
            [dict(op='gather', component=component, offset=offset)
             for component in range(4) for offset in range(3)] + [dict(op='query'), dict(op='query', full=True)])


def program(case):
    def load(reg, offset, width=4):
        return base('Ldc') | (width << 48) | (3 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | reg
    op = case['op']
    if op == 'gradient':
        ops = handle(0, SLOT_128, SAMPLER_1) + [load(1, 0), load(2, 4), load(3, 8)]
        ops += [load(4 + i, 16 + 4 * i) for i in range(4)]
        instruction = base('TxdB') | (case['offset'] << 35) | (4 << 20)
    elif op == 'query':
        ops = handle(6, SLOT_128, SAMPLER_1) + [load(7, 8), mov(19, 0x76543210)]
        instruction = base('TxqB') | (1 << 22) | (6 << 8)
    else:
        ops = handle(6, SLOT_128, SAMPLER_1) + [load(0, 0), load(1, 4), load(7, 8), load(8, 12)]
        if op == 'sample': instruction = base('TexB') | ((3 if case['lod'] else 1) << 37) | (case['offset'] << 36)
        elif op == 'fetch': instruction = base('TldB') | (case['lod'] << 55) | (case['offset'] << 35)
        else: instruction = base('Tld4B') | (case['component'] << 38) | (case['offset'] << 36)
        instruction |= 6 << 20
    # Keep both packed width/height/mip outputs with a guarded register and the
    # full query, whose absent 2D depth must be zero after type resolution.
    mask = 11 if op == 'query' and not case.get('full') else 15
    instruction |= (mask << 31) | (7 << 16) | 16
    if op != 'query': instruction |= 2 << 28
    ops += [instruction, load(4, 56, width=5), base('Stg') | (6 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 16]
    return shader(ops)


def generate(work, translator):
    work.mkdir(parents=True, exist_ok=True)
    jobs = []
    for index, case in enumerate(VARIANTS):
        key = f'{0xfeed7000 + index:016x}'
        code = work / (key + '.bin'); code.write_bytes(program(case))
        jobs.append(dict(key=key, stage='cs', code=str(code), tex={'128': '2d'}, local=[1, 1, 1],
                         host_target='vulkan', sb_align=256))
    path = work / 'jobs.json'; path.write_text(json.dumps(jobs))
    subprocess.run(['dotnet', str(translator), str(path), str(work)], check=True)
    env = exporter(); env['collections'] = collections
    source = ROOT / 'nxnative/dm/android/tools/shader_vk.py'; tree = ast.parse(source.read_text())
    names = {'OP_NAME', 'DEC_BINDING', 'SC_UNIFORM_CONSTANT'}
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'spirv_bindings' or
             isinstance(n, ast.Assign) and any(isinstance(t, ast.Tuple) and
                 any(isinstance(v, ast.Name) and v.id in names for v in t.elts) for t in n.targets)]
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), env)
    before, after = [], []
    for job in jobs:
        key = job['key']; raw = (work / (key + '.spv')).read_bytes()
        res = json.loads((work / (key + '.res.json')).read_text())
        if res['notes']: raise ValueError(res['notes'])
        # Same existing offset/numeric sequence as the actual exporter, with a
        # control bundle immediately before this port's cleanup.
        control = spv_precise.lower(spv_half.lower(spv_texture_offsets.lower(raw)))
        clean = spv_gather.lower(control)
        binds, local, _ = env['spirv_bindings'](clean)
        refl = env['refl_bin']([env['classify']('cs', b, res) for b in binds], b'', local)
        before.append((int(key, 16), 5, control, refl)); after.append((int(key, 16), 5, clean, refl))
        (work / (key + '.clean.spv')).write_bytes(clean)
    # These translator failures predate capability cleanup. Keep them visible
    # in the copied corpus, but never execute the invalid modules.
    known_invalid = {5: 'Image Operand Offset', 7: 'Image Operand Offset', 23: 'Vector access is out of bounds'}
    for index, (control, cleaned) in enumerate(zip(before, after)):
        for label, entry in (('control', control), ('clean', cleaned)):
            result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=entry[2], capture_output=True)
            if index in known_invalid:
                if result.returncode == 0 or known_invalid[index] not in result.stderr.decode():
                    raise ValueError(f'baseline expectation changed: {label} variant {index}: {result.stderr!r}')
            elif result.returncode:
                raise ValueError(f'{label} variant {index}: {result.stderr.decode()}')
    dmsv.write(work / 'control.dmsv', before, level=0)
    dmsv.write(work / 'clean.dmsv', after, level=0)
    (work / 'cases.json').write_text(json.dumps(VARIANTS, indent=2))
    return work


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path); parser.add_argument('translator', type=Path)
    args = parser.parse_args(); print(generate(args.directory.resolve(), args.translator.resolve()))
