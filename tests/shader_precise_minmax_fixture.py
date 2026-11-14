"""Donor numeric-operand cases adapted to native GLSL; no guest architecture required."""
from pathlib import Path
import argparse, shutil, struct, subprocess, sys
W=Path(__file__).resolve().parents[1];sys.path.insert(0,str(W/'nxnative/dm/android/tools'))
import spv_precise,spv_half
ap=argparse.ArgumentParser(description=__doc__); ap.add_argument('directory',type=Path)
D=ap.parse_args().directory.resolve(); D.mkdir(parents=True,exist_ok=True)
compiler=shutil.which('glslc')
if not compiler: raise SystemExit('Put the NDK shader tools on PATH')
values=(0x00000000,0x80000000,1,0x807fffff,0x00800000,0x80800000,0x3e800000,0xbf000000,0x3f800000,0xc0000000,0x7f800000,0xff800000,0x7fc01234,0xffc02345,0x7f804567,0xff803456)
def number(word):return struct.unpack('<f',struct.pack('<I',word))[0]
def bits(x):return struct.unpack('<I',struct.pack('<f',x))[0]
pairs=[]
for a in values:
 for b in values:
  ma,mb=a&0x7fffffff,b&0x7fffffff
  if min(ma,mb)>0x7f800000 or (max(ma,mb)<0x00800000 and a!=b):continue
  if ma>0x7f800000:lo=hi=b
  elif mb>0x7f800000:lo=hi=a
  elif number(a)<=number(b):lo,hi=a,b
  else:lo,hi=b,a
  pairs.append((a,b,lo,hi))
# The same donor numeric-operand cases, using native GLSL instead of its Ampere generator.
for name,rows,body in [('minmax',pairs,'b.data[i].z=floatBitsToUint(min(a,c));b.data[i].w=floatBitsToUint(max(a,c));'),('saturate',[(a,0,bits(0 if a&0x7fffffff>0x7f800000 else min(1,max(0,number(a)))),0xdeadbeef) for a in values if a==0 or a&0x7fffffff>=0x00800000],'b.data[i].z=floatBitsToUint(clamp(a,0.,1.));')]:
 initial=b''.join(struct.pack('<4I',a,b,0xcccccccc,0xdeadbeef) for a,b,_,_ in rows)
 expected=b''.join(struct.pack('<4I',*r) for r in rows)
 (D/(name+'-inputs.bin')).write_bytes(initial);(D/(name+'-expected.bin')).write_bytes(expected)
 shader='#version 450\nlayout(local_size_x=64) in;layout(set=0,binding=0,std430) buffer Data {uvec4 data[];} b;void main(){uint i=gl_GlobalInvocationID.x;if(i>=b.data.length())return;float a=uintBitsToFloat(b.data[i].x),c=uintBitsToFloat(b.data[i].y);'+body+'}\n'
 raw=subprocess.check_output([compiler,'-fshader-stage=compute','--target-env=vulkan1.1','-','-o','-'],input=shader.encode())
 (D/(name+'.spv')).write_bytes(spv_precise.lower(spv_half.lower(raw)))
 print(name,len(rows),'records')
