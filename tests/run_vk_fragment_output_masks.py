"""Manual Android device replay; see vk_fragment_output_masks.md. Does not contact devices."""
from pathlib import Path
import argparse, platform, subprocess, struct
W=Path(__file__).resolve().parents[1]; dm=W/'nxnative/dm'
ap=argparse.ArgumentParser(description='Build the donor MRT output-mask fixture against an existing Android native host build')
ap.add_argument('--ndk',type=Path,required=True)
ap.add_argument('--host-build',type=Path,required=True)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--game',default='0100000000010000')
a=ap.parse_args(); D=a.out.resolve(); D.mkdir(parents=True,exist_ok=True); ndk=a.ndk
host={'Darwin':'darwin-x86_64','Linux':'linux-x86_64'}[platform.system()]

cc=ndk/'toolchains/llvm/prebuilt'/host/'bin/clang'; glslc=ndk/'shader-tools'/host/'glslc'
entries=[]
sources=[('vertex',0,0x52,(dm/'android/shaders/maskclear.vert').read_text()),
         ('even',1,0x61,'#version 450\nlayout(location=0)out vec4 a;layout(location=2)out vec4 b;void main(){a=vec4(1,0,0,1);b=vec4(0,1,0,1);}\n'),
         ('odd',1,0x62,'#version 450\nlayout(location=1)out float a;layout(location=3)out float b;void main(){a=b=1;}\n')]
for name,stage,key,text in sources:
    src=D/(name+('.vert' if stage==0 else '.frag'));src.write_text(text);out=src.with_suffix('.spv')
    subprocess.run([str(glslc),'--target-env=vulkan1.1',str(src),'-o',str(out)],check=True)
    entries.append((key,stage,out.read_bytes(),struct.pack('<5I',0,0,0,0,0)))
offset=12+32*len(entries);table=[];data=[]
for key,stage,spv,reflection in entries:
    table.append(struct.pack('<QIIIIII',key,stage,offset,len(spv),offset+len(spv),len(reflection),0));data.extend([spv,reflection]);offset+=len(spv)+len(reflection)
(D/'shaders.dmsv').write_bytes(struct.pack('<4sII',b'DMSV',1,len(entries))+b''.join(table+data))
inc=['-include',str(dm/'android/compat/include/darwin_compat.h')]+['-I'+str(dm/p) for p in ['android/compat/include','host','gfx/nvn','gfx/tex','gfx/nvn/include','android']]+['-I'+str(a.host_build/'gen'/a.game)]
obj=D/'fragment_output_masks.o'
subprocess.run([str(cc),'--target=aarch64-linux-android31','-O2','-g','-DDM_ANDROID=1','-D_GNU_SOURCE',*inc,'-c',str(W/'tests/vk_fragment_output_masks.c'),'-o',str(obj)],check=True)
objs=list((a.host_build/'obj').glob('*.o'))
if not objs: raise SystemExit('Build the production Vulkan host first with dm/android/build.py')
subprocess.run([str(cc),'--target=aarch64-linux-android31','-fuse-ld=lld','-pie','-Wl,--wrap=main','-Wl,--wrap=prog_key','-o',str(D/'fragment-output-masks'),str(obj),*map(str,objs),'-landroid','-llog','-lvulkan','-laaudio','-lnativewindow','-lmediandk','-lz','-lm','-ldl'],check=True)
