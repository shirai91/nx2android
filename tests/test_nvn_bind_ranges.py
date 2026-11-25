"""NVN bind ranges are checked when they are recorded (slots_fit): a negative first or count, a stage beyond the six the replay keeps, or a first near INT_MAX
records nothing (SetViewports with first -1 had passed first + n <= 16, and the Vulkan replay wrote S.vp[-1]); a bind running past its table records the slots inside it."""
from pathlib import Path
import subprocess, tempfile, unittest
import csrc
import host_cc

NVN = csrc.REPO / 'nxnative/dm/gfx/nvn'
NAMES = ('SetViewports', 'SetViewportSwizzles', 'BindVertexBuffers', 'BindUniformBuffer', 'BindUniformBuffers', 'BindStorageBuffer',
         'BindStorageBuffers', 'BindTextures', 'BindSeparateSamplers', 'BindImages')


def nvn_function(source, name):
    start = source.index('NVN(nvnCommandBuffer' + name + ','); end = source.index('{', start) + 1; depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[start:end]


def production():
    source = csrc.read('nxnative/dm/gfx/nvn/hle_nvn.c')
    fit = csrc.between(source, 'static inline int slots_fit(', 'NVN(nvnCommandBufferSetViewports,', 'hle_nvn.c slots_fit')
    handles = csrc.function(source, 'static p_bind_handles *bind_handles(', 'hle_nvn.c bind_handles')
    return r'''#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "nvn_obj.h"
#include "nvn_protos.h"
#define LOG(...) (reds++)
#define CALLER_CHAIN_LOG(...) ((void)0)
static int reds, recs; static uint8_t arena[1 << 16];
#define CB(c) ((cmd_t *)(c))
static void *rec(NVNcommandBuffer *c, uint32_t op, size_t payload) { (void)c; (void)op; assert(payload <= sizeof arena); recs++; return arena; }
#define REC(c, OP, T) ((T *)rec((c), (OP), sizeof(T)))
#define RECN(c, OP, T, ARR, N) ((T *)rec((c), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))
static uint64_t mtl_samp_seq(void) { return 0; }
#define NVN(name,ret,...) static ret h_##name(__VA_ARGS__); static NVNT_##name entry_##name=h_##name; static ret h_##name(__VA_ARGS__)
''' + fit + handles + '\n' + '\n'.join(nvn_function(source, n) for n in NAMES) + r'''
#define NONE(call) do { int r0 = recs; call; if (recs != r0) { printf("recorded: %s\n", #call); return 1; } } while (0)
int main(void) {
 static NVNcommandBuffer cbuf; NVNcommandBuffer *c = &cbuf;
 float vp[64] = {0}; NVNviewportSwizzle sw[4] = {0}; NVNbufferRange r3[3] = {{0x1000, 16}, {0x2000, 16}, {0x3000, 16}}; uint64_t th[3] = {1, 2, 3};
 NONE(entry_nvnCommandBufferSetViewports(c, -1, 1, vp)); NONE(entry_nvnCommandBufferSetViewports(c, 0x7fffffff, 1, vp));
 NONE(entry_nvnCommandBufferSetViewportSwizzles(c, -1, 1, sw));
 NONE(entry_nvnCommandBufferBindVertexBuffers(c, -1, 1, r3)); NONE(entry_nvnCommandBufferBindVertexBuffers(c, 0x7fffffff, 2, r3));
 NONE(entry_nvnCommandBufferBindTextures(c, NVN_SHADER_STAGE_FRAGMENT, -1, 1, (const NVNtextureHandle *)th));
 NONE(entry_nvnCommandBufferBindTextures(c, NVN_SHADER_STAGE_FRAGMENT, 0x7fffffff, 3, (const NVNtextureHandle *)th));
 NONE(entry_nvnCommandBufferBindTextures(c, (NVNshaderStage)-1, 0, 1, (const NVNtextureHandle *)th));
 NONE(entry_nvnCommandBufferBindSeparateSamplers(c, NVN_SHADER_STAGE_FRAGMENT, -2, 2, (const NVNseparateSamplerHandle *)th));
 NONE(entry_nvnCommandBufferBindImages(c, NVN_SHADER_STAGE_FRAGMENT, 32, 1, (const NVNimageHandle *)th));
 NONE(entry_nvnCommandBufferBindUniformBuffer(c, NVN_SHADER_STAGE_VERTEX, -1, 0x20000, 0x100));
 NONE(entry_nvnCommandBufferBindUniformBuffers(c, (NVNshaderStage)-1, 0, 2, r3));
 NONE(entry_nvnCommandBufferBindStorageBuffer(c, NVN_SHADER_STAGE_COMPUTE, 16, 0x40000, 0x1000));
 NONE(entry_nvnCommandBufferBindStorageBuffers(c, NVN_SHADER_STAGE_COMPUTE, -1, 2, r3));
 int r0 = recs; entry_nvnCommandBufferSetViewports(c, 0, 16, vp); entry_nvnCommandBufferBindVertexBuffers(c, 14, 2, r3); assert(recs == r0 + 2);
 r0 = recs; entry_nvnCommandBufferBindUniformBuffers(c, NVN_SHADER_STAGE_VERTEX, 14, 3, r3); assert(recs == r0 + 2);   /* past slot 16: the two slots inside it */
 r0 = recs; entry_nvnCommandBufferBindTextures(c, NVN_SHADER_STAGE_FRAGMENT, 254, 3, (const NVNtextureHandle *)th); assert(recs == r0 + 1 && ((p_bind_handles *)arena)->n == 2);
 assert(reds > 0);
 puts("NVN bind ranges: PASS"); return 0;
}
'''


@unittest.skipUnless(host_cc.posix(), 'a POSIX host C compiler is required')
class NvnBindRanges(unittest.TestCase):
    def test_out_of_range_binds_record_nothing(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / 'ranges.c'; exe = Path(td) / 'ranges'; src.write_text(production(), encoding='utf-8')
            r = subprocess.run(['cc', '-std=gnu11', '-O1', '-fno-strict-aliasing', '-I' + str(NVN), '-I' + str(NVN / 'include'), str(src), '-o', str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertIn('PASS', out.stdout, out.stdout + out.stderr)


if __name__ == '__main__': unittest.main()
