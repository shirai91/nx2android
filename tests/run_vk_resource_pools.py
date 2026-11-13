"""Manual Android device replay; see vk_resource_pools.md. Does not contact devices."""
from pathlib import Path
import argparse, platform, subprocess, struct
W=Path(__file__).resolve().parents[1]; dm=W/'nxnative/dm'
ap=argparse.ArgumentParser(description='Build the donor sparse resource-pool fixture against an existing Android native host build')
ap.add_argument('--ndk',type=Path,required=True)
ap.add_argument('--host-build',type=Path,required=True)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--translator',type=Path,required=True)
ap.add_argument('--dotnet',default='dotnet')
ap.add_argument('--game',default='0100000000010000')
a=ap.parse_args(); D=a.out.resolve(); D.mkdir(parents=True,exist_ok=True); ndk=a.ndk
host={'Darwin':'darwin-x86_64','Linux':'linux-x86_64'}[platform.system()]

cc=ndk/'toolchains/llvm/prebuilt'/host/'bin/clang'; glslc=ndk/'shader-tools'/host/'glslc'
from resource_pool_fixture import generate
entries=[generate(D,a.translator,a.dotnet)]
from test_resource_pool_export import pool_exporter
for name,stage,key,source in [('vertex',0,0x52,(dm/'android/shaders/maskclear.vert').read_text()),('fragment',1,0xfeed6001,'''#version 450
layout(location=0)out float color;
layout(binding=0)uniform Input{uvec4 values[64];}fp_c3;
layout(binding=1)uniform texture2D fp_t2d_cb0_40000000[1048576];
layout(binding=2)uniform sampler fp_s_cb0_40000000[4096];
void main(){uint h=fp_c3.values[8+uint(gl_FragCoord.x)].x;color=textureLod(sampler2D(fp_t2d_cb0_40000000[h&0xfffffu],fp_s_cb0_40000000[h>>20]),vec2(uintBitsToFloat(fp_c3.values[1].x),.5),0).r;}
''')]:
    src=D/(name+('.vert' if stage==0 else '.frag'));out=src.with_suffix('.spv');src.write_text(source)
    subprocess.run([str(glslc),'--target-env=vulkan1.1',str(src),'-o',str(out)],check=True)
    data=out.read_bytes();env=pool_exporter();bindings,_,_=env['spirv_bindings'](data)
    data,bindings=env['lower_resource_pools'](data,[env['classify']('vs' if stage==0 else 'ps',b,{}) for b in bindings])
    entries.append((key,stage,data,env['refl_bin'](bindings,b'',(0,0,0))))
# Internal map records are never an exported ABI; exercise loader rejection.
key,stage,data,reflection=entries[0]
bad_map=struct.pack('<BBBBHHhBBii',20,1,255,0,90,1,95,0,0,0,0)
entries.append((0xfeed6020,stage,data,struct.pack('<5I',1,0,8,1,1)+bad_map))
header=list(struct.unpack_from('<5I',reflection));header[0]+=1
entries.append((0xfeed6021,stage,data,struct.pack('<5I',*header)+reflection[20:]+bad_map))
entries.sort(key=lambda entry:entry[0])
offset=12+32*len(entries);table=[];data=[]
for key,stage,spv,reflection in entries:
    table.append(struct.pack('<QIIIIII',key,stage,offset,len(spv),offset+len(spv),len(reflection),0));data.extend([spv,reflection]);offset+=len(spv)+len(reflection)
(D/'shaders.dmsv').write_bytes(struct.pack('<4sII',b'DMSV',1,len(entries))+b''.join(table+data))
inc=['-include',str(dm/'android/compat/include/darwin_compat.h')]+['-I'+str(dm/p) for p in ['android/compat/include','host','gfx/nvn','gfx/tex','gfx/nvn/include','android']]+['-I'+str(a.host_build/'gen'/a.game)]
obj=D/'resource_pools.o'
subprocess.run([str(cc),'--target=aarch64-linux-android31','-O2','-g','-DDM_ANDROID=1','-D_GNU_SOURCE',*inc,'-c',str(W/'tests/vk_resource_pools.c'),'-o',str(obj)],check=True)
objs=list((a.host_build/'obj').glob('*.o'))
if not objs: raise SystemExit('Build the production Vulkan host first with dm/android/build.py')
subprocess.run([str(cc),'--target=aarch64-linux-android31','-fuse-ld=lld','-pie','-Wl,--wrap=main','-Wl,--wrap=prog_key','-Wl,--wrap=nvn_dpool_base','-o',str(D/'resource-pools'),str(obj),*map(str,objs),'-landroid','-llog','-lvulkan','-laaudio','-lnativewindow','-lmediandk','-lz','-lm','-ldl'],check=True)
