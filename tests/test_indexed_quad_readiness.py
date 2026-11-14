"""CPU quad conversion sees the same completed/deferred bytes as GPU reads."""
from pathlib import Path
import os, subprocess, tempfile, unittest
import host_cc
from test_cpu_handle_readiness import function
A=Path(__file__).resolve().parents[1]/'nxnative/dm/android'


class IndexedQuadReadiness(unittest.TestCase):
    @unittest.skipUnless(host_cc.ubsan(), 'host C compiler with UBSan required')
    def test_index_widths_deferred_snapshot_and_bounded_preflight(self):
        src=(A/'gfx_vk_exec.c').read_text(encoding='utf-8')
        ready=function(src,'static int quad_index_ready(') if 'static int quad_index_ready(' in src else 'static int quad_index_ready(const p_draw_idx *d){return 1;}'
        convert=function(src,'static int quad_indices(')
        draw=function(src, 'static void draw_idx(')
        preflight=next(line for line in draw.splitlines() if 'if (!quad_index_ready(d)) return;' in line)
        reject=next(line for line in draw.splitlines() if 'if (!vk_buf_at(d->ibuf, 1,' in line)
        code=r"""
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define die(...) abort()
typedef uint64_t VkDeviceSize,VkBuffer;
typedef struct {int prim,itype,count;uint64_t ind,ibuf;} p_draw_idx;
enum {NVN_DRAW_PRIMITIVE_QUADS=7};
static uint8_t memory[128],snapshot[128],scratch[1024];static uint32_t *output;static unsigned allocated;
static unsigned reads,waits,scratch_calls,heap_grows;static size_t heap_size;static uint64_t waited_size;
// Pool registration is only one 64-byte chunk; the CPU mapping continues beyond it.
static int mapped(uint64_t a,uint64_t n){return a>=(uintptr_t)memory && a-(uintptr_t)memory<sizeof memory && n<=sizeof memory-(a-(uintptr_t)memory);}
static void *vk_pool_of(uint64_t a,uint64_t n){return mapped(a,n) && a-(uintptr_t)memory<64 && n<=64-(a-(uintptr_t)memory)?memory:NULL;}
static void cpu_ready(uint64_t a,uint64_t n){assert(mapped(a,n));waits++;waited_size=n;}
static void vk_ubd_read(uint64_t a,uint64_t n,uint8_t *out){assert(mapped(a,n));memcpy(out,snapshot+(a-(uintptr_t)memory),n);reads++;}
static void *vk_scratch(uint64_t n,unsigned align,VkBuffer *b,VkDeviceSize *o){scratch_calls++;allocated=(allocated+3)&~3u;assert(n<=sizeof scratch-allocated);output=(uint32_t*)(scratch+allocated);allocated+=(unsigned)n;*b=1;*o=0;return output;}
static void *snapshot_realloc(void *p,size_t n){heap_grows++;heap_size=n;return realloc(p,n);}
READY
static int diagnostics,accepted;
static int vk_buf_at(uint64_t a,uint64_t n,VkBuffer *b,VkDeviceSize *o){return vk_pool_of(a,n)!=NULL;}
static void diagnostic(int code,const char *message){assert(code==56);assert(!strcmp(message,"index buffer outside the memory pools"));diagnostics++;}
#define VKRED_ONCE(code,message) diagnostic(code,message)
static void draw_rejection(const p_draw_idx *d){
PREFLIGHT
 VkBuffer ib;VkDeviceSize io;
REJECT
 accepted++;
}
#define realloc snapshot_realloc
CONVERT
#undef realloc
int main(void){
 // Match the existing draw path: a registered first byte, with indices crossing its chunk.
 p_draw_idx crossing={.prim=7,.itype=2,.count=4,.ibuf=(uintptr_t)(memory+60)};
 assert(quad_index_ready(&crossing));assert(waits==1&&waited_size==16);
 for(unsigned i=0;i<4;i++){uint32_t value=21+i;memcpy(snapshot+60+i*4,&value,4);}
 VkBuffer cross_buf;VkDeviceSize cross_off;
 assert(quad_indices(4,memory+60,2,0,&cross_buf,&cross_off)==6);
 const uint32_t cross_expected[]={21,22,23,21,23,24};
 assert(!memcmp(output,cross_expected,sizeof cross_expected));
 assert(scratch_calls==1&&allocated==24&&heap_grows==1&&heap_size>=16);
 for(int type=0;type<3;type++){
  unsigned bytes=1u<<type;memset(memory,0,sizeof memory);memset(snapshot,0,sizeof snapshot);
  // Unaligned guest input is copied to an aligned heap snapshot before typed reads.
  const uint32_t values[]={0,UINT32_MAX,0x80008080u,0x7fff7f7fu,1,0xfffffffeu,0x81018301u,2};
  uint32_t mask=type==0?UINT8_MAX:type==1?UINT16_MAX:UINT32_MAX;
  for(unsigned i=0;i<8;i++)memcpy(snapshot+1+i*bytes,&values[i],bytes);
  p_draw_idx d={.prim=7,.itype=type,.count=9,.ibuf=(uintptr_t)(memory+1)};
  unsigned old=waits;assert(quad_index_ready(&d));assert(waits==old+1&&waited_size==9*bytes);
  VkBuffer b;VkDeviceSize o;unsigned before=reads,alloc_before=scratch_calls;assert(quad_indices(9,memory+1,type,0,&b,&o)==12);
  assert(reads==before+1&&scratch_calls==alloc_before+1); // One immutable snapshot; avoid tracker scans per expanded index.
  const uint32_t expected[]={values[0]&mask,values[1]&mask,values[2]&mask,values[0]&mask,values[2]&mask,values[3]&mask,values[4]&mask,values[5]&mask,values[6]&mask,values[4]&mask,values[6]&mask,values[7]&mask};
  assert(!memcmp(output,expected,sizeof expected));
 }
 assert(reads>0);
 // Peak-sized CPU heap storage is reused by repeated and smaller draws.
 unsigned grown=heap_grows;size_t capacity=heap_size;VkBuffer reuse_buf;VkDeviceSize reuse_off;
 assert(quad_indices(9,memory+1,2,0,&reuse_buf,&reuse_off)==12);assert(heap_grows==grown&&heap_size==capacity);
 assert(quad_indices(12,memory+1,2,0,&reuse_buf,&reuse_off)==18);assert(heap_grows==grown+1&&heap_size>=48);
 assert(quad_indices(4,memory+1,0,0,&reuse_buf,&reuse_off)==6);assert(heap_grows==grown+1);

 p_draw_idx d={.prim=7,.itype=2,.count=4,.ibuf=(uintptr_t)memory};unsigned old=waits;
 d.ibuf=UINT64_MAX-3;draw_rejection(&d);assert(!diagnostics&&!accepted);
 d.ibuf=(uintptr_t)(memory+64);draw_rejection(&d);assert(diagnostics==1&&!accepted);
 assert(waits==old); // Invalid pool addresses reach the existing rejection/log without waiting.
 d.ibuf=(uintptr_t)memory;d.count=-1;assert(!quad_index_ready(&d));
 d.count=3;assert(!quad_index_ready(&d));d.count=4;d.itype=3;assert(!quad_index_ready(&d));
 assert(waits==old);
 d.prim=4;assert(quad_index_ready(&d));d.prim=7;d.ind=1;assert(quad_index_ready(&d));assert(waits==old);
 VkBuffer b;VkDeviceSize o;unsigned before=reads;assert(quad_indices(4,NULL,0,5,&b,&o)==6);
 const uint32_t generated[]={5,6,7,5,7,8};assert(!memcmp(output,generated,sizeof generated)&&reads==before);
 assert(quad_indices(3,NULL,0,5,&b,&o)==0);
 puts("CPU quad index widths, deferred bytes, consumed bounds and native paths PASS");
}
""".replace('READY',ready).replace('CONVERT',convert).replace('PREFLIGHT',preflight).replace('REJECT',reject)
        with tempfile.TemporaryDirectory(prefix='nx2a-index-ready-') as tmp:
            c=Path(tmp)/'check.c';exe=Path(tmp)/'check';c.write_text(code, encoding='utf-8')
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Wno-unused-parameter','-fsanitize=undefined','-fno-sanitize-recover=all',*(['-fsanitize=address'] if os.environ.get('NX2A_TEST_SANITIZE') else []),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
