"""Exercise production storage lookup, uploads and lifetime with real NVN object types."""
import os, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_cc
W=Path(__file__).resolve().parents[1]
class TextureFormatAliases(unittest.TestCase):
    def test_family_identity_upload_visibility_and_lifetime(self):
        src=(W/'nxnative/dm/android/gfx_vk_tex.c').read_text(encoding='utf-8')
        code=(W/'tests/texture_cache_harness.c').read_text(encoding='utf-8')
        def part(start,end):
            return src[src.index(start):src.index(end,src.index(start))]
        code=code.replace('/* CACHE KEYS */',part('static NVNformat texture_backing_format(', '// The sRGB / UNORM twin'))
        code=code.replace('/* CACHE LIFETIME */',part('enum { EVICT_KEEP', 'static vkimg_t *vk_tex_(const tex_t *t);'))
        code=code.replace('/* LOOKUP */',part('static vkimg_t *vk_tex_(const tex_t *t) {', '// Packaged textures give up'))
        code=code.replace('/* DROP POOL */',part('void vk_tex_drop_pool(', '// CPU-written textures'))
        self.run_c(code)

    def test_packaged_metadata_initialization_and_reinitialization(self):
        src=(W/'nxnative/dm/gfx/nvn/hle_nvn.c').read_text(encoding='utf-8')
        start=src.index('NVN(nvnTextureInitialize,')
        init=src[start:src.index('NVN(nvnTextureFinalize,',start)]
        code=r'''#include "nvn_obj.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define NVN(name,ret,...) static ret name(__VA_ARGS__)
#define TX(t) ((tex_t *)(t))
#define LOG(...) ((void)0)
#define OLOG(...) ((void)0)
#define DM_GAME_TEX_CPU_UNCACHED 0 // Default generated profile value.
static int marks, releases;
static void ft_del(uint64_t serial) {(void)serial;}
static void ft_add(const tex_t *t) {(void)t;}
void mtl_tex_release(tex_t *t) {(void)t;releases++;}
void mtl_tex_init(const tex_t *t) {(void)t;}
void mtl_tex_cpu_written(const tex_t *t) {(void)t;marks++;}
uint64_t nvn_new_serial(void) {static uint64_t serial;return ++serial;}
'''+init+r'''
int main(void) {
    uint8_t memory[4096]={0}, packaged[4096];memset(packaged,0x83,sizeof packaged);
    pool_t pool={.mem=memory};
    tex_builder_t b={.pool=&pool,.pkg_data=packaged,.d={.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=8,.height=8,.depth=1,.levels=1}};
    struct {tex_t t;uint8_t guard[16];} obj={0};memset(obj.guard,0xa5,sizeof obj.guard);
    assert(nvnTextureInitialize((NVNtexture*)&obj.t,(const NVNtextureBuilder*)&b));
    assert(obj.t.has_pkg_data && !obj.t.has_layout && marks==1 && obj.t.L.size<=sizeof memory);
    assert(!memcmp(memory,packaged,obj.t.L.size));tex_t copy=obj.t;assert(copy.has_pkg_data);
    uint64_t old_serial=obj.t.serial;b.pkg_data=NULL;
    assert(nvnTextureInitialize((NVNtexture*)&obj.t,(const NVNtextureBuilder*)&b));
    assert(!obj.t.has_pkg_data && releases==1 && obj.t.serial!=old_serial);
    mtl_tex_cpu_written(&obj.t);assert(!obj.t.has_pkg_data);
    for(unsigned i=0;i<sizeof obj.guard;i++)assert(obj.guard[i]==0xa5);
}
'''
        self.run_c(code, str(W/'nxnative/dm/gfx/nvn/nvn_tex.c'))

    def run_c(self, code, *extra_sources):
        cc=os.environ.get('CC') or shutil.which('cc') or shutil.which('clang')
        if not cc:self.skipTest('host C compiler unavailable')
        if not host_cc.asan_ubsan(cc):self.skipTest('selected compiler cannot link ASan/UBSan')
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'test.c').write_text(code,encoding='utf-8')
            exe=p/('test.exe' if os.name=='nt' else 'test')
            args=[cc,'-std=c11','-fsanitize=address,undefined','-I'+str(W/'nxnative/dm/gfx/nvn'),'-I'+str(W/'nxnative/dm/gfx/nvn/include'),'-I'+str(W/'nxnative/dm/gfx/tex'),str(p/'test.c'),*extra_sources,'-o',str(exe)]
            build=subprocess.run(args,capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=='__main__':unittest.main()
