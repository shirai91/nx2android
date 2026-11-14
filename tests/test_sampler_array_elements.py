"""Existing donor S_SAMPU array semantics through the target packed DMSV ABI."""
from pathlib import Path
import os, struct, subprocess, tempfile, unittest
import host_cc
from test_cpu_handle_readiness import function
from test_shader_storage_flags import exporter
A=Path(__file__).resolve().parents[1]/'nxnative/dm/android'
class SamplerArray(unittest.TestCase):
    def test_exported_split_array_source_roundtrip(self):
        env=exporter()
        # Donor bindless.vk.txt: tex UBO7+352, independent sampler UBO8+128.
        b={'name':'fp_t3d_cb786442_10080058','type':'sampled','binding':0,'count':32,'dim':2}
        e=env['classify']('ps',b,{})
        self.assertEqual((e['kind'],e['reg'],e['off'],e['samp_ubo']),('tex_ubo',7,352,[8,128]))
        raw=env['refl_bin']([e],b'',(1,0,0));self.assertEqual(len(raw),40)
        fields=struct.unpack_from('<BBBBHHhBBii',raw,20)
        self.assertEqual((fields[7],fields[10]),(4,8|(128<<8)))
    @unittest.skipUnless(host_cc.ubsan(), 'host C compiler with UBSan required')
    def test_element_offsets_fixed_sources_and_preflight(self):
        src=(A/'gfx_vk_exec.c').read_text(encoding='utf-8');header=(A/'gfx_vk.h').read_text(encoding='utf-8')
        adapter=function(src,'static dmsv_bind sampler_element(') if 'static dmsv_bind sampler_element(' in src else 'static dmsv_bind sampler_element(const dmsv_bind *b,uint32_t k){return *b;}'
        preflight=function(src,'static void uniform_ready(')+function(src,'static void shader_cpu_ready(')
        decl='\n'.join(l for l in header.splitlines() if l.startswith(('enum { RK_SUPPORT','enum { RS_NONE','typedef struct __attribute__((packed)) { uint8_t kind')))
        code=r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
DECL
typedef struct {uint64_t addr,size;} p_bind_buf;
static struct {p_bind_buf ubo[6][16];} S;
typedef struct {int nbind;const dmsv_bind *b;} vkshader_t;
static uint64_t read_addr,read_size;
static void cpu_ready(uint64_t a,uint64_t n){read_addr=a;read_size=n;}
ADAPTER
PREFLIGHT
int main(void) {
 dmsv_bind b={.kind=RK_TEX_UBO,.reg=7,.count=32,.off=352,.samp=RS_UBO,.samp_arg=8|(128<<8)};
 for(unsigned k=0;k<32;k++) {dmsv_bind e=sampler_element(&b,k);assert((e.samp_arg&255)==8);assert((e.samp_arg>>8)==128+8*(int)k);assert(e.off==b.off&&e.reg==b.reg);}
 assert(b.samp_arg==(8|(128<<8)));
 for(int source=RS_NONE;source<RS_UBO;source++){b.samp=source;for(unsigned k=0;k<32;k++){dmsv_bind e=sampler_element(&b,k);assert(!memcmp(&e,&b,sizeof b));}}
 b.samp=RS_UBO;b.samp_arg=-1;assert(sampler_element(&b,7).samp_arg<0);
 b.samp_arg=INT32_MAX-7;assert(sampler_element(&b,0).samp_arg==b.samp_arg);assert(sampler_element(&b,1).samp_arg<0);
 b.samp_arg=8|(128<<8);b.reg=-1;vkshader_t s={1,&b};S.ubo[1][8]=(p_bind_buf){0x10000,65536};
 shader_cpu_ready(&s,1);assert(read_addr==0x10080&&read_size==252);
 b.count=0;shader_cpu_ready(&s,1);assert(read_size==252);
 b.count=1;shader_cpu_ready(&s,1);assert(read_size==4);
 b.count=32;S.ubo[1][8].size=128+8*7+4;shader_cpu_ready(&s,1);assert(read_size==60);
 puts("32 matching sampler elements, fixed sources, packed overflow and full preflight PASS");
}
""".replace('DECL',decl).replace('ADAPTER',adapter).replace('PREFLIGHT',preflight)
        with tempfile.TemporaryDirectory(prefix='nx2a-sampler-array-') as tmp:
            c=Path(tmp)/'check.c';exe=Path(tmp)/'check';c.write_text(code, encoding='utf-8')
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Wno-unused-parameter','-fsanitize=undefined','-fno-sanitize-recover=all',*(['-fsanitize=address'] if os.environ.get('NX2A_TEST_SANITIZE') else []),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
