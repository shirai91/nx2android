# Extract shaders from a DMSV bundle (dm/android/tools/shader_vk.py; either version, dmsv.py) by key, as <key>.spv in the output folder.   dmsv_extract.py <bundle> <out dir> <key hex>...
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'nxnative', 'dm', 'android', 'tools'))
import dmsv   # noqa: E402
want = {int(k, 16) for k in sys.argv[3:]}
with dmsv.Bundle(sys.argv[1]) as b:
    for e in b.entries:
        if e[0] in want:
            spv = b.spv(e); p = os.path.join(sys.argv[2], f'{e[0]:016x}.spv'); open(p, 'wb').write(spv)
            print(f'{e[0]:016x} stage {e[1]}: {len(spv)} bytes SPIR-V{f" ({e[3]} zstd-compressed)" if e[6] else ""}, reflection {e[5]} bytes -> {p}')
