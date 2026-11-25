"""NVNbuffer keeps its offset and size where guest ring allocators store them (+0x18, +0x20), so the buffer queries follow those stores."""
from pathlib import Path
import subprocess, tempfile, unittest
import csrc
import host_cc

NVN = csrc.REPO / 'nxnative/dm/gfx/nvn'


def production():
    source = csrc.read('nxnative/dm/gfx/nvn/hle_nvn.c')
    obj = csrc.read('nxnative/dm/gfx/nvn/nvn_obj.h')
    pool = next(line for line in obj.splitlines() if line.endswith('FITS(pool_t, NVNmemoryPool);'))
    region = csrc.between(source, '// Buffer object = pool', '// Memory pool queries', 'hle_nvn.c buffer objects')
    getters = [line for line in source.splitlines() if line.startswith(('NVN(nvnBufferBuilderGetSize,', 'NVN(nvnBufferBuilderGetMemoryOffset,'))]
    return '''#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "nvn_protos.h"
#define MAGIC(a,b,c,d) ((uint32_t)(a)|(uint32_t)(b)<<8|(uint32_t)(c)<<16|(uint32_t)(d)<<24)
#define FITS(T,H) _Static_assert(sizeof(T)<=sizeof(H), "object exceeds NVN ABI")
#define LOG(...) ((void)0)
#define OLOG(...) ((void)0)
#define ONCE_LOG(...) ((void)0)
#define NVN(name,ret,...) static ret h_##name(__VA_ARGS__); static NVNT_##name entry_##name=h_##name; static ret h_##name(__VA_ARGS__)
struct vpool;
''' + pool + '\n' + region + '\n'.join(getters) + '''
static uint8_t mem[0x10000];
int main(void) {
 pool_t pool = {.magic = 1, .mem = mem, .size = sizeof mem}; NVNbufferBuilder bb = {0};
 struct { uint64_t before; NVNbuffer buffer; uint64_t after; } g = {.before = 0x1122334455667788ull, .after = 0x8877665544332211ull};
 NVNbuffer *b = &g.buffer; uint64_t v;
 entry_nvnBufferBuilderSetDefaults(&bb); entry_nvnBufferBuilderSetStorage(&bb, (NVNmemoryPool *)&pool, 0x100, 0x200);
 assert(entry_nvnBufferInitialize(b, &bb));
 assert(entry_nvnBufferGetAddress(b) == (NVNbufferAddress)(uintptr_t)(mem + 0x100)); assert(entry_nvnBufferGetSize(b) == 0x200);
 memcpy(&v, (uint8_t *)b + 0x18, 8); assert(v == 0x100); memcpy(&v, (uint8_t *)b + 0x20, 8); assert(v == 0x200);
 const struct { uint64_t off, size; } a[] = {{0x4000, 0x100}, {0x8000, 0x300}, {0, 0}};
 for (unsigned i = 0; i < 3; i++) {
  memcpy((uint8_t *)b + 0x18, &a[i].off, 8); memcpy((uint8_t *)b + 0x20, &a[i].size, 8);
  assert(entry_nvnBufferGetAddress(b) == (NVNbufferAddress)(uintptr_t)(mem + a[i].off));
  assert(entry_nvnBufferMap(b) == mem + a[i].off);
  assert(entry_nvnBufferGetMemoryOffset(b) == (ptrdiff_t)a[i].off);
  assert(entry_nvnBufferGetSize(b) == a[i].size);
  assert(entry_nvnBufferGetMemoryPool(b) == (NVNmemoryPool *)&pool);
 }
 assert(entry_nvnBufferBuilderGetMemoryOffset(&bb) == 0x100); assert(entry_nvnBufferBuilderGetSize(&bb) == 0x200);
 entry_nvnBufferFinalize(b);
 assert(g.before == 0x1122334455667788ull && g.after == 0x8877665544332211ull);
 puts("NVN buffer ring stores: PASS"); return 0;
}
'''


@unittest.skipUnless(host_cc.posix(), 'a POSIX host C compiler is required')
class NvnBufferRingStores(unittest.TestCase):
    def test_queries_follow_guest_stores(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / 'buf.c'; exe = Path(td) / 'buf'; src.write_text(production(), encoding='utf-8')
            subprocess.run(['cc', '-std=gnu11', '-O1', '-fno-strict-aliasing', '-I' + str(NVN), '-I' + str(NVN / 'include'), str(src), '-o', str(exe)],
                           check=True, capture_output=True)
            self.assertIn('PASS', subprocess.check_output([str(exe)], text=True))


if __name__ == '__main__': unittest.main()
