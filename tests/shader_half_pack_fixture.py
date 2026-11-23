"""Native half-pack control and CPU oracle for every binary16 rounding boundary."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'nxnative/dm/android/tools'), str(ROOT/'tests')]
from test_shader_half_pack import finite_inputs, oracle
import spv_half, spv_precise


def generate(directory):
    directory.mkdir(parents=True, exist_ok=True)
    values = finite_inputs()
    values += [v | sign for v in (0x7f800001, 0x7fa00000, 0x7fc12345, 0x7fffffff) for sign in (0, 0x80000000)]
    inputs = bytearray(); expected = bytearray()
    for a, b in zip(values[::2], values[1::2]):
        inputs += struct.pack('<4I', a, b, 0xcccccccc, 0xdeadbeef)
        half = [0x7e00 if v & 0x7fffffff > 0x7f800000 else oracle(v) for v in (a, b)]
        expected += struct.pack('<4I', a, b, half[0] | half[1] << 16, 0xdeadbeef)
    (directory/'inputs.bin').write_bytes(inputs); (directory/'expected.bin').write_bytes(expected)
    (directory/'native.comp').write_text('''#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) buffer Data { uvec4 data[]; } b;
void main() {
uint i=gl_GlobalInvocationID.x;
if(i<b.data.length()) b.data[i].z=packHalf2x16(uintBitsToFloat(b.data[i].xy));
}
''')
    compiler = shutil.which('glslc') or str(Path(os.environ['ANDROID_NDK_HOME'])/'shader-tools/darwin-x86_64/glslc')
    subprocess.run([compiler, '--target-env=vulkan1.1', directory/'native.comp', '-o', directory/'native.spv'], check=True)
    raw = (directory/'native.spv').read_bytes()
    lowered = spv_precise.lower(spv_half.lower(raw))
    (directory/'lowered.spv').write_bytes(lowered)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', directory/'lowered.spv'], check=True)
    (directory/'cases.json').write_text(json.dumps(dict(components=len(values), pairs=len(values)//2, bytes=len(inputs)), indent=2)+'\n')
    return directory


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
