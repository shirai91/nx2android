"""Exercise the production NVN setters and queue queries with real ABI types."""
from pathlib import Path
import shutil, subprocess, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'nxnative/dm/gfx/nvn'
NAMES = ('SamplerBuilderSetDefaults', 'SamplerBuilderSetDevice', 'SamplerBuilderSetBorderColor',
         'SamplerBuilderSetBorderColori', 'SamplerBuilderSetBorderColorui', 'SamplerInitialize', 'SamplerGetBorderColor',
         'QueueInitialize', 'QueueGetTotalCommandMemoryUsed', 'QueueGetTotalControlMemoryUsed', 'QueueGetTotalComputeMemoryUsed',
         'QueueResetMemoryUsageCounts', 'CommandBufferSetSubpixelPrecisionBias')

def production():
    source = (NVN / 'hle_nvn.c').read_text(encoding='utf-8')
    obj = (NVN / 'nvn_obj.h').read_text(encoding='utf-8')
    types = '\n'.join(line for line in obj.splitlines() if any('} ' + name + ';' in line for name in ('samp_par', 'samp_builder_t', 'samp_t', 'queue_t', 'queue_builder_t')))
    functions = []
    for name in NAMES:
        start = source.index('NVN(nvn' + name + ','); end = source.index('{', start); depth = 1; end += 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}'); end += 1
        functions.append(source[start:end])
    return '''#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "nvn_protos.h"
#define MAGIC(a,b,c,d) ((uint32_t)(a)|(uint32_t)(b)<<8|(uint32_t)(c)<<16|(uint32_t)(d)<<24)
#define FITS(T,H) _Static_assert(sizeof(T)<=sizeof(H), "object exceeds NVN ABI")
#define SB(b) (&((samp_builder_t *)(b))->p)
#define SP(s) (&((const samp_t *)(s))->p)
#define OLOG(...) ((void)0)
static int warnings;
#define ONCE_LOG(...) do { static int seen; if (!seen++) warnings++; } while (0)
#define NVN(name,ret,...) static ret h_##name(__VA_ARGS__); static NVNT_##name entry_##name=h_##name; static ret h_##name(__VA_ARGS__)
''' + types + '\n' + '\n'.join(functions) + '''
int main(void) {
 NVNsamplerBuilder b={0}; NVNsampler s={0}; NVNdevice d={0}; float got[4];
 entry_nvnSamplerBuilderSetDevice(&b,&d); entry_nvnSamplerBuilderSetDefaults(&b);
 uint32_t u[4]={0,1,7,255}; entry_nvnSamplerBuilderSetBorderColorui(&b,u);
 assert(entry_nvnSamplerInitialize(&s,&b)); entry_nvnSamplerGetBorderColor(&s,got);
 for(int i=0;i<4;i++) assert(got[i]==(float)u[i]);
 int v[4]={-4,0,1,17}; entry_nvnSamplerBuilderSetBorderColori(&b,v);
 assert(entry_nvnSamplerInitialize(&s,&b)); entry_nvnSamplerGetBorderColor(&s,got);
 for(int i=0;i<4;i++) assert(got[i]==(float)v[i]);
 assert(((samp_builder_t *)&b)->dev==&d);
 float f[4]={0.25f,0.5f,0.75f,1}; entry_nvnSamplerBuilderSetBorderColor(&b,f);
 entry_nvnSamplerInitialize(&s,&b); entry_nvnSamplerGetBorderColor(&s,got); assert(!memcmp(f,got,sizeof f));
 NVNqueue q={0}; NVNqueueBuilder qb={0}; ((queue_builder_t *)&qb)->dev=&d;
 assert(entry_nvnQueueInitialize(&q,&qb)); ((queue_t *)&q)->submits=47;
 assert(entry_nvnQueueGetTotalCommandMemoryUsed(&q)==0);
 assert(entry_nvnQueueGetTotalControlMemoryUsed(&q)==0);
 assert(entry_nvnQueueGetTotalComputeMemoryUsed(&q)==0);
 entry_nvnQueueResetMemoryUsageCounts(&q); assert(((queue_t *)&q)->submits==47); assert(((queue_t *)&q)->dev==&d);
 NVNcommandBuffer cb={0}; entry_nvnCommandBufferSetSubpixelPrecisionBias(&cb,0,0); assert(warnings==0);
 entry_nvnCommandBufferSetSubpixelPrecisionBias(&cb,1,0); assert(warnings==1);
 puts("NVN API semantics: PASS"); return 0;
}
'''

@unittest.skipUnless(shutil.which('cc'), 'host C compiler required; the same fixture can run on Android with the NDK')
class NvnApiSemantics(unittest.TestCase):
    def test_typed_border_state_and_queue_accounting(self):
        with tempfile.TemporaryDirectory() as td:
            src=Path(td)/'api.c'; exe=Path(td)/'api'; src.write_text(production(),encoding='utf-8')
            subprocess.run(['cc','-std=gnu11','-O2','-fno-strict-aliasing','-I'+str(NVN),'-I'+str(NVN/'include'),str(src),'-o',str(exe)],check=True,capture_output=True)
            self.assertIn('PASS',subprocess.check_output([str(exe)],text=True))

if __name__=='__main__': unittest.main()
