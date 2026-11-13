"""Full pool metadata and donor runtime bounds survive target export."""
import ast
import collections
import importlib
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1];TOOLS=ROOT/'nxnative/dm/android/tools'
sys.path.insert(0,str(TOOLS))
from test_shader_storage_flags import exporter

def pool_exporter():
    env=exporter();env['collections']=collections
    try:env['spv_pool']=importlib.import_module('spv_pool')
    except ModuleNotFoundError:pass
    source=TOOLS/'shader_vk.py';tree=ast.parse(source.read_text())
    nodes=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in ('spirv_bindings','lower_resource_pools') or
           isinstance(n,ast.Assign) and any(isinstance(t,ast.Tuple) and any(isinstance(v,ast.Name) and (v.id.startswith('OP_') or v.id.startswith('DEC_') or v.id.startswith('SC_')) for v in t.elts) for t in n.targets)]
    exec(compile(ast.Module(body=nodes,type_ignores=[]),str(source),'exec'),env);return env

class PoolMetadata(unittest.TestCase):
    def test_full_id_ranges_get_distinct_pool_kinds(self):
        env=exporter()
        for kind,desc,count,name in [('tex_pool','image',1<<20,'cp_t2d_cb0_40000000'),('samp_pool','sampler',1<<12,'cp_s_cb0_40000000')]:
            with self.subTest(kind=kind):
                b=dict(name=name,type=desc,binding=3,count=count)
                self.assertEqual(env['classify']('cs',b,{})['kind'],kind)

    def test_existing_logical_slot_profiles_remain_32(self):
        env=exporter();env['GAME']={'android_dynamic_slots':'on'}
        for desc,kind,prefix in [('image','texd','t2d'),('sampler','sampd','s')]:
            b=dict(name=f'cp_{prefix}_cb0_40000000',type=desc,binding=2,count=32)
            self.assertEqual(env['classify']('cs',b,{})['kind'],kind)

    @unittest.skipUnless(shutil.which('glslc') and shutil.which('spirv-val'),'requires glslc and spirv-val')
    def test_full_pools_lower_with_compact_binding_identity_and_runtime_bounds(self):
        env=pool_exporter();self.assertTrue(callable(env.get('lower_resource_pools')))
        with tempfile.TemporaryDirectory(prefix='nx2a-pool-export-') as tmp:
            work=Path(tmp);src=work/'pool.comp';raw=work/'pool.spv'
            src.write_text('''#version 450
layout(local_size_x=1) in;
layout(binding=0) uniform Inputs {uvec4 ids;} cp_c3;
layout(binding=2) uniform texture2D cp_t2d_cb0_40000000[1048576];
layout(binding=3) uniform sampler cp_s_cb0_40000000[4096];
layout(binding=4) buffer Result {vec4 color;} cp_s0;
void main(){cp_s0.color=textureLod(sampler2D(cp_t2d_cb0_40000000[cp_c3.ids.x],cp_s_cb0_40000000[cp_c3.ids.y]),vec2(.5),0);}
''')
            subprocess.run(['glslc','--target-env=vulkan1.1',str(src),'-o',str(raw)],check=True)
            # GLSL limits binding literals; reproduce the translator's legal SPIR-V numbering.
            words=list(struct.unpack(f'<{raw.stat().st_size//4}I',raw.read_bytes()));i=5
            while i<len(words):
                if words[i]&0xffff==71 and words[i+2]==33: words[i+3]={3:1048578,4:1052674}.get(words[i+3],words[i+3])
                i+=words[i]>>16
            data=struct.pack(f'<{len(words)}I',*words);binds,_,_=env['spirv_bindings'](data)
            res={'sbufs':[dict(binding=1052674,cb_slot=0,cb_offset=0x310//4,flags=1)]}
            classified=[env['classify']('cs',b,res) for b in binds]
            data,lowered=env['lower_resource_pools'](data,classified)
            output=work/'bounded.spv';output.write_bytes(data)
            subprocess.run(['spirv-val','--target-env','vulkan1.1',str(output)],check=True)
            # Validate the actual C runtime remap too, including donor rejection cases.
            executable=work/'descriptor-table'
            subprocess.run([shutil.which('cc') or 'cc','-std=c11','-fsanitize=address,undefined','-I',str(TOOLS.parent),str(ROOT/'tests/spv_descriptor_table.c'),'-o',str(executable)],check=True)
            remapped=work/'remapped.spv'
            subprocess.run([str(executable),str(output),str(remapped),'1','5','0','2','6','0'],check=True)
            subprocess.run(['spirv-val','--target-env','vulkan1.1',str(remapped)],check=True)
            byname={b['name']:b for b in lowered}
            self.assertEqual([byname[n]['binding'] for n in ['cp_c3','cp_t2d_cb0_40000000','cp_s_cb0_40000000','cp_s0','dm_pool_bounds']],[0,1,2,3,4])
            self.assertEqual((byname['cp_s0']['kind'],byname['cp_s0']['reg'],byname['cp_s0']['rw']),('sbuf',0,True))
            self.assertEqual(byname['dm_pool_bounds']['reg'],2)
            for name,kind in [('cp_t2d_cb0_40000000','tex_pool'),('cp_s_cb0_40000000','samp_pool')]:
                b=byname[name];self.assertEqual(b['kind'],kind);self.assertEqual(b['count'],1)
            refl=env['refl_bin'](lowered,b'',(1,1,1));self.assertEqual(len(refl),20+20*5)
            # Re-reflect real transformed SPIR-V, not just the adapter's sidecar.
            actual,_,_=env['spirv_bindings'](data)
            self.assertEqual({b['name']:b['count'] for b in actual if b['type'] in ('image','sampler')},{'cp_t2d_cb0_40000000':0,'cp_s_cb0_40000000':0})

if __name__=='__main__':unittest.main()
