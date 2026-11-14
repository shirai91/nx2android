"""Exercise real NVN recording code: control-owned host handles and lifetime."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import os
from host_cc import posix, ubsan


ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'nxnative/dm/gfx/nvn'


def harness():
    source = (NVN / 'hle_nvn.c').read_text(encoding='utf-8')

    def function(marker):
        start = source.index(marker)
        body = source.index('{', start)
        # Match braces outside comments/strings while retaining the original source.
        tokens = re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|[{}]', source[body:])
        depth = 0
        for token in tokens:
            if token[0] == '{':
                depth += 1
            elif token[0] == '}':
                depth -= 1
                if not depth:
                    return source[start:body + token.end()]
        raise AssertionError(marker)

    funcs = [
        'static uint8_t *align8(', 'static void put_hdr(',
        'static void cmd_new_segment(', 'static uint8_t *hostseg_take(', 'static void hostseg_release(',
        'NVN(nvnCommandBufferInitialize,', 'NVN(nvnCommandBufferFinalize,',
        'NVN(nvnCommandBufferAddCommandMemory,', 'NVN(nvnCommandBufferAddControlMemory,',
        'static void cmd_host_anchor(', 'static uint8_t *cmd_alloc_keep(', 'static uint8_t *cmd_alloc(',
        'NVN(nvnCommandBufferBeginRecording,', 'NVN(nvnCommandBufferEndRecording,',
        'NVN(nvnCommandBufferGetCommandMemoryUsed,', 'NVN(nvnCommandBufferGetControlMemoryUsed,',
        'NVN(nvnCommandBufferGetControlMemorySize,', 'NVN(nvnCommandBufferGetControlMemoryFree,',
        'NVN(nvnCommandBufferGetCommandMemoryFree,', 'NVN(nvnCommandBufferGetCommandMemorySize,',
    ]
    pre = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include "nvn_obj.h"
#define NVN(name, ret, ...) ret h_##name(__VA_ARGS__)
#define CB(c) ((cmd_t *)(c))
#define LOG(...) ((void)0)
#define OP_RESERVE 16
#define HOSTSEG_SZ (256u << 10)
#define HOSTSEG_MIN (4u << 10)
#define HOSTSEG_CAP (1u << 30)
static uint64_t dm_nvn_frame_count(void) { return 1000000; }
static uint64_t now_ns(void) { return 0; }
static void die(const char *fmt, ...) { (void)fmt; abort(); }
static int cs_on(void) { return 0; }
static void cs_op(uint32_t op, uint32_t size) { (void)op; (void)size; }
static struct { uint64_t waste, hostseg, grants, grant_bytes, grant_min, grant_max, cbs, bytes, recs; } g_cs;
static pthread_mutex_t g_cs_mu = PTHREAD_MUTEX_INITIALIZER;
'''
    globals_start = source.index('typedef struct { uint8_t *mem; uint64_t frame; int busy;')
    globals_end = source.index('// Take a segment of sz bytes:', globals_start)
    diagnostic = source.find('// CMDSTAT also measures recording-side', globals_start, globals_end)
    if diagnostic >= 0:
        globals_end = diagnostic
    return pre + source[globals_start:globals_end] + '\n'.join(function(f) for f in funcs) + r'''
static unsigned busy(void) { unsigned n=0; for (int i=0;i<g_nhostseg;i++) n+=g_hostseg[i].busy; return n; }
static unsigned unknown(void) { unsigned n=0; for (int i=0;i<g_nhostseg;i++) n+=g_hostseg[i].busy && !g_hostseg[i].org_hi; return n; }
static uint8_t *jump(uint8_t *p) { assert(((op_hdr *)p)->op==OP_JUMP); uint8_t *q; memcpy(&q,p+8,8); return q; }
static void start(NVNcommandBuffer *cb, void *ctl, size_t n) {
    h_nvnCommandBufferInitialize(cb,NULL); h_nvnCommandBufferAddControlMemory(cb,ctl,n);
    h_nvnCommandBufferAddCommandMemory(cb,NULL,0,512); h_nvnCommandBufferBeginRecording(cb);
}
static void control_callback(NVNcommandBuffer *cb, NVNcommandBufferMemoryEvent event, size_t n, void *data) {
    assert(event==NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_CONTROL_MEMORY); assert(n==16);
    h_nvnCommandBufferAddControlMemory(cb,data,512);
}
int main(void) {
    NVNcommandBuffer cb={0}, other={0}; _Alignas(8) uint8_t ctl[1024]={0}, replacement[1024]={0}, guest[256]={0};
    memset(ctl,0xa7,sizeof ctl);
    start(&cb,ctl,512); assert(!g_nhostseg); uint8_t *p=cmd_alloc(CB(&cb),40); memset(p+8,0x42,32);
    NVNcommandHandle small=h_nvnCommandBufferEndRecording(&cb);
    assert(small==(uintptr_t)p && small!=(uintptr_t)ctl && g_nhostseg==1 && unknown()==0);
    assert(h_nvnCommandBufferGetCommandMemoryUsed(&cb)==48);
    assert(h_nvnCommandBufferGetCommandMemorySize(&cb)==0);
    assert(h_nvnCommandBufferGetControlMemoryUsed(&cb)==16);
    assert(h_nvnCommandBufferGetControlMemoryFree(&cb)==496);
    for (int i=0;i<512;i++) assert(ctl[i]==0xa7); // private replay records never overwrite guest control data
    // Finalizing/reinitializing the recorder must not free a cached handle.
    h_nvnCommandBufferFinalize(&cb); h_nvnCommandBufferInitialize(&cb,NULL);
    assert(((uint8_t *)(uintptr_t)small)[8]==0x42);
    // Even a first record larger than the control block gets an independent host chain.
    start(&cb,ctl,64); p=cmd_alloc(CB(&cb),128); memset(p+8,0x51,120);
    NVNcommandHandle first=h_nvnCommandBufferEndRecording(&cb); uint8_t *first_host=(uint8_t *)(uintptr_t)first;
    assert(first!=(uintptr_t)ctl && first_host==p && busy()==1 && unknown()==0);
    assert(h_nvnCommandBufferGetControlMemoryUsed(&cb)==16);
    // Another recording in the same control block gets a distinct anchor and segment.
    h_nvnCommandBufferBeginRecording(&cb); p=cmd_alloc(CB(&cb),128); memset(p+8,0x62,120);
    NVNcommandHandle second=h_nvnCommandBufferEndRecording(&cb); uint8_t *second_host=(uint8_t *)(uintptr_t)second;
    assert(second!=(uintptr_t)(ctl+16) && second_host!=first_host && busy()==2);
    assert(h_nvnCommandBufferGetControlMemoryUsed(&cb)==32);
    // Adjacent ranges and unrelated grants cannot invalidate either cached stream.
    h_nvnCommandBufferAddControlMemory(&other,ctl+32,16); assert(busy()==2);
    h_nvnCommandBufferAddControlMemory(&other,replacement,64); assert(busy()==2);
    assert(first_host[8]==0x51 && second_host[8]==0x62);
    memset(ctl,0xe4,64); // guest control-side edits cannot alter a cached CPU replay stream
    assert(first_host[8]==0x51 && second_host[8]==0x62);
    h_nvnCommandBufferFinalize(&cb); h_nvnCommandBufferInitialize(&cb,NULL); assert(busy()==2);
    // Reusing exactly the first anchor reclaims only its chain, never the adjacent live handle.
    h_nvnCommandBufferAddControlMemory(&other,ctl,16); assert(busy()==1 && second_host[8]==0x62);
    start(&cb,replacement,64); cmd_alloc(CB(&cb),128); h_nvnCommandBufferEndRecording(&cb);
    assert(g_nhostseg==2 && busy()==2);  // reused released storage
    // Exhausted identity storage requests a new control grant without changing command accounting.
    start(&cb,ctl+128,16); cmd_alloc(CB(&cb),32); h_nvnCommandBufferEndRecording(&cb);
    CB(&cb)->cb=control_callback; CB(&cb)->cb_data=replacement+256;
    h_nvnCommandBufferBeginRecording(&cb); p=cmd_alloc(CB(&cb),128);
    NVNcommandHandle callback_handle=h_nvnCommandBufferEndRecording(&cb);
    assert(callback_handle==(uintptr_t)p && callback_handle!=(uintptr_t)(replacement+256));
    assert(h_nvnCommandBufferGetControlMemoryUsed(&cb)==16);
    // Existing guest command-memory accounting and overflow remain command-backed.
    pool_t pool={.mem=guest,.size=sizeof guest};
    h_nvnCommandBufferInitialize(&cb,NULL); h_nvnCommandBufferAddControlMemory(&cb,ctl+512,512);
    h_nvnCommandBufferAddCommandMemory(&cb,(NVNmemoryPool *)&pool,0,64); h_nvnCommandBufferBeginRecording(&cb);
    cmd_alloc(CB(&cb),40); cmd_alloc(CB(&cb),40); h_nvnCommandBufferEndRecording(&cb);
    assert(h_nvnCommandBufferGetCommandMemoryUsed(&cb)==104);
    assert(h_nvnCommandBufferGetControlMemoryUsed(&cb)==0);
    // Repeated same-range recordings stay bounded; frame age alone never reclaims.
    int capacity=g_nhostseg;
    for (int i=0;i<10000;i++) { start(&cb,ctl+768,64); cmd_alloc(CB(&cb),128); h_nvnCommandBufferEndRecording(&cb); }
    assert(g_nhostseg<=capacity+1 && unknown()==0 && second_host[8]==0x62);
    // An exhausted control grant must not eagerly lend a segment that BeginRecording would discard.
    _Alignas(8) uint8_t tiny[16]={0}; int before=g_nhostseg;
    start(&cb,tiny,sizeof tiny); h_nvnCommandBufferEndRecording(&cb);
    assert(h_nvnCommandBufferGetControlMemoryUsed(&cb)==16 && g_nhostseg<=before+1);
    before=g_nhostseg;
    h_nvnCommandBufferAddCommandMemory(&cb,NULL,0,512); assert(g_nhostseg==before && !CB(&cb)->cur);
    puts("control-backed command lifetime checks passed"); return 0;
}
'''


class ControlMemory(unittest.TestCase):
    @unittest.skipUnless(posix(), 'a POSIX host C compiler is required')
    def test_cached_handles_and_reclamation(self):
        with tempfile.TemporaryDirectory(prefix='nx2a-control-memory-') as tmp:
            c, exe = Path(tmp)/'check.c', Path(tmp)/'check'
            c.write_text(harness(), encoding='utf-8')
            subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Wno-unused-variable',
                            '-Wno-unused-function','-I',str(NVN),'-I',str(NVN/'include'),str(c),'-pthread','-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)




ACCOUNTING = r'''

#include <stdatomic.h>
typedef struct { NVNcommandBuffer *cb; pool_t *pool; atomic_int done; atomic_int draws; } job_t;
static void *writer(void *arg) {
    job_t *job = arg;
    for (int round = 0; round < 1000; round++) {
        h_nvnCommandBufferAddCommandMemory(job->cb, (NVNmemoryPool *)job->pool, 0, 8192);
        h_nvnCommandBufferBeginRecording(job->cb);
        while (h_nvnCommandBufferGetCommandMemoryFree(job->cb) > 256) {
            cmd_alloc(CB(job->cb), 40);
            atomic_fetch_add(&job->draws, 1);
        }
        h_nvnCommandBufferEndRecording(job->cb);
    }
    atomic_store(&job->done, 1); return NULL;
}
int main(void) {
    assert(sizeof(cmd_t) <= sizeof(NVNcommandBuffer));
    assert(offsetof(cmd_t,nvn_flag10) == 0x10);
    union { uint64_t align; unsigned char bytes[sizeof(NVNcommandBuffer)+32]; } guarded;
    memset(&guarded, 0xa5, sizeof guarded);
    NVNcommandBuffer *cb = (NVNcommandBuffer *)guarded.bytes;
    _Alignas(8) uint8_t guest[8192]={0}, ctl[256]={0};
    pool_t pool={.mem=guest,.size=sizeof guest};
    h_nvnCommandBufferInitialize(cb,NULL);
    assert(h_nvnCommandBufferGetCommandMemorySize(cb)==0);
    h_nvnCommandBufferAddControlMemory(cb,ctl,sizeof ctl);
    h_nvnCommandBufferAddCommandMemory(cb,NULL,0,512);
    assert(h_nvnCommandBufferGetCommandMemorySize(cb)==0);
    h_nvnCommandBufferBeginRecording(cb); cmd_alloc(CB(cb),40);
    NVNcommandHandle retained=h_nvnCommandBufferEndRecording(cb);
    assert(retained && h_nvnCommandBufferGetControlMemoryUsed(cb)==16);
    assert(h_nvnCommandBufferGetCommandMemorySize(cb)==0);
    h_nvnCommandBufferAddCommandMemory(cb,(NVNmemoryPool *)&pool,0,64);
    assert(h_nvnCommandBufferGetCommandMemorySize(cb)==64 && !CB(cb)->host_control);
    h_nvnCommandBufferBeginRecording(cb);
    for (int i=0;i<3;i++) cmd_alloc(CB(cb),40);
    h_nvnCommandBufferEndRecording(cb);
    assert(h_nvnCommandBufferGetCommandMemoryUsed(cb)>64);
    assert(h_nvnCommandBufferGetCommandMemorySize(cb)==64);
    // The control-backed early return and subsequent recording leave the last real grant intact.
    h_nvnCommandBufferAddCommandMemory(cb,NULL,0,512);
    assert(CB(cb)->host_control && h_nvnCommandBufferGetCommandMemorySize(cb)==64);
    h_nvnCommandBufferBeginRecording(cb); cmd_alloc(CB(cb),40); h_nvnCommandBufferEndRecording(cb);
    assert(h_nvnCommandBufferGetCommandMemorySize(cb)==64);
    assert(h_nvnCommandBufferGetControlMemoryUsed(cb)==32);
    // A small real grant must publish its size before the host-segment fallback returns.
    h_nvnCommandBufferAddCommandMemory(cb,(NVNmemoryPool *)&pool,0,32);
    assert(h_nvnCommandBufferGetCommandMemorySize(cb)==32);
    h_nvnCommandBufferAddCommandMemory(cb,(NVNmemoryPool *)&pool,16,sizeof guest-16);
    assert(h_nvnCommandBufferGetCommandMemorySize(cb)==sizeof guest-16);
    h_nvnCommandBufferAddCommandMemory(cb,(NVNmemoryPool *)&pool,0,sizeof guest);
    job_t job={.cb=cb,.pool=&pool}; pthread_t thread;
    assert(!pthread_create(&thread,NULL,writer,&job));
    size_t reads=0;
    do { assert(h_nvnCommandBufferGetCommandMemorySize(cb)==sizeof guest); reads++; }
    while (!atomic_load(&job.done));
    assert(!pthread_join(thread,NULL));
    assert(reads && atomic_load(&job.draws)>20000);
    for (size_t i=sizeof(NVNcommandBuffer);i<sizeof guarded.bytes;i++) assert(guarded.bytes[i]==0xa5);
    printf("merged command memory: PASS (%d recorded commands, %zu concurrent size reads)\n",atomic_load(&job.draws),reads);
    return 0;
}

'''


CAPACITY = r'''
#include <sys/resource.h>
static unsigned busy_segments(void) {
    unsigned n=0; for(int i=0;i<g_nhostseg;i++) n+=g_hostseg[i].busy; return n;
}
static void record_handles(NVNcommandBuffer *cb,uint8_t *control,NVNcommandHandle *handles,int n,size_t grant) {
    for(int i=0;i<n;i++) {
        h_nvnCommandBufferInitialize(cb,NULL);
        h_nvnCommandBufferAddControlMemory(cb,control+16*i,16);
        h_nvnCommandBufferAddCommandMemory(cb,NULL,0,grant);
        h_nvnCommandBufferBeginRecording(cb);
        uint8_t *p=cmd_alloc(CB(cb),40); memset(p+8,(unsigned char)i,32);
        handles[i]=h_nvnCommandBufferEndRecording(cb);
        assert(h_nvnCommandBufferGetCommandMemorySize(cb)==0);
        assert(h_nvnCommandBufferGetCommandMemoryUsed(cb)==48);
        assert(h_nvnCommandBufferGetCommandMemoryFree(cb)==HOSTSEG_MIN-48);
        assert(h_nvnCommandBufferGetControlMemorySize(cb)==16);
        assert(h_nvnCommandBufferGetControlMemoryUsed(cb)==16);
        assert(h_nvnCommandBufferGetControlMemoryFree(cb)==0);
    }
}
int main(int argc,char **argv) {
    struct rlimit limit={0,0}; setrlimit(RLIMIT_CORE,&limit);
    assert(argc==4); size_t grant=(size_t)strtoul(argv[2],NULL,10); int n=atoi(argv[3]);
    NVNcommandBuffer cb={0}; uint8_t *control=calloc((size_t)n,16), *unrelated=calloc(1,32);
    NVNcommandHandle *handles=calloc((size_t)n,sizeof *handles); assert(control&&unrelated&&handles);
    if(!strcmp(argv[1],"grow")) {
        h_nvnCommandBufferInitialize(&cb,NULL); h_nvnCommandBufferAddControlMemory(&cb,control,16);
        h_nvnCommandBufferAddCommandMemory(&cb,NULL,0,0); h_nvnCommandBufferBeginRecording(&cb);
        cmd_alloc(CB(&cb),5000); h_nvnCommandBufferEndRecording(&cb);
        assert(g_nhostseg==1 && g_hostseg_bytes==8192);
        assert(h_nvnCommandBufferGetControlMemoryUsed(&cb)==16);
        puts("large first record grows from 4 KiB to 8 KiB: PASS"); return 0;
    }
    record_handles(&cb,control,handles,n,grant);
    assert(g_nhostseg==n && g_hostseg_bytes==(uint64_t)n*HOSTSEG_MIN);
    for(int i=0;i<n;i++) assert(((uint8_t *)(uintptr_t)handles[i])[8]==(unsigned char)i);
    assert(busy_segments()==(unsigned)n);
    if(!strcmp(argv[1],"reuse")) {
        uint64_t capacity=g_hostseg_bytes;
        h_nvnCommandBufferAddControlMemory(&cb,unrelated,32); assert(busy_segments()==(unsigned)n);
        h_nvnCommandBufferAddControlMemory(&cb,control,(size_t)n*16); assert(!busy_segments());
        record_handles(&cb,control,handles,n,grant);
        assert(g_nhostseg==n && g_hostseg_bytes==capacity);
        for(int i=0;i<n;i++) assert(((uint8_t *)(uintptr_t)handles[i])[8]==(unsigned char)i);
    }
    printf("retained handles: %d, request: %zu, capacity: %llu, per handle: %u: PASS\n",
           n,grant,(unsigned long long)g_hostseg_bytes,HOSTSEG_MIN);
    free(control);free(unrelated);free(handles);return 0;
}
'''


@unittest.skipUnless(posix(), 'a POSIX host C compiler is required')
class ControlMemoryCapacity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='nx2a_control_capacity_')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.prefix = harness().split('static unsigned busy(void)')[0]
        cls.sanitize = os.getenv('NX2A_TEST_SANITIZE') == '1' and ubsan()
        cls.capacity = cls.compile(cls.prefix+CAPACITY, 'capacity')
        cls.accounting = cls.compile(cls.prefix+ACCOUNTING, 'accounting')

    @classmethod
    def compile(cls, code, name):
        source, binary = cls.root/(name+'.c'), cls.root/name
        source.write_text(code, encoding='utf-8')
        flags = ['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'] if cls.sanitize else []
        subprocess.run(['cc','-std=gnu11','-O1','-g','-pthread',*flags,'-I'+str(NVN),
                        '-I'+str(NVN/'include'),str(source),'-o',str(binary)],check=True,capture_output=True)
        return binary

    def run_capacity(self, mode, grant, count):
        result = subprocess.run([str(self.capacity),mode,str(grant),str(count)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS',result.stdout)
        print(result.stdout.strip())

    def test_zero_small_and_odyssey_requests_start_at_four_kib(self):
        for grant in (0,1,8,63,64,512):
            with self.subTest(grant=grant): self.run_capacity('capacity',grant,128)

    def test_many_live_zero_request_handles_have_bounded_capacity(self):
        self.run_capacity('capacity',0,8192)

    def test_only_overlapping_control_regrants_allow_reuse(self):
        self.run_capacity('reuse',0,128)

    def test_large_first_record_grows_as_needed(self):
        self.run_capacity('grow',0,1)

    def test_mainline_size_queries_with_control_owned_recordings(self):
        result = subprocess.run([str(self.accounting)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('merged command memory: PASS',result.stdout)
        print(result.stdout.strip())

    def test_regression_rejects_the_old_zero_request_initial_size(self):
        fixed = 'uint32_t sz = HOSTSEG_MIN; while (sz < n && sz < HOSTSEG_SZ) sz <<= 1;'
        old = 'uint32_t sz = n >= 64 ? HOSTSEG_MIN : HOSTSEG_SZ; while (sz < n && sz < HOSTSEG_SZ) sz <<= 1;'
        self.assertEqual(self.prefix.count(fixed),1)
        legacy = self.compile(self.prefix.replace(fixed,old,1)+CAPACITY,'legacy_capacity')
        result = subprocess.run([str(legacy),'capacity','0','128'],capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0,'Legacy 256 KiB allocations must fail the capacity regression')


if __name__ == '__main__':
    unittest.main()
