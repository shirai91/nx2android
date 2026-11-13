"""Exercise production storage-key construction using the real NVN object types."""
import os, re, shutil, subprocess, tempfile, unittest
from pathlib import Path
W=Path(__file__).resolve().parents[1]
class TextureFormatAliases(unittest.TestCase):
    def test_family_identity_and_exclusions(self):
        src=(W/'nxnative/dm/android/gfx_vk_tex.c').read_text()
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
    for(packaged=1;packaged<=3;packaged++)assert(!same(a,b));packaged=0;
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
    puts("64 canonical family pairs and 13 exclusion cases passed");
}
'''
        cc=os.environ.get('CC') or shutil.which('clang')
        if not cc:self.skipTest('clang unavailable')
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'test.c').write_text(code)
            args=[cc,'-std=c11','-fsanitize=address,undefined','-I'+str(W/'nxnative/dm/gfx/nvn'),'-I'+str(W/'nxnative/dm/gfx/nvn/include'),'-I'+str(W/'nxnative/dm/gfx/tex'),str(p/'test.c'),'-o',str(p/'test')]
            subprocess.run(args,check=True,capture_output=True,text=True)
            result=subprocess.run([str(p/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=='__main__':unittest.main()
