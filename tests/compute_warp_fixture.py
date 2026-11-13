"""Target export adapter for unchanged nxnative 1f01e28c Maxwell warp oracles."""
import json, os, shutil, struct, subprocess
from pathlib import Path
from test_resource_pool_export import pool_exporter
import spv_half, spv_precise, spv_texture_offsets, spv_warp
GUARD = 0xbaddecaf


def export_cases(work, programs):
    work.mkdir(parents=True, exist_ok=True)
    jobs=[]
    for key, local, binary, expected, control in programs:
        code=work/(key+'.bin');code.write_bytes(binary)
        jobs.append(dict(key=key,stage='cs',code=str(code),local=local,sb_align=256,subgroup=int(os.environ.get('NX2A_WARP_SUBGROUP','128')),**control))
    manifest=work/'jobs.json';manifest.write_text(json.dumps(jobs))
    subprocess.run([os.environ.get('DOTNET','dotnet'),os.environ['DMSH_TEST_XLATE'],str(manifest),str(work)],check=True)
    env=pool_exporter();entries=[]
    for key, local, binary, expected, control in programs:
        raw=(work/(key+'.spv')).read_bytes();res=json.loads((work/(key+'.res.json')).read_text())
        if res['notes']:raise ValueError(res['notes'])
        data=spv_texture_offsets.lower(raw)
        if not os.environ.get('NX2A_WARP_NATIVE'):data=spv_warp.lower(data,shutil.which('spirv-opt'))
        data=spv_precise.lower(spv_half.lower(data))
        subprocess.run(['spirv-val','--target-env','vulkan1.1','-'],input=data,check=True)
        bindings,actual_local,_=env['spirv_bindings'](data)
        bindings=[env['classify']('cs',b,res) for b in bindings]
        outputs=[b for b in bindings if b['kind']=='sbuf_ubo']
        if len(outputs)!=1 or outputs[0]['off']!=56 or outputs[0]['reg']!=0 or any(b['kind'] not in ('sbuf_ubo','cbuf','support') for b in bindings):raise ValueError(bindings)
        if list(actual_local)!=list(local):raise ValueError(actual_local)
        (work/(key+'.expected')).write_bytes(struct.pack(f'<{len(expected)}I',*expected))
        entries.append((int(key,16),5,data,env['refl_bin'](bindings,b'',local)))
    return entries
