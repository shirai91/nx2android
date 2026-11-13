"""Manual Android device replay; see vk_cpu_handle_readiness.md. Does not contact devices."""
from pathlib import Path
import argparse, platform, subprocess, struct
W=Path(__file__).resolve().parents[1]; dm=W/'nxnative/dm'
ap=argparse.ArgumentParser(description='Build the donor GPU-produced uniform-handle fixture against an existing Android native host build')
ap.add_argument('--ndk',type=Path,required=True)
ap.add_argument('--host-build',type=Path,required=True)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--game',default='0100000000010000')
a=ap.parse_args(); D=a.out.resolve(); D.mkdir(parents=True,exist_ok=True); ndk=a.ndk
host={'Darwin':'darwin-x86_64','Linux':'linux-x86_64'}[platform.system()]

cc=ndk/'toolchains/llvm/prebuilt'/host/'bin/clang'; glslc=ndk/'shader-tools'/host/'glslc'
def bind(kind,desc,binding,reg=0,count=1,dim=255,off=0,samp=0,sarg=0):
    return struct.pack('<BBBBHHhBBii',kind,desc,dim,0,binding,count,reg,samp,0,off,sarg)
def refl(bs,words=(0,0,0)):
    return struct.pack('<5I',len(bs),0,*words)+b''.join(bs)
entries=[]
fixtures=[('vertex',0,0x52,(dm/'android/shaders/maskclear.vert').read_text(),refl([])),
('fragment',1,0xfeed6001,'#version 450\nlayout(location=0)out vec4 color;layout(binding=0)uniform sampler3D images[32];layout(binding=1)uniform Params{uint index;float coordinate;}p;void main(){color=textureLod(images[p.index],vec3(p.coordinate,.5,.5),0);}',refl([bind(12,2,0,7,32,2,352,4,8|((128+8*7)<<8)),bind(1,0,1,9)],(1,0,0))),
('producer',5,0xfeed6000,'#version 450\nlayout(local_size_x=1)in;layout(binding=0)buffer Output{uint words[];}o;layout(binding=1)uniform Input{uvec4 p;}i;void main(){o.words[i.p.x]=i.p.y;o.words[i.p.x+1]=i.p.z;}',refl([bind(4,1,0),bind(1,0,1)],(1,1,1)))]
fixtures.append(('consumer',5,0xfeed6002,'#version 450\nlayout(local_size_x=16)in;layout(binding=0)uniform sampler3D images[32];layout(binding=1)uniform Params{uint index;float coordinate;}p;layout(binding=2)buffer Output{uint colors[];}o;void main(){o.colors[gl_LocalInvocationIndex]=packUnorm4x8(textureLod(images[p.index],vec3(p.coordinate,.5,.5),0));}',refl([bind(12,2,0,7,32,2,352,4,8|((128+8*7)<<8)),bind(1,0,1,9),bind(6,1,2,10)],(16,1,1))))
fixtures.append(('writer',1,0xfeed6003,'#version 450\nlayout(location=0)out uint handle;layout(binding=0)uniform Input{uvec4 p;}i;void main(){handle=i.p.y;}',refl([bind(1,0,0,0)],(1,0,0))))
for name,stage,key,source,reflection in fixtures:
    src=D/(name+{0:'.vert',1:'.frag',5:'.comp'}[stage]);out=src.with_suffix('.spv');src.write_text(source)
    subprocess.run([str(glslc),'--target-env=vulkan1.1',str(src),'-o',str(out)],check=True)
    entries.append((key,stage,out.read_bytes(),reflection))
entries.sort(key=lambda entry:entry[0])
offset=12+32*len(entries);table=[];data=[]
for key,stage,spv,reflection in entries:
    table.append(struct.pack('<QIIIIII',key,stage,offset,len(spv),offset+len(spv),len(reflection),0));data.extend([spv,reflection]);offset+=len(spv)+len(reflection)
(D/'shaders.dmsv').write_bytes(struct.pack('<4sII',b'DMSV',1,len(entries))+b''.join(table+data))
inc=['-include',str(dm/'android/compat/include/darwin_compat.h')]+['-I'+str(dm/p) for p in ['android/compat/include','host','gfx/nvn','gfx/tex','gfx/nvn/include','android']]+['-I'+str(a.host_build/'gen'/a.game)]
obj=D/'cpu_handle_readiness.o'
subprocess.run([str(cc),'--target=aarch64-linux-android31','-O2','-g','-DDM_ANDROID=1','-D_GNU_SOURCE',*inc,'-c',str(W/'tests/vk_cpu_handle_readiness.c'),'-o',str(obj)],check=True)
objs=list((a.host_build/'obj').glob('*.o'))
if not objs: raise SystemExit('Build the production Vulkan host first with dm/android/build.py')
subprocess.run([str(cc),'--target=aarch64-linux-android31','-fuse-ld=lld','-pie','-Wl,--wrap=main','-Wl,--wrap=prog_key','-Wl,--wrap=nvn_dpool_base','-o',str(D/'cpu-handle-readiness'),str(obj),*map(str,objs),'-landroid','-llog','-lvulkan','-laaudio','-lnativewindow','-lmediandk','-lz','-lm','-ldl'],check=True)
