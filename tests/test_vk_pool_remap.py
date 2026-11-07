"""Reject unsafe continuation after failed or partial fixed pool remapping."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PoolRemap(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_failures_before_and_after_guest_mapping_changes(self):
        source = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = source.index('int vk_pool_remap_ro(')
        function = source[start:source.index('\n}\n', start) + 3]
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct { uint64_t base,end,lo,hi; int zero_copy; void *shadow,*ahb; } vkpool_t;
static vkpool_t g_pool[2]; static int g_npool;
static pthread_rwlock_t g_pool_mu=PTHREAD_RWLOCK_INITIALIZER;
static unsigned char memory[128];
static int fail_at,kind,calls,mappings,closed,stops;
static jmp_buf stop;
static void fake_die(const char *fmt,...) {
    char text[300]; va_list a; va_start(a,fmt); vsnprintf(text,sizeof text,fmt,a); va_end(a);
    assert(strstr(text,"[RED] [vk]"));
    if(kind==5)assert(strstr(text,"cannot remap") && strstr(text,"errno=13"));
    else assert(strstr(text,"incomplete read-only pool remap"));
    assert(!pthread_rwlock_trywrlock(&g_pool_mu)); pthread_rwlock_unlock(&g_pool_mu);
    stops++; longjmp(stop,1);
}
#define die fake_die
static int sockets(int domain,int type,int protocol,int *sv) {
    assert(domain==AF_UNIX && type==(SOCK_SEQPACKET|SOCK_CLOEXEC) && !protocol);
    calls++; if(calls==fail_at && kind==1)return -1;
    sv[0]=10;sv[1]=11;return 0;
}
#define socketpair sockets
static int AHardwareBuffer_sendHandleToUnixSocket(void *ahb,int fd) {
    assert(ahb && fd==10);return calls==fail_at && kind==2 ? -1 : 0;
}
static int recv_fds(int fd,int *fds,int cap) {
    assert(fd==11 && cap==16);if(calls==fail_at && kind==3)return -1;
    fds[0]=12;fds[1]=13;return 2;
}
static int pool_data_fd(const int *fds,int nfd,uint64_t size) {
    assert(fds[0]==12 && nfd==2 && size==64);return calls==fail_at && kind==4 ? -1 : 1;
}
static int close_fd(int fd) { assert(fd>=10 && fd<=13);closed++;errno=EINVAL;return 0; }
#define close close_fd
static void *map(void *addr,size_t size,int prot,int flags,int fd,off_t off) {
    assert(addr==memory+(calls-1)*64 && size==64 && prot==PROT_READ);
    assert(flags==(MAP_SHARED|MAP_FIXED) && fd==13 && !off);mappings++;
    if(calls==fail_at && kind==5) { memset(addr,0,64);errno=EACCES;return MAP_FAILED; }
    return addr;
}
#define mmap map
FUNCTION
static void reset(void) {
    calls=mappings=closed=stops=0;g_npool=2;memset(memory,23,sizeof memory);
    for(int i=0;i<2;i++)g_pool[i]=(vkpool_t){.base=(uintptr_t)memory,.end=(uintptr_t)memory+128,
        .lo=(uintptr_t)memory+i*64,.hi=(uintptr_t)memory+(i+1)*64,.zero_copy=1,.ahb=memory};
}
int main(void) {
    for(kind=1;kind<=5;kind++)for(fail_at=1;fail_at<=2;fail_at++) {
        reset();
        if(!setjmp(stop)) {
            assert(vk_pool_remap_ro(memory,128)==-1);
            assert(fail_at==1 && kind!=5);
        }
        assert(stops==(fail_at==2 || kind==5));
        assert(calls==fail_at && mappings==fail_at-1+(kind==5));
        assert(closed==4*(fail_at-1)+(kind==1 ? 0 : kind<=3 ? 2 : 4));
        assert(!pthread_rwlock_trywrlock(&g_pool_mu));pthread_rwlock_unlock(&g_pool_mu);
        if(kind!=5)for(unsigned i=0;i<sizeof memory;i++)assert(memory[i]==23);
        assert(g_pool[0].zero_copy && g_pool[1].zero_copy);
    }
    kind=fail_at=0;reset();assert(!vk_pool_remap_ro(memory,128) && mappings==2 && closed==8);
    reset();g_npool=1;
    if(!setjmp(stop)) { vk_pool_remap_ro(memory,128);assert(!"partial coverage continued"); }
    assert(stops==1 && mappings==1 && closed==4);
    reset();g_npool=0;assert(vk_pool_remap_ro(memory,128)==-1 && !calls && !stops);
    reset();for(int i=0;i<2;i++) { g_pool[i].zero_copy=0;g_pool[i].shadow=memory; }
    assert(!vk_pool_remap_ro(memory,128) && !calls && !stops);
    // Accounting for copied bytes alone must not make a pre-map failure fatal.
    reset();g_pool[0].zero_copy=0;g_pool[0].shadow=memory;kind=1;fail_at=1;
    assert(vk_pool_remap_ro(memory,128)==-1 && !mappings && !stops);
}
'''.replace('FUNCTION', function)
        with tempfile.TemporaryDirectory(prefix='nx2a-pool-remap-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)
