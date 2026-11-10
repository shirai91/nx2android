"""Exercise the real initial-content decision with synthetic texture storage."""
import shutil, subprocess, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def source():
    text=(ROOT/'nxnative/dm/android/gfx_vk_tex.c').read_text()
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
enum { NVN_TEXTURE_FLAGS_IMAGE=1, NVN_TEXTURE_FLAGS_COMPRESSIBLE=2, NVN_TEXTURE_FLAGS_LINEAR=4,
 NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET=8, NVN_TEXTURE_FLAGS_SPARSE=16,
 NVN_TEXTURE_FLAGS_DISPLAY=32, NVN_TEXTURE_FLAGS_VIDEO_DECODE=64 };
typedef struct { unsigned flags; } pool_t;
typedef struct { const pool_t *pool; const uint8_t *mem; int has_layout; int64_t off; struct { unsigned target,samples,flags,width,height,depth,format; } d; struct { uint64_t size; } L; } tex_t;
typedef struct { int compressed,depth,stencil; } vkfmt_t;
typedef struct { int gpu_w; uint64_t pool; int64_t off,size; } vkimg_t;
static struct { uint32_t cap; void *vals[4]; } g_by_key={.cap=4};
static int g_img_mu, marked;
#define pthread_mutex_lock(p) ((void)(p))
#define pthread_mutex_unlock(p) ((void)(p))
static void mtl_tex_cpu_written(const tex_t *t) { (void)t; marked++; }
#define VKLOG(...) ((void)0)
''' + function('mem_any')+'\n'+function('cpu_storage_gpu_written')+'\n'+function('cpu_content_tex')+r'''
int main(void) {
 uint8_t pixels[64]={1}; pool_t pool={0}; vkfmt_t rgba={0}, bc={1,0,0};
 tex_t t={.pool=&pool,.mem=pixels,.d={.target=1,.samples=1,.width=4,.height=4,.depth=1},.L={64}};
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
 return 0;
}
'''

class CpuTexture(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'),'a host C compiler is required; also run with the NDK on Android')
    def test_initial_cpu_content(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); (p/'test.c').write_text(source())
            subprocess.run(['cc','-std=c11',str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
