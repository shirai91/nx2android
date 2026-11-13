"""Exercise production storage-key construction using the real NVN object types."""
import os, re, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_cc
W=Path(__file__).resolve().parents[1]
class TextureFormatAliases(unittest.TestCase):
    def test_family_identity_and_exclusions(self):
        src=(W/'nxnative/dm/android/gfx_vk_tex.c').read_text(encoding='utf-8')
        helper=''
        if 'static NVNformat texture_backing_format(' in src:
            begin=src.index('static NVNformat texture_backing_format(')
            end=src.index('\n}',begin)+2
            helper=src[begin:end]
        begin=src.index('akey k; memset(');end=src.index('    pthread_mutex_lock(&g_img_mu);',begin)
        key=src[begin:end]
        key_type=re.search(r"typedef struct \{ uint64_t pool; int64_t off; nvn_texdesc d;[^\n]* akey;",src).group()
        code=r'''#include "nvn_obj.h"
#include <assert.h>
#include <stdio.h>
#include "nvn_hmap.h"
'''+key_type+r'''
typedef struct { int depth,stencil; } vkfmt_t;
static unsigned packaged;
static int packaged_tex(const tex_t *t) { return (packaged & (1u << (t->serial-1))) || t->has_layout || (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR); }
'''+helper+r'''
static akey key_of(const tex_t *t) { vkfmt_t fmt={0};const vkfmt_t *f=&fmt;
'''+key+r'''
return k; }
static int same(tex_t a,tex_t b) { akey x=key_of(&a),y=key_of(&b);return !memcmp(&x,&y,sizeof x); }
int main(void) {
    char memory[1];pool_t pool={.mem=(void*)memory};
    tex_t a={.serial=1,.pool=&pool,.mem=(void*)memory,.d={.width=8,.height=8,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}},b=a;b.serial=2;
    const NVNformat rgba[]={NVN_FORMAT_RGBA8,NVN_FORMAT_RGBA8_SRGB,NVN_FORMAT_RGBX8,NVN_FORMAT_RGBX8_SRGB};
    const NVNformat bgra[]={NVN_FORMAT_BGRA8,NVN_FORMAT_BGRA8_SRGB,NVN_FORMAT_BGRX8,NVN_FORMAT_BGRX8_SRGB};
    for(int family=0;family<2;family++)for(int array=0;array<2;array++)for(int i=0;i<4;i++)for(int j=0;j<4;j++) {
        a.d.target=b.d.target=array?NVN_TEXTURE_TARGET_2D_ARRAY:NVN_TEXTURE_TARGET_2D;
        a.d.format=(family?bgra:rgba)[i];b.d.format=(family?bgra:rgba)[j];assert(same(a,b));
    }
    a.d.target=b.d.target=NVN_TEXTURE_TARGET_2D;a.d.format=NVN_FORMAT_RGBA8;b.d.format=NVN_FORMAT_RGBA8_SRGB;
    // First lookup, then WriteTexels/flush/first-touch marks only one serial.
    nvn_hmap cache={.klen=sizeof(akey)};
    akey before=key_of(&a);nvn_hm_put(&cache,&before,&a);
    packaged=1;akey after=key_of(&a);
    assert(nvn_hm_get(&cache,&after)==&a && cache.n==1);
    assert(!memcmp(&before,&after,sizeof before));
    b.d.format=a.d.format;assert(same(a,b));akey peer=key_of(&b);
    assert(nvn_hm_get(&cache,&peer)==&a);free(cache.keys);free(cache.vals);packaged=0;b.d.format=NVN_FORMAT_RGBA8_SRGB;
    for(int mask=1;mask<=3;mask++) { a.has_pkg_data=mask&1;b.has_pkg_data=(mask>>1)&1;assert(!same(a,b)); }
    a.has_pkg_data=b.has_pkg_data=0;
    a.d.flags=b.d.flags=NVN_TEXTURE_FLAGS_LINEAR;assert(!same(a,b));a.d.flags=b.d.flags=0;
    b.d.flags=NVN_TEXTURE_FLAGS_IMAGE;assert(!same(a,b));b.d.flags=0;
    b.d.width=16;assert(!same(a,b));b.d.width=8;
    b.d.format=NVN_FORMAT_BGRA8;assert(!same(a,b));b.d.format=NVN_FORMAT_RGBA8_SRGB;
    a.has_layout=b.has_layout=1;assert(!same(a,b));a.has_layout=b.has_layout=0;
    pool.flags=NVN_MEMORY_POOL_FLAGS_VIRTUAL;assert(!same(a,b));pool.flags=0;
    a.mem=b.mem=NULL;assert(!same(a,b));a.mem=b.mem=(void*)memory;
    a.pool=b.pool=NULL;assert(!same(a,b));a.pool=b.pool=&pool;
    a.d.samples=b.d.samples=4;assert(!same(a,b));a.d.samples=b.d.samples=0;
    a.d.target=b.d.target=NVN_TEXTURE_TARGET_3D;assert(!same(a,b));
    a.d.target=b.d.target=NVN_TEXTURE_TARGET_CUBEMAP;assert(!same(a,b));
    puts("64 canonical family pairs, stable CPU-written identity and packaged exclusions passed");
}
'''
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
        cc=os.environ.get('CC') or shutil.which('clang')
        if not cc:self.skipTest('clang unavailable')
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
