"""Exercise the real initial-content decision with synthetic texture storage."""
import shutil, subprocess, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def source():
    text=(ROOT/'nxnative/dm/android/gfx_vk_tex.c').read_text(encoding='utf-8')
    def function(name):
        start=text.index('static int '+name+'(')
        # Skip the forward declaration.
        if text.index(';',start)<text.index('{',start): start=text.index('static int '+name+'(',start+1)
        return text[start:text.index('\n}',start)+2]
    return r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
enum { NVN_MEMORY_POOL_FLAGS_VIRTUAL=1, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS=2 };
enum { NVN_TEXTURE_TARGET_2D=1 };
enum { NVN_TEXTURE_FLAGS_IMAGE=1, NVN_TEXTURE_FLAGS_COMPRESSIBLE=2, NVN_TEXTURE_FLAGS_LINEAR=4,
 NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET=8, NVN_TEXTURE_FLAGS_SPARSE=16,
 NVN_TEXTURE_FLAGS_DISPLAY=32, NVN_TEXTURE_FLAGS_VIDEO_DECODE=64 };
typedef struct { unsigned flags; } pool_t;
typedef struct { const pool_t *pool; const uint8_t *mem; int has_layout; int64_t off; struct { unsigned target,samples,flags,width,height,depth,format; } d; struct { uint64_t size; } L; } tex_t;
typedef struct { int compressed,depth,stencil,dec; } vkfmt_t;
typedef struct { int gpu_w; uint64_t pool; int64_t off,size; } vkimg_t;
static struct { uint32_t cap; void *vals[4]; } g_by_key={.cap=4};
static int g_img_mu, marked;
#define pthread_mutex_lock(p) ((void)(p))
#define pthread_mutex_unlock(p) ((void)(p))
static void mtl_tex_cpu_written(const tex_t *t) { (void)t; marked++; }
#define VKLOG(...) ((void)0)
static const char *fixture_getenv(const char *name) {
    if (!strcmp(name, "DM_NVN_CPU_TEX2D_FIRST_USE")) return TEX2D_ON ? "1" : NULL;
    if (!strcmp(name, "DM_NVN_CPU_NO_ACCESS_IMAGE_FIRST_USE")) return IMAGE_ON ? "1" : NULL;
    return NULL;
}
#define getenv fixture_getenv
''' + next(line for line in text.splitlines() if line.startswith('enum { TEX_CONTENT_NONE'))+'\n'+function('mem_any')+'\n'+function('cpu_storage_gpu_written')+'\n'+function('cpu_content_tex')+r'''
int main(void) {
 uint8_t pixels[64]={1}; pool_t pool={0}; vkfmt_t rgba={0}, bc={1,0,0}, dec_bc={0,0,0,1};
 tex_t t={.pool=&pool,.mem=pixels,.d={.target=1,.samples=1,.width=4,.height=4,.depth=1},.L={64}};
 t.d.flags=NVN_TEXTURE_FLAGS_IMAGE; assert(cpu_content_tex(&t,&bc)==1);
 t.d.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE; assert(cpu_content_tex(&t,&bc)==DM_GAME_TEX_CPU_UNCACHED);
 pool.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS;
 t.d.flags=NVN_TEXTURE_FLAGS_IMAGE; marked=0;
 int image_content=IMAGE_ON ? TEX_CONTENT_INITIAL : TEX_CONTENT_NONE;
 assert(cpu_content_tex(&t,&rgba)==image_content && marked==0);
 vkimg_t written={.gpu_w=1,.pool=(uint64_t)(uintptr_t)&pool,.off=16,.size=16}; g_by_key.vals[0]=&written;
 assert(cpu_content_tex(&t,&rgba)==TEX_CONTENT_NONE);
 written.off=64; assert(cpu_content_tex(&t,&rgba)==image_content); g_by_key.vals[0]=0;
 t.d.target=2; assert(cpu_content_tex(&t,&rgba)==TEX_CONTENT_NONE); t.d.target=NVN_TEXTURE_TARGET_2D;
 rgba.depth=1; assert(cpu_content_tex(&t,&rgba)==TEX_CONTENT_NONE); rgba.depth=0;
 t.d.flags|=NVN_TEXTURE_FLAGS_DISPLAY; assert(cpu_content_tex(&t,&rgba)==TEX_CONTENT_NONE);
 t.d.flags=NVN_TEXTURE_FLAGS_IMAGE;
 pool.flags|=NVN_MEMORY_POOL_FLAGS_VIRTUAL; assert(cpu_content_tex(&t,&rgba)==TEX_CONTENT_NONE);
 pool.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS;
 memset(pixels,0,sizeof pixels); assert(cpu_content_tex(&t,&rgba)==TEX_CONTENT_NONE);
 pixels[63]=255; assert(cpu_content_tex(&t,&rgba)==image_content && marked==0);
 pool.flags=0; assert(cpu_content_tex(&t,&rgba)==TEX_CONTENT_NONE);
 /* a BC format decoded on the CPU (compressed 0, dec 1) is still block-compressed in guest memory: it is uploaded
    on first use, and the CPU_NO_ACCESS gate above (about who may write the storage) does not apply to it */
 t.d.flags=NVN_TEXTURE_FLAGS_IMAGE; pool.flags=0; assert(cpu_content_tex(&t,&dec_bc)==1);
 pool.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS; assert(cpu_content_tex(&t,&dec_bc)==1); pool.flags=0;
 t.d.flags=0; marked=0;
 if (!TEX2D_ON) {   /* without the profile switch an uncompressed 2D texture keeps its old path; compressed ones are still uploaded */
  assert(cpu_content_tex(&t,&rgba)==0 && marked==0); assert(cpu_content_tex(&t,&bc)==1);
  assert(cpu_content_tex(&t,&dec_bc)==1);   /* and a CPU-decoded one is uploaded without the 2D switch too */
  return 0; }
 assert(cpu_content_tex(&t,&rgba)==1 && marked==1);
 vkimg_t alias={.gpu_w=1,.pool=(uint64_t)(uintptr_t)&pool,.off=16,.size=16}; g_by_key.vals[0]=&alias;
 assert(cpu_content_tex(&t,&rgba)==0);
 alias.off=128; assert(cpu_content_tex(&t,&rgba)==1);
 alias.off=16; alias.gpu_w=0; assert(cpu_content_tex(&t,&rgba)==1); g_by_key.vals[0]=0;
 rgba.depth=1; assert(cpu_content_tex(&t,&rgba)==0); rgba.depth=0;
 t.d.flags=NVN_TEXTURE_FLAGS_IMAGE; assert(cpu_content_tex(&t,&rgba)==0); t.d.flags=0;
 pool.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS; assert(cpu_content_tex(&t,&rgba)==0); pool.flags=0;
 memset(pixels,0,sizeof pixels); assert(cpu_content_tex(&t,&rgba)==0);
 pixels[63]=255; assert(cpu_content_tex(&t,&bc)==1);
 assert(cpu_content_tex(&t,&dec_bc)==1);   /* the same for a CPU-decoded BC format, with the 2D switch on */
 return 0;
}
'''

class CpuTexture(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'),'a host C compiler is required; also run with the NDK on Android')
    def test_initial_cpu_content(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); (p/'test.c').write_text(source(),encoding='utf-8')
            for on in (0,1):
                for uncached in (0,1):
                    for image in (0,1):
                        exe=p/f'test{on}_{uncached}_{image}'
                        subprocess.run(['cc','-std=c11',f'-DTEX2D_ON={on}',f'-DIMAGE_ON={image}',
                                        f'-DDM_GAME_TEX_CPU_UNCACHED={uncached}',str(p/'test.c'),'-o',str(exe)],check=True)
                        subprocess.run([str(exe)],check=True)
