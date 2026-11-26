"""Exercise the actual SPIR-V input rewrite, including component pointer types."""
import shutil, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include "vk_spv_vertex.h"
int main(void) {
    uint32_t w[] = {
        0x07230203,0x10000,0,32,0,
        4u<<16|71,5,30,1,
        4u<<16|21,1,32,1,
        4u<<16|23,2,1,4,
        4u<<16|32,3,1,2,
        4u<<16|32,4,1,1,
        4u<<16|59,3,5,1,
        5u<<16|54,1,10,0,1,
        4u<<16|61,2,12,5,
        5u<<16|65,4,13,5,0,
        4u<<16|61,1,14,13,
        1u<<16|56
    };
    for (uint32_t sign=0; sign<2; sign++) {
        w[12]=sign;
        uint32_t nw=0, *o=spv_vertex_signedness(w,sizeof(w)/4,2,&nw);
        assert(o && nw>sizeof(w)/4 && o[3]>32);
        spv_vtype types[128]={0}; uint32_t var=0, component=0, casts=0, loads=0;
        for (uint32_t i=5;i<nw;) {
            uint32_t n=o[i]>>16, op=o[i]&65535; assert(n && i+n<=nw);
            if (op==21 || op==23 || op==32) types[o[i+1]]=(spv_vtype){op,o[i+2],o[i+3]};
            if (op==59 && o[i+2]==5) var=o[i+1];
            if (op==65) component=o[i+1];
            if (op==61) { loads++; assert(o[i+2]!=12 && o[i+2]!=14); }
            if (op==124) { casts++; assert((o[i+1]==2 && o[i+2]==12) || (o[i+1]==1 && o[i+2]==14)); }
            i+=n;
        }
        assert(casts==2 && loads==2 && var && component);
        uint32_t vector=types[var].b, scalar=types[vector].a;
        assert(types[vector].op==23 && types[scalar].op==21 && types[scalar].a==32 && types[scalar].b==(sign^1));
        assert(types[component].op==32 && types[component].b==scalar);
        free(o);
    }
    uint32_t n=0; assert(!spv_vertex_signedness(w,sizeof(w)/4,0,&n));
    // a float vec4 input at location 1 whose scaled format is fetched as uint (mask bit 16 + 1) or sint (bit 32 + 1): the input and its component pointer
    // become int, and both loads convert back to float (OpConvertUToF 112 / OpConvertSToF 111), never a bitcast
    uint32_t f[] = {
        0x07230203,0x10000,0,32,0,
        4u<<16|71,5,30,1,
        3u<<16|22,1,32,
        4u<<16|23,2,1,4,
        4u<<16|32,3,1,2,
        4u<<16|32,4,1,1,
        4u<<16|59,3,5,1,
        5u<<16|54,1,10,0,1,
        4u<<16|61,2,12,5,
        5u<<16|65,4,13,5,0,
        4u<<16|61,1,14,13,
        1u<<16|56
    };
    for (uint32_t sign=0; sign<2; sign++) {
        uint32_t nw=0, *o=spv_vertex_signedness(f,sizeof(f)/4,1ull<<(sign?33:17),&nw);
        assert(o && nw>sizeof(f)/4 && o[3]>32);
        spv_vtype types[128]={0}; uint32_t var=0, component=0, cvts=0, casts=0;
        for (uint32_t i=5;i<nw;) {
            uint32_t n=o[i]>>16, op=o[i]&65535; assert(n && i+n<=nw);
            if (op==21 || op==23 || op==32) types[o[i+1]]=(spv_vtype){op,o[i+2],o[i+3]};
            if (op==59 && o[i+2]==5) var=o[i+1];
            if (op==65) component=o[i+1];
            if (op==61) assert(o[i+2]!=12 && o[i+2]!=14);
            if (op==(sign?111u:112u)) { cvts++; assert((o[i+1]==2 && o[i+2]==12) || (o[i+1]==1 && o[i+2]==14)); }
            if (op==124) casts++;
            i+=n;
        }
        assert(cvts==2 && casts==0 && var && component);
        uint32_t vector=types[var].b, scalar=types[vector].a;
        assert(types[vector].op==23 && types[vector].b==4 && types[scalar].op==21 && types[scalar].a==32 && types[scalar].b==sign);
        assert(types[component].op==32 && types[component].b==scalar);
        free(o);
    }
    return 0;
}
'''

class VertexSignedness(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required; also exercised on Android with the NDK')
    def test_fetch_bits_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); (p/'test.c').write_text(SOURCE,encoding='utf-8')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'nxnative/dm/android'),str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
