# Which shaders of a DMSV bundle read which members of Ryujinx's support buffer (the uniform block named "support_buffer").   spv_support_use.py <bundle> [--keys]
# Members: 0 FragmentAlphaTest, 1 FragmentIsBgra[8], 2 ViewportInverse, 3 ViewportSize, 4 FragmentRenderScaleCount, 5 RenderScale[73], 6 TfeOffset, 7 TfeVertexCount
import collections, os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'nxnative', 'dm', 'android', 'tools'))
import dmsv   # noqa: E402  (either version of the bundle)
bundle = dmsv.Bundle(sys.argv[1])
use = collections.Counter(); keys = collections.defaultdict(list)
for e in bundle.entries:
    key, st = e[0], e[1]; spv = bundle.spv(e)
    w = struct.unpack_from('<%dI' % (len(spv) // 4), spv)
    names, consts, j, sb, found = {}, {}, 5, None, set()
    while j < len(w):
        wc, op = w[j] >> 16, w[j] & 0xffff
        if wc == 0: break
        if op == 5: names[w[j + 1]] = bytes(struct.pack('<%dI' % (wc - 2), *w[j + 2:j + wc])).split(b'\0')[0].decode(errors='replace')   # OpName
        elif op == 43 and wc == 4: consts[w[j + 2]] = w[j + 3]                                                                          # OpConstant
        elif op == 65 and wc >= 5 and names.get(w[j + 3]) == 'support_buffer' and w[j + 4] in consts: found.add(consts[w[j + 4]])      # OpAccessChain
        j += wc
    for m in found: use[(st, m)] += 1; keys[(st, m)].append('%016x' % key)
for (st, m), c in sorted(use.items()): print(f'stage {st} member {m}: {c} shaders' + (f"  e.g. {' '.join(keys[(st, m)][:4])}" if '--keys' in sys.argv else ''))
