"""Build the donor-derived MediaCodec layout probe; never contacts a device."""
from pathlib import Path
import argparse
import platform
import subprocess

ROOT=Path(__file__).resolve().parents[1]
dm=ROOT/'nxnative/dm'
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--ndk',type=Path,required=True)
ap.add_argument('--host-build',type=Path,required=True)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--game',default='0100000000010000')
a=ap.parse_args()
host={'Darwin':'darwin-x86_64','Linux':'linux-x86_64'}[platform.system()]
cc=a.ndk/'toolchains/llvm/prebuilt'/host/'bin/clang'
a.out.parent.mkdir(parents=True,exist_ok=True)
subprocess.run([str(cc),'--target=aarch64-linux-android31','-O2','-g','-DDM_ANDROID=1','-D_GNU_SOURCE',
    '-include',str(dm/'android/compat/include/darwin_compat.h'),'-I'+str(dm/'android/compat/include'),
    '-I'+str(dm/'host'),'-I'+str(a.host_build/'gen'/a.game),str(ROOT/'tests/avc_layout_device.c'),
    str(dm/'android/movie_avc.c'),'-lmediandk','-llog','-o',str(a.out)],check=True)
