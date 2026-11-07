"""A failed zero-copy pool restore must stop rather than lose guest bytes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PoolRestore(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_snapshot_remap_failures_and_success(self):
        source = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = source.index('static void pool_copy_back(vkpool_t *p) {')
        function = source[start:source.index('\n}\n', start) + 3]
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
typedef struct { uint64_t lo,hi; } vkpool_t;
static uint8_t memory[64],original[64],snapshot[64];
static int failure,allocations,mappings,frees; static jmp_buf stop;
static void fake_die(const char *fmt,...) {
    char line[300]; va_list a; va_start(a,fmt); vsnprintf(line,sizeof line,fmt,a); va_end(a);
    assert(strstr(line,"[RED] [vk]"));
    if(failure==1)assert(strstr(line,"cannot preserve") && !mappings && !frees);
    else assert(strstr(line,"cannot restore") && strstr(line,"errno=12") && mappings==1 && frees==1);
    longjmp(stop,1);
}
#define die fake_die
static void *fake_malloc(size_t n) {
    assert(n==64 && !mappings); allocations++; return failure==1 ? NULL : snapshot;
}
static void fake_free(void *p) { assert(p==snapshot && !frees); frees++; errno=EINVAL; }
static void *fake_mmap(void *addr,size_t n,int prot,int flags,int fd,off_t off) {
    assert(addr==memory && n==64 && prot==(PROT_READ|PROT_WRITE));
    assert(flags==(MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED) && fd==-1 && !off && !frees);
    assert(!memcmp(snapshot,original,64)); mappings++;
    if(failure==2) { errno=ENOMEM; return MAP_FAILED; }
    memset(memory,0,64); return memory;
}
#define malloc fake_malloc
#define free fake_free
#define mmap fake_mmap
FUNCTION
int main(void) {
    for(unsigned i=0;i<64;i++)original[i]=(uint8_t)(i*37+11);
    vkpool_t p={.lo=(uintptr_t)memory,.hi=(uintptr_t)memory+64};
    for(failure=1;failure<=2;failure++) {
        allocations=mappings=frees=0; memcpy(memory,original,64);
        if(!setjmp(stop)) { pool_copy_back(&p); assert(!"failed restore continued"); }
        assert(allocations==1 && !memcmp(memory,original,64));
        assert(mappings==(failure==2) && frees==(failure==2));
    }
    failure=allocations=mappings=frees=0; memcpy(memory,original,64);
    pool_copy_back(&p);
    assert(allocations==1 && mappings==1 && frees==1 && !memcmp(memory,original,64));
}
'''.replace('FUNCTION', function)
        with tempfile.TemporaryDirectory(prefix='nx2a-pool-restore-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)
