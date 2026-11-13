"""Donor sparse-pool compaction, using nx2android's replay registration selection."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
ANDROID=ROOT/'nxnative/dm/android'

class ResourcePools(unittest.TestCase):
    def test_sparse_registration_maps_and_replay_history(self):
        header=ANDROID/'gfx_vk_resource_pool.h';impl=ANDROID/'gfx_vk_resource_pool.inc'
        self.assertTrue(header.exists() and impl.exists(),'resource pool runtime not ported')
        source=(ANDROID/'gfx_vk_tex.c').read_text();start=source.index('static const treg_ver *treg_at(');end=source.index('\n}',start)+2
        with tempfile.TemporaryDirectory(prefix='nx2a-resource-pool-') as tmp:
            work=Path(tmp);c=work/'pool.c';exe=work/'pool'
            c.write_text('''#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "gfx_vk_resource_pool.h"
typedef struct { unsigned content; } tex_t;
typedef struct { unsigned level; } view_t;
typedef struct { tex_t t; view_t v; unsigned char ok,has_v; unsigned long long seq; } treg_ver;
typedef struct { treg_ver cur; treg_ver *old; } treg_t;
typedef struct { unsigned filter; } samp_par;
enum { TREG_OLD=3 };
static treg_t *g_treg; static int g_ntreg;
static samp_par *g_sreg; static unsigned char *g_sreg_ok; static int g_nsreg;
static uint64_t g_tex_gen=1,g_samp_gen=1,job;
static pthread_mutex_t g_rmu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t nvn_replaying_job(void){return job;}
''' + source[start:end] + '''
#include "gfx_vk_resource_pool.inc"
int main(void){
 vk_resource_table t={0},s={0};
 assert(vk_resource_pool_build(&t,0,0)); assert(t.count==1 && t.map[0]==1 && !t.map[1] && !t.map[2]);
 g_ntreg=65540;g_treg=calloc(g_ntreg,sizeof *g_treg);g_tex_gen++;
 const unsigned ids[]={1,2,31,32,301,4096,65536};
 for(unsigned i=0;i<sizeof ids/sizeof *ids;i++)g_treg[ids[i]].cur=(treg_ver){.t={i%3+1},.ok=1,.seq=1};
 assert(vk_resource_pool_build(&t,0,0));assert(t.count==4);
 for(unsigned i=0;i<sizeof ids/sizeof *ids;i++)assert(t.map[ids[i]+1]==i%3+1);
 assert(!t.map[4] && !t.map[302+1] && !t.map[t.map[0]+1]);
 assert(vk_resource_pool_build(&t,0,30));assert(t.map[2]==1 && t.map[3]==2 && !t.map[1]);
 // Identical texture metadata with a different view is a separate descriptor.
 g_treg[32].cur.has_v=1;g_treg[32].cur.v.level=2;g_tex_gen++;
 assert(vk_resource_pool_build(&t,0,0));assert(t.count==5);
 // A queued job sees the same historical registration as ordinary texture lookup.
 treg_ver old[3]={g_treg[1].cur};g_treg[1].old=old;g_treg[1].cur.t.content=99;g_treg[1].cur.seq=10;g_tex_gen++;
 job=5;assert(vk_resource_pool_build(&t,0,0));assert(t.map[2]==t.map[65536+1]);
 job=11;assert(vk_resource_pool_build(&t,0,0));assert(t.map[2]!=t.map[65536+1]);
 g_treg[1].cur.ok=0;g_tex_gen++;assert(vk_resource_pool_build(&t,0,0));assert(!t.map[2]);
 g_nsreg=4600;g_sreg=calloc(g_nsreg,sizeof *g_sreg);g_sreg_ok=calloc(g_nsreg,1);g_samp_gen++;
 g_sreg_ok[1]=g_sreg_ok[4095]=g_sreg_ok[4096]=1;g_sreg[1].filter=g_sreg[4095].filter=7;
 assert(vk_resource_pool_build(&s,1,0));assert(s.map[0]==4096 && s.count==2 && s.map[2]==s.map[4096]);
 assert(vk_resource_pool_build(&s,1,1));assert(s.count==3 && s.map[1] && s.map[4096]);
 assert(!vk_resource_pool_build(&s,1,-1));
 free(t.map);free(t.handles);free(s.map);free(s.handles);free(g_treg);free(g_sreg);free(g_sreg_ok);
 puts("sparse texture/sampler pools, dedup, bounds, rebasing and replay history PASS");return 0;
}
''')
            subprocess.run([shutil.which('clang') or 'cc','-std=c11','-fsanitize=address,undefined','-g','-I',str(ANDROID),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__=='__main__':unittest.main()
