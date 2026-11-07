"""Select copied pools by guarded identity or serial-independent size/flags."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


class SelectivePool(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_creation_guard_and_existing_backings(self):
        src = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = src.index('static int pool_copy_selected(')
        code = src[start:src.index('void mtl_pool_del(', start)]
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAXPOOL 100
#define ZC_CHUNK 0x10000000ull
#define VK_ERROR_FEATURE_NOT_PRESENT -8
#define VK_ERROR_DEVICE_LOST -4
#define VK_SUCCESS 0
#define VKCHK(r) assert(!(r))
typedef int VkResult;
typedef struct {uint64_t lo,hi,base,end,serial;int flags,zero_copy;void *shadow;} vkpool_t;
static vkpool_t g_pool[MAXPOOL];static int g_npool;static uint32_t g_pool_gen;
static pthread_rwlock_t g_pool_mu=PTHREAD_RWLOCK_INITIALIZER;
static void *vkFlushMappedMemoryRanges=(void *)1,*vkInvalidateMappedMemoryRanges=(void *)1;
static int imports,copies,cached_copies,init_calls,selected_messages,mismatch_messages,invalid_messages;
static void log_message(const char *fmt,...) {
    char line[512];va_list a;va_start(a,fmt);vsnprintf(line,sizeof line,fmt,a);va_end(a);
    selected_messages+=strstr(line,"selected for copied backing")!=NULL;
    mismatch_messages+=strstr(line,"identity mismatch")!=NULL;
    invalid_messages+=strstr(line,"selection disabled")!=NULL;
}
#define VKLOG log_message
#define die(...) abort()
static void vk_init(void) {init_calls++;}
static int zc_over(uint64_t n) {(void)n;return 0;}
static void pool_ord_insert(int k) {(void)k;}
static int mem_diag_on(void) {return 0;}
static int pool_zero_copy(vkpool_t *p,uint8_t *mem,uint64_t size) {
    assert(p->lo==(uintptr_t)mem && p->hi-p->lo==size && (p->flags==0x22 || p->flags==0x24));
    imports++;p->zero_copy=1;return 0;
}
static VkResult pool_copied_alloc(vkpool_t *p,uint64_t size,int cached) {
    assert(p->flags==0x22 && size==0x1000000 && !p->zero_copy);
    copies++;cached_copies+=cached;p->shadow=(void *)1;return VK_SUCCESS;
}
static void pool_add_one(const void *,uint64_t,uint64_t,uint64_t,int);
CODE
int main(int argc,char **argv) {
    assert(argc==4);int expected=atoi(argv[1]),check_invalid=atoi(argv[2]),mixed=atoi(argv[3]);
    for(unsigned i=0;i<94;i++) {
        int eligible=i==2 || i==33 || i==75 || i==91;
        vk_pool_add_flags((void *)(uintptr_t)(0x4000000ull*(i+1)),mixed && i==9 ? 0x2000000 : 0x1000000,
            !mixed || eligible || i==9 ? 0x22 : 0x24);
    }
    assert(g_npool==94 && init_calls==94 && g_pool_gen==94);
    assert(copies==expected && imports==94-expected && cached_copies==expected);
    for(int i=0;i<94;i++) {
        assert(g_pool[i].serial==(uint64_t)i+1);
        int selected=mixed ? (i==2 || i==33 || i==75 || i==91) : (expected && i==93);
        assert(g_pool[i].zero_copy==!selected);
    }
    assert(selected_messages==expected && invalid_messages==check_invalid);
    if(expected && !mixed) {
        vkpool_t p=g_pool[93];p.lo+=4096;assert(!pool_copy_selected(&p)); // no partial chunk selection
        p=g_pool[93];p.flags=0x24;assert(!pool_copy_selected(&p));
        p=g_pool[93];p.end+=4096;assert(!pool_copy_selected(&p));
        // Restart-only: changing the env cannot redirect a selection during this run.
        setenv("DM_VK_COPY_POOL","3:1000000:22",1);assert(!pool_copy_selected(&g_pool[2]));
    }
}
'''.replace('CODE', code)
        with tempfile.TemporaryDirectory(prefix='nx2a-select-pool-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            cases = [('', 0, 0), ('94:1000000:22', 1, 0), ('94:1000000:24', 0, 0),
                     ('94:2000000:22', 0, 0), ('95:1000000:22', 0, 0)]
            cases += [(v, 0, 1) for v in ('94', '-1:1000000:22', '94:1000000:22x',
                      '0:1000000:22', '94:0:22', '94:1000000:ffffffff',
                      '18446744073709551616:1000000:22', '94::22', '94:1000000:', '*:0:22', '*:1000000:', '**:1000000:22')]
            for value, expected, invalid in cases:
                with self.subTest(selector=value):
                    env = dict(os.environ, DM_VK_COPY_POOL=value, DM_VK_POOL_CACHED='1')
                    env.pop('DM_VK_NO_ZEROCOPY', None)
                    subprocess.run([str(exe), str(expected), str(invalid), '0'], env=env, check=True, timeout=15)

            # Reordered candidate: serial 94 now has 0x24; the dynamic class still matches other serials.
            env = dict(os.environ, DM_VK_COPY_POOL='*:1000000:22', DM_VK_POOL_CACHED='1')
            env.pop('DM_VK_NO_ZEROCOPY', None)
            subprocess.run([str(exe), '4', '0', '1'], env=env, check=True, timeout=15)

    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_non_vulkan_host_keeps_flag_publication_order(self):
        src = (Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/nvn/hle_nvn.c').read_text()
        start = src.index('__attribute__((weak)) void vk_pool_add_flags(')
        function = src[start:src.index('\n}', start) + 2]
        harness = r'''
#include <assert.h>
#include <stdint.h>
static int stage;
static void mtl_pool_add(const void *p,uint64_t n) {assert(p==(void *)0x1234 && n==0x100000001ull && !stage);stage=1;}
static void vk_pool_set_flags(const void *p,int f) {assert(p==(void *)0x1234 && f==0x22 && stage==1);stage=2;}
FUNCTION
int main(void) {vk_pool_add_flags((void *)0x1234,0x100000001ull,0x22);assert(stage==2);}
'''.replace('FUNCTION', function)
        with tempfile.TemporaryDirectory(prefix='nx2a-pool-flags-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)
