"""A renderer must not acknowledge partially published page generations."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class DirtyPublication(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_inflight_and_out_of_order_writers(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'nxnative/dm/gfx/nvn/nvn_dirty.c').read_text()
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        # Scheduler hooks only: the actual atomics, page protection and reader gate remain intact.
        for needle, phase in [('atomic_fetch_add(&g_publishing, 1);', 1),
                              ('uint64_t g = atomic_fetch_add(&g_now, 1) + 1;', 2),
                              ('atomic_store(&r->gen[p], g);', 3)]:
            self.assertEqual(source.count(needle), 1)
            source = source.replace(needle, needle + f' pause_at({phase});')
        texture = (root / 'nxnative/dm/android/gfx_vk_tex.c').read_text()
        gate = next(line.strip() for line in texture.splitlines() if 'if (im->pkg_pending || ((!im->pkg_span' in line)
        gate = gate[:gate.index('{')]
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
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
static void mach_timebase_info(mach_timebase_info_data_t *t) {t->numer=t->denom=1;}
static uint64_t mach_absolute_time(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
#define LOG(...) fprintf(stderr,__VA_ARGS__)
#define die(...) abort()
static void (*prepare)(void *,size_t);static int (*fault_hook)(void *,int,void *);
static void dm_fault_hook_register(int (*fn)(void *,int,void *)) {fault_hook=fn;}
static void dm_guest_write_prep_register(void (*fn)(void *,size_t)) {prepare=fn;}
static pthread_barrier_t paused,resume;static int phase;static _Thread_local int slow_writer;
static void pause_at(int point) {if(slow_writer && point==phase) {pthread_barrier_wait(&paused);pthread_barrier_wait(&resume);}}
TRACKER_BODY
typedef struct {int pkg_pending;const void *pkg_span;uint64_t pkg_stamp,pkg_seen;} image;
typedef struct {uint8_t *mem;struct {size_t size;} L;} texture;
static int should_upload(image *im,const texture *t) {
    uint64_t now=nvn_dirty_now();int upload=0;
    GATE {upload=1;}
    if(!im->pkg_pending)im->pkg_seen=now;
    return upload;
}
static uint8_t *mem;static size_t page;
static void *first_writer(void *unused) {(void)unused;slow_writer=1;prepare(mem,1);mem[0]=29;return NULL;}
static void *later_writer(void *unused) {(void)unused;prepare(mem+page,1);mem[page]=41;return NULL;}
static void fault(int sig,siginfo_t *info,void *context) {if(sig!=SIGSEGV || !fault_hook(info->si_addr,1,context))_exit(90);}
int main(int argc,char **argv) {
    assert(argc==2);phase=atoi(argv[1]);assert(phase>=1 && phase<=3);
    assert(atomic_is_lock_free(&g_publishing));page=(size_t)getpagesize();
    mem=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(mem!=MAP_FAILED);memset(mem,7,2*page);
    nvn_dirty_add_range(mem,2*page);nvn_dirty_protect(mem,page);nvn_dirty_protect(mem+page,page);
    image im={.pkg_span=nvn_dirty_span(mem,1),.pkg_stamp=nvn_dirty_now(),.pkg_seen=nvn_dirty_now()};texture t={.mem=mem,.L={1}};
    assert(im.pkg_stamp==1 && !should_upload(&im,&t));
    pthread_barrier_init(&paused,NULL,2);pthread_barrier_init(&resume,NULL,2);
    pthread_t first,later;assert(!pthread_create(&first,NULL,first_writer,NULL));pthread_barrier_wait(&paused);
    assert(nvn_dirty_now()==0 && mem[0]==7);
    // A texture created/uploaded during publication must also keep a conservative stamp.
    image fresh={.pkg_span=im.pkg_span,.pkg_stamp=nvn_dirty_now(),.pkg_seen=nvn_dirty_now()};assert(!fresh.pkg_stamp);
    assert(!should_upload(&im,&t) && !im.pkg_seen);
    assert(!pthread_create(&later,NULL,later_writer,NULL));assert(!pthread_join(later,NULL));
    assert(mem[page]==41 && atomic_load(&rg(0)->last)>1);
    assert(nvn_dirty_now()==0); // a later completed writer must not hide the earlier unfinished writer
    pthread_barrier_wait(&resume);assert(!pthread_join(first,NULL));
    assert(!atomic_load(&g_publishing) && nvn_dirty_now()==3 && mem[0]==29);
    assert(should_upload(&im,&t) && should_upload(&fresh,&t));
    im.pkg_stamp=im.pkg_seen=nvn_dirty_now();assert(!should_upload(&im,&t));
    // Normal page-fault writes keep the same completed generation behavior and no pending publisher.
    struct sigaction sa={.sa_sigaction=fault,.sa_flags=SA_SIGINFO};sigemptyset(&sa.sa_mask);assert(!sigaction(SIGSEGV,&sa,NULL));
    nvn_dirty_protect(mem,page);uint64_t before=nvn_dirty_now();((volatile uint8_t *)mem)[0]=53;
    assert(nvn_dirty_now()>before && !atomic_load(&g_publishing) && nvn_dirty_since(mem,1,before));
    nvn_dirty_remove_range(mem);assert(!munmap(mem,2*page));
    pthread_barrier_destroy(&paused);pthread_barrier_destroy(&resume);
}
'''.replace('TRACKER_BODY', source).replace('GATE', gate)
        with tempfile.TemporaryDirectory(prefix='nx2a-dirty-publication-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                            '-Wno-misleading-indentation', str(c), '-o', str(exe)], check=True)
            for phase in range(1, 4):
                with self.subTest(phase=phase):
                    subprocess.run([str(exe), str(phase)], check=True, timeout=15)
