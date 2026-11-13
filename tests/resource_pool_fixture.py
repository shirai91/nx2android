"""Actual Maxwell sparse-resource fixture from nxnative 1f01e28c (no game assets)."""
import json,re,struct,subprocess
from pathlib import Path
from test_resource_pool_export import pool_exporter
ROOT=Path(__file__).resolve().parents[1]
TABLE=(ROOT/'nxnative/dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs').read_text()
def base(name):
    match = re.search(r'Add\("([01x]{64})", InstName\.' + name + r'\s*,', TABLE)
    assert match, f'{name} is not in InstTable.cs'
    return int(match[1].replace('x', '0'), 2)

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

def generate(work,translator,dotnet='dotnet',dimension=2,full=True):
    work.mkdir(parents=True,exist_ok=True)
    def ldc(reg, offset, width=4):
        return base('Ldc') | (width << 48) | (3 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | reg
    def add(reg, other):
        return base('Iadd') | (other << 20) | (7 << 16) | (reg << 8) | reg
    ops = [ldc(4, 0, 5), base('S2r') | (33 << 20) | (7 << 16) | 14, mov(15, 2),
           base('Shl') | (15 << 20) | (7 << 16) | (14 << 8) | 14, add(4, 14),
           base('Ldg') | (4 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 6,
           ldc(0, 16), mov(1, 0x3f000000), mov(3, 0), base('TexB') | (1 << 37) | (1 << 31) | (dimension << 28) | (6 << 20) | (7 << 16) | 8,
           ldc(12, 56, 5), add(12, 14), base('Stg') | (4 << 48) | (1 << 45) | (7 << 16) | (12 << 8) | 8]
    key = '00000000feed6000'; path = work / (key + '.bin'); path.write_bytes(shader(ops))

    job=dict(key=key,stage='cs',code=str(path),local=[8,1,1],sb_align=256,full_resource_pools=full)
    jobs=work/'jobs.json';jobs.write_text(json.dumps([job]))
    subprocess.run([dotnet,str(translator),str(jobs),str(work)],check=True)
    raw=(work/(key+'.spv')).read_bytes();res=json.loads((work/(key+'.res.json')).read_text())
    if any('not implemented' in n or 'Invalid' in n for n in res['notes']):raise ValueError(res['notes'])
    env=pool_exporter();env['GAME']={'android_dynamic_slots':'off' if full else 'on'};binds,local,_=env['spirv_bindings'](raw)
    classified=[env['classify']('cs',b,res) for b in binds]
    lowered,bindings=env['lower_resource_pools'](raw,classified)
    (work/'bounded.spv').write_bytes(lowered)
    assert {b['kind'] for b in bindings} >= ({'tex_pool','samp_pool','pool_bounds'} if full else {'texd','sampd'})
    if not full:assert all(b['count']==32 for b in bindings if b['kind'] in ('texd','sampd'))
    return int(key,16),5,lowered,env['refl_bin'](bindings,b'',local)
