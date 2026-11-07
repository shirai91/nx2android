"""Do not publish page states after failed or partial protection changes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class DirtyProtectionFailures(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_protection_and_defrag_restore(self):
        source = (Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/nvn/nvn_dirty.c').read_text()
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
typedef struct { uint32_t numer,denom; } mach_timebase_info_data_t;
static void mach_timebase_info(mach_timebase_info_data_t *t) { t->numer=t->denom=1; }
static uint64_t mach_absolute_time(void) {
    struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
#define LOG(...) fprintf(stderr,__VA_ARGS__)
static void fatal(const char *message,...);
#define die fatal
static int (*fault_hook)(void *,int,void *);
static void (*write_hook)(void *,size_t);
static void dm_fault_hook_register(int (*fn)(void *,int,void *)) { fault_hook=fn; }
static void dm_guest_write_prep_register(void (*fn)(void *,size_t)) { write_hook=fn; }
static int mode,armed,attempts,remaps;
static uint8_t *memory;static size_t page;
static int checked_mprotect(void *addr,size_t size,int prot) {
    if(armed && prot==(mode<=2 ? PROT_READ : PROT_READ|PROT_WRITE)) {
        assert(addr==memory && size==(mode<=2 ? 3 : 2)*page);attempts++;
        if(mode==2 || mode==4)assert(!mprotect(addr,page,prot));
        errno=ENOMEM;return -1;
    }
    return mprotect(addr,size,prot);
}
#define mprotect checked_mprotect
DIRTY
#undef mprotect
static void fatal(const char *message,...) {
    assert(mode && attempts==1 && strstr(message,"[RED] dirty-page tracking:"));
    assert(strstr(message,mode<=2 ? "cannot protect guest pages" : "cannot restore writable pages"));
    range_t *r=rg(0);
    for(unsigned i=0;i<3;i++)assert(atomic_load(&r->state[i])==PG_BUSY);
    if(mode<=2) {
        assert(!remaps);
        for(unsigned i=0;i<3;i++)assert(!atomic_load(&r->span[i]));
    } else assert(remaps==1 && atomic_load(&r->nmp)>=DEFRAG_MP);
    _exit(77);
}
static int remap_read_only(void *addr,size_t size) {
    assert(addr==memory && size==3*page);remaps++;
    assert(!mprotect(addr,size,PROT_READ));return 0;
}
static void fault(int sig,siginfo_t *info,void *context) {
    if(sig!=SIGSEGV || !fault_hook(info->si_addr,1,context))_exit(90);
}
int main(int argc,char **argv) {
    assert(argc==2);mode=atoi(argv[1]);page=(size_t)getpagesize();
    memory=mmap(NULL,3*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(memory!=MAP_FAILED);memset(memory,7,3*page);nvn_dirty_add_range(memory,3*page);
    struct sigaction sa={.sa_sigaction=fault,.sa_flags=SA_SIGINFO};
    sigemptyset(&sa.sa_mask);assert(!sigaction(SIGSEGV,&sa,NULL));
    if(mode==1 || mode==2) { armed=1;nvn_dirty_protect(memory,3*page);return 91; }
    nvn_dirty_protect(memory,3*page);
    for(unsigned i=0;i<3;i++)assert(atomic_load(&rg(0)->state[i])==PG_PROT);
    uint64_t stamp=nvn_dirty_now();
    ((volatile uint8_t *)memory)[0]=29;
    assert(nvn_dirty_since(memory,3*page,stamp));
    // Keep the third page protected and the first two writable before defragmentation.
    nvn_dirty_protect(memory,3*page);write_hook(memory,2*page);
    assert(atomic_load(&rg(0)->state[2])==PG_PROT);
    atomic_store(&rg(0)->nmp,DEFRAG_MP);atomic_store(&rg(0)->prep,0);armed=mode!=0;
    assert(nvn_dirty_defrag(remap_read_only)==1);
    assert(!mode && !attempts && remaps==1 && !atomic_load(&rg(0)->nmp));
    assert(atomic_load(&rg(0)->state[0])==PG_NONE && atomic_load(&rg(0)->state[1])==PG_NONE);
    assert(atomic_load(&rg(0)->state[2])==PG_PROT);
    memory[0]=31;memory[page]=37;stamp=nvn_dirty_now();
    ((volatile uint8_t *)memory)[2*page]=41;
    assert(nvn_dirty_since(memory+2*page,page,stamp));
    assert(memory[0]==31 && memory[page]==37 && memory[2*page]==41);
    nvn_dirty_remove_range(memory);assert(!munmap(memory,3*page));
}
'''.replace('DIRTY', source)
        with tempfile.TemporaryDirectory(prefix='nx2a-dirty-protect-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                            '-Wno-misleading-indentation', str(c), '-o', str(exe)], check=True)
            for mode in range(5):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(exe), str(mode)], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 77 if mode else 0, result.stderr)
