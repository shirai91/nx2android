"""Execute the production GPU-write tracking, readiness and descriptor preflight."""
from pathlib import Path
import shutil, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
A=ROOT/'nxnative/dm/android'
def function(s, signature):
    i=s.index(signature); start=s.index('{',i); depth=1; j=start+1
    while depth:
        depth+=(s[j]=='{')-(s[j]=='}');j+=1
    return s[i:j]+'\n'
class Readiness(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'),'host C compiler required')
    def test_ordering_coverage_pressure_and_preflight(self):
        core=(A/'gfx_vk_core.c').read_text(); replay=(A/'gfx_vk_exec.c').read_text(); header=(A/'gfx_vk.h').read_text()
        i=core.index('typedef struct { uint64_t lo, hi, fence; } gpuw_t;')
        decl=core[i:core.index('static void sub_gpu_wrote',i)]
        ready=function(core,'void vk_cpu_ready(') if 'void vk_cpu_ready(' in core else 'void vk_cpu_ready(uint64_t a,uint64_t n) {}'
        stage=''
        for sig in ('static void uniform_ready(', 'static void shader_cpu_ready('):
            if sig in replay: stage+=function(replay,sig)
        if not stage: stage='static void shader_cpu_ready(const vkshader_t *s,int stage){}'
        enums='\n'.join(l for l in header.splitlines() if l.startswith(('enum { RK_SUPPORT','enum { RS_NONE','typedef struct __attribute__((packed)) { uint8_t kind')))
        code=r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define die(...) abort()
static uint64_t g_fence,done;static int waits,cuts,resumes;
typedef struct {uint64_t lo,hi;} ubd_t;
static ubd_t g_ubd[512];static int g_nubd;
static void sub_gpu_wrote(uint64_t a,uint64_t n){}
uint64_t vk_fence_done(void){return done;}
int mtl_wait(uint64_t f,uint64_t timeout){assert(f<=g_fence);done=f;waits++;return 1;}
uint64_t mtl_sub_commit(void){cuts++;return ++g_fence;}
void vk_sub_resume(void){resumes++;}
DECL
WRITE
READY
ENUMS
typedef struct {uint64_t addr,size;} p_bind_buf;
static struct {p_bind_buf ubo[6][16];} S;
typedef struct {int nbind;const dmsv_bind *b;} vkshader_t;
static uint64_t address,length;static int reads;
static void cpu_ready(uint64_t a,uint64_t n){address=a;length=n;reads++;}
STAGE
static void reset(void){g_ngw=g_nubd=g_fence=done=waits=cuts=resumes=0;}
int main(void){
 reset();vk_gpu_wrote(100,8);vk_cpu_ready(104,4);assert(cuts==1&&resumes==1&&waits==1);
 reset();vk_gpu_wrote(100,8);vk_cpu_ready(108,4);assert(!cuts&&!waits);
 reset();vk_gpu_wrote(100,8);g_ubd[g_nubd++]=(ubd_t){100,104};g_ubd[g_nubd++]=(ubd_t){104,108};vk_cpu_ready(100,8);assert(!cuts&&!waits);
 reset();vk_gpu_wrote(100,8);g_ubd[g_nubd++]=(ubd_t){100,104};vk_cpu_ready(100,8);assert(cuts==1);
 reset();vk_gpu_wrote(100,8);g_fence=1;vk_cpu_ready(100,8);assert(waits==1&&!cuts&&done==1);
 reset();for(unsigned i=0;i<2049;i++)vk_gpu_wrote(100+i*64,8);vk_cpu_ready(100,8);assert(cuts==1);
 reset();vk_gpu_wrote(100,8);vk_cpu_ready(UINT64_MAX-1,8);vk_cpu_ready(100,0);assert(!cuts);
 dmsv_bind b={.kind=RK_TEX_UBO,.reg=7,.count=32,.off=352};vkshader_t s={1,&b};
 S.ubo[1][7]=(p_bind_buf){0x10000,65536};shader_cpu_ready(&s,1);assert(reads==1&&address==0x10160&&length==252);
 S.ubo[1][7].size=356;shader_cpu_ready(&s,1);assert(length==4);int before=reads;
 S.ubo[1][7].size=352;shader_cpu_ready(&s,1);assert(reads==before);
 b.off=0;b.count=1;S.ubo[1][7].size=0;shader_cpu_ready(&s,1);assert(reads==before+1&&address==0x10000&&length==4);
 b=(dmsv_bind){.kind=RK_SBUF_UBO,.reg=2,.off=16};S.ubo[5][2]=(p_bind_buf){0x20000,32};shader_cpu_ready(&s,5);assert(address==0x20010&&length==8);
 b=(dmsv_bind){.kind=RK_TEX_UBO,.reg=-1,.count=1,.samp=RS_UBO,.samp_arg=3|(64<<8)};S.ubo[0][3]=(p_bind_buf){0x30000,80};shader_cpu_ready(&s,0);assert(address==0x30040&&length==4);
 before=reads;shader_cpu_ready(&s,-1);shader_cpu_ready(&s,6);S.ubo[0][3].addr=UINT64_MAX-8;shader_cpu_ready(&s,0);assert(reads==before);
 puts("GPU readiness current/prior/coverage/pressure and descriptor bounds PASS");
}
"""
        code=code.replace('DECL',decl).replace('WRITE',function(core,'void vk_gpu_wrote(uint64_t a, uint64_t n) {')).replace('READY',ready).replace('ENUMS',enums).replace('STAGE',stage)
        with tempfile.TemporaryDirectory(prefix='nx2a-ready-') as tmp:
            c=Path(tmp)/'check.c';exe=Path(tmp)/'check';c.write_text(code)
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Wno-unused-parameter','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
