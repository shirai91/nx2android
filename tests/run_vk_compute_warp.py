"""Build donor warp fixtures for manual Android replay; does not contact a device."""
from pathlib import Path
import argparse, importlib, os, platform, shutil, struct, subprocess, sys
ROOT=Path(__file__).resolve().parents[1];dm=ROOT/'nxnative/dm'
ap=argparse.ArgumentParser(description=__doc__)
for name in ('ndk','host-build','out','translator'):ap.add_argument('--'+name,type=Path,required=True)
ap.add_argument('--dotnet',default='dotnet');ap.add_argument('--native',action='store_true',help='negative control: leave original physical subgroup operations')
ap.add_argument('--subgroup',type=int,choices=(32,128),default=32,help='donor uses 32; nx2android default is 128')
ap.add_argument('--converged-only',action='store_true',help='omit conditional fixture family (unchanged by donor pass with target subgroup=128)')
ap.add_argument('--game',default='0100000000010000');a=ap.parse_args()
D=a.out.resolve();D.mkdir(parents=True,exist_ok=True);host={'Darwin':'darwin-x86_64','Linux':'linux-x86_64','Windows':'windows-x86_64'}[platform.system()]
suffix='.exe' if os.name=='nt' else '';cc=a.ndk/'toolchains/llvm/prebuilt'/host/('bin/clang'+suffix)
os.environ['NX2A_WARP_SUBGROUP']=str(a.subgroup)
os.environ.update(DMSH_TEST_XLATE=str(a.translator),DOTNET=a.dotnet,PATH=str(a.ndk/'shader-tools'/host)+os.pathsep+os.environ['PATH'])
if a.native:os.environ['NX2A_WARP_NATIVE']='1'
else:os.environ.pop('NX2A_WARP_NATIVE',None)
entries=[];manifest=[]
for name in ('warp_shuffle_fixture','warp_vote_fixture','warp_mask_fixture','predicated_warp_fixture'):
    if a.converged_only and name=='predicated_warp_fixture':continue
    work=D/name;entries.extend(importlib.import_module(name).generate(work))
    for p in sorted(work.glob('*.expected')):shutil.copyfile(p,D/p.name);manifest.append(f'{p.stem} {p.stat().st_size//4}\n')
(D/'cases.txt').write_text(''.join(manifest))
sys.path.insert(0,str(dm/'android/tools'));import dmsv
dmsv.write(str(D/'shaders.dmsv'),entries,level=0)
inc=['-include',str(dm/'android/compat/include/darwin_compat.h')]+['-I'+str(dm/p) for p in ['android/compat/include','host','gfx/nvn','gfx/tex','gfx/nvn/include','android']]+['-I'+str(a.host_build/'gen'/a.game)]
obj=D/'compute_warp.o'
subprocess.run([str(cc),'--target=aarch64-linux-android31','-O2','-g','-DDM_ANDROID=1','-D_GNU_SOURCE',*inc,'-c',str(ROOT/'tests/vk_compute_warp.c'),'-o',str(obj)],check=True)
objs=list((a.host_build/'obj').glob('*.o'))
if not objs:raise SystemExit('Build the production Vulkan host first')
subprocess.run([str(cc),'--target=aarch64-linux-android31','-fuse-ld=lld','-pie','-Wl,--wrap=main','-Wl,--wrap=prog_key','-o',str(D/'compute-warp'),str(obj),*map(str,objs),'-landroid','-llog','-lvulkan','-laaudio','-lnativewindow','-lmediandk','-lz','-lm','-ldl'],check=True)
print(f'Built {len(entries)} cases in {D}')
