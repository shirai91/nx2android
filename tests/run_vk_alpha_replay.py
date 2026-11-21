"""Manual Android device replay; see vk_alpha_replay.md. Does not contact devices."""
from pathlib import Path
import argparse, platform, subprocess, struct
W=Path(__file__).resolve().parents[1]; dm=W/'nxnative/dm'
ap=argparse.ArgumentParser(description='Build the donor alpha fixture against an existing Android native host build')
ap.add_argument('--ndk',type=Path,required=True)
ap.add_argument('--host-build',type=Path,required=True)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--game',default='0100000000010000')
a=ap.parse_args(); D=a.out.resolve(); D.mkdir(parents=True,exist_ok=True); ndk=a.ndk
host={'Darwin':'darwin-x86_64','Linux':'linux-x86_64'}[platform.system()]

cc=ndk/'toolchains/llvm/prebuilt'/host/'bin/clang'; glslc=ndk/'shader-tools'/host/'glslc'
entries=[]
for name,stage,key,flags in [('vertex',0,0x52,[]),('late',1,0x61,[]),('early',1,0x62,['-DBOUND=1']),('integer',1,0x63,['-DINTEGER_TARGET=1'])]:
    out=D/(name+'.spv');src=W/'tests/fixtures/alpha.vert' if stage==0 else W/'tests/fixtures/alpha.frag'
    subprocess.run([str(glslc),'--target-env=vulkan1.1',*flags,str(src),'-o',str(out)],check=True)
    bindings=[]
    if name=='early':
        bindings=[struct.pack('<BBBBHHhBBii',1,0,0,0,0,1,0,0,0,0,0),struct.pack('<BBBBHHhBBii',4,1,0,0,1,1,0,0,0,0,0)]
    reflection=struct.pack('<5I',len(bindings),0,0,0,0)+b''.join(bindings)
    entries.append((key,stage,out.read_bytes(),reflection))
offset=12+32*len(entries);table=[];data=[]
for key,stage,spv,reflection in entries:
    table.append(struct.pack('<QIIIIII',key,stage,offset,len(spv),offset+len(spv),len(reflection),0));data.extend([spv,reflection]);offset+=len(spv)+len(reflection)
(D/'shaders.dmsv').write_bytes(struct.pack('<4sII',b'DMSV',1,len(entries))+b''.join(table+data))
inc=['-include',str(dm/'android/compat/include/darwin_compat.h')]+['-I'+str(dm/p) for p in ['android/compat/include','host','gfx/nvn','gfx/tex','gfx/nvn/include','android']]+['-I'+str(a.host_build/'gen'/a.game)]
obj=D/'alpha_replay.o'
subprocess.run([str(cc),'--target=aarch64-linux-android31','-O2','-g','-DDM_ANDROID=1','-D_GNU_SOURCE',*inc,'-c',str(W/'tests/vk_alpha_replay.c'),'-o',str(obj)],check=True)
objs=list((a.host_build/'obj').glob('*.o'))
if not objs: raise SystemExit('Build the production Vulkan host first with dm/android/build.py')
subprocess.run([str(cc),'--target=aarch64-linux-android31','-fuse-ld=lld','-pie','-Wl,--wrap=main','-Wl,--wrap=prog_key','-o',str(D/'alpha-replay'),str(obj),*map(str,objs),'-landroid','-llog','-lvulkan','-laaudio','-lnativewindow','-lmediandk','-lz','-lm','-ldl'],check=True)
