"""Optional submit serialization waits for completion without enabling copies."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ZeroCopySerial(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_wait_policy_order_and_failed_wait(self):
        source = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        begin = source.index('void mtl_sub_begin(void) {')
        start = source.index('    static int zc_serial = -1;', begin)
        policy = source[start:source.index('    if (g_tsq)', start)]
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static int waits,copies,messages,reports,result=1;
static uint64_t g_fence=7;
#define VKLOG(s) do { assert(strstr(s,"submit serialization enabled")); messages++; } while(0)
#define die(s) do { assert(strstr(s,"previous submission did not complete")); assert(!copies && !reports); _exit(91); } while(0)
static int mem_diag_on(void) { return 1; }
static uint64_t mem_diag_ns(void) { return 1; }
static void mem_diag_note(unsigned phase,uint64_t t,uint64_t bytes) { assert(!phase && t==1 && !bytes && waits); reports++; }
static int mtl_wait(uint64_t fence,uint64_t timeout) { assert(fence==7 && timeout==UINT64_MAX); waits++; return result; }
static void vk_pools_sync(void *cb) { assert(!cb && waits && reports); copies++; }
static void begin(int shadow) {
POLICY
}
int main(int argc,char **argv) {
    assert(argc==2 && !setenv("DM_VK_ZC_SERIAL",argv[1],1)); int enabled=atoi(argv[1]);
    begin(0); assert(waits==enabled && !copies && messages==enabled && reports==enabled);
    begin(1); assert(waits==enabled+1 && copies==1 && reports==enabled+1); // no duplicate wait for copied pools
    assert(!setenv("DM_VK_ZC_SERIAL",enabled ? "0" : "1",1));
    begin(0); assert(waits==2*enabled+1 && copies==1 && messages==enabled);
    pid_t child=fork(); assert(child>=0);
    if(!child) { copies=reports=0;result=0;begin(1);_exit(3); }
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==91);
    if(enabled) {
        child=fork();assert(child>=0);
        if(!child) {copies=reports=0;result=0;begin(0);_exit(3);}
        assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==91);
    }
}
'''.replace('POLICY', policy)
        with tempfile.TemporaryDirectory(prefix='nx2a-zc-serial-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            for enabled in ('0', '1'):
                subprocess.run([str(exe), enabled], check=True, timeout=15)
