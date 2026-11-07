"""Bracket initial CPU writes before a new zero-copy buffer reaches the GPU."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ZeroCopyInit(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_locked_upload_and_failures(self):
        source = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = source.index('static int pool_init_copy(')
        helper = source[start:source.index('\n}\n', start) + 3]
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int unused; } AHardwareBuffer;
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN 48
static unsigned char locked_bytes[4];
static const unsigned char expected[]={1,2,3,4};
static int failure, locks, unlocks, stage, code, messages;
#define VKLOG(s) do { assert(strstr(s,"initial upload lock enabled")); messages++; } while(0)
static void mem_diag_zc(unsigned s,const char *name,uint64_t size,const char *kind,int error) {
    assert(name && size==4 && !strcmp(kind,"return")); stage=(int)s; code=error;
}
static int AHardwareBuffer_lock(AHardwareBuffer *a,uint64_t usage,int fence,const void *rect,void **out) {
    assert(a && usage==AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN && fence==-1 && !rect); locks++;
    if(failure==1)return -ENOMEM;
    *out=failure==3 ? NULL : locked_bytes; return 0;
}
static int AHardwareBuffer_unlock(AHardwareBuffer *a,int32_t *fence) {
    assert(a && !fence); unlocks++;
    if(failure!=3)assert(!memcmp(locked_bytes,expected,4)); // CPU copy completed before unlock
    return failure==2 ? -EIO : 0;
}
HELPER
int main(int argc,char **argv) {
    assert(argc==2 && !setenv("DM_VK_ZC_INIT_LOCK",argv[1],1));
    AHardwareBuffer a={0}; unsigned char temporary[4]={0},src[4]; memcpy(src,expected,4);
    assert(!pool_init_copy(&a,temporary,src,4) && !memcmp(src,expected,4));
    if(!atoi(argv[1])) {
        assert(!memcmp(temporary,expected,4) && !locks && !unlocks && !messages);
        return 0;
    }
    assert(locks==1 && unlocks==1 && messages==1 && !memcmp(temporary,"\0\0\0\0",4));
    for(failure=1;failure<=3;failure++) {
        locks=unlocks=stage=code=0;
        assert(pool_init_copy(&a,temporary,src,4)==-1 && locks==1 && messages==1);
        assert(unlocks==(failure!=1)); // failed lock never unlocks; null pointer still unlocks
        assert(stage==11+failure && code==(failure==1 ? -ENOMEM : failure==2 ? -EIO : -EINVAL));
        assert(!memcmp(src,expected,4) && !memcmp(temporary,"\0\0\0\0",4));
    }
}
'''.replace('HELPER', helper)
        with tempfile.TemporaryDirectory(prefix='nx2a-zc-init-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            for enabled in ('0', '1'):
                subprocess.run([str(exe), enabled], check=True, timeout=15)
