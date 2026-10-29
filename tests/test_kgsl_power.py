"""Compile the actual private-driver hook and check kernel ABI, failures and untouched calls."""
from pathlib import Path
import os,shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parent.parent

class ContextPower(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'),'host clang needed')
    def test_actual_context_hook(self):
        code=r'''
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
int kgsl_hook_ioctl(int,unsigned long,...);
struct create {uint32_t flags,id;};
struct constraint {uint32_t type,context_id;void *data;size_t size;};
struct property {uint32_t type;void *value;size_t size;};
static int mode,calls,logs,sets;
int __android_log_print(int priority,const char *tag,const char *fmt,...) {
    (void)priority;assert(tag && fmt);logs++;return 0;
}
#ifdef __APPLE__
int power_test_syscall(int nr,...) {
#else
long power_test_syscall(long nr,...) {
#endif
    assert(nr==SYS_ioctl);va_list ap;va_start(ap,nr);
    int fd=va_arg(ap,int);unsigned long req=va_arg(ap,unsigned long);void *arg=va_arg(ap,void *);va_end(ap);
    assert(fd==7);calls++;
    if(mode==0){assert(req==0x1234 && arg==(void *)1);return 17;}
    if(mode==5){assert(!arg);errno=EFAULT;return -1;}
    if(req==0xc0080913ul){
        struct create *c=arg;assert(c->flags==(uint32_t)(mode==2 && calls==2?0x41:0x841));
        if(mode==2 && calls==1){errno=EINVAL;return -1;}
        if(mode==3){errno=EACCES;return -1;}
        if(mode==6){errno=EINTR;return -1;}
        c->id=123;errno=EBUSY;return 0;
    }
    assert(req==_IOW(0x09,0x32,struct property));sets++;
    struct property *p=arg;assert(p->type==0x12 && p->size==24);
    struct constraint *c=p->value;
    assert(c->type==1 && c->context_id==123 && c->size==4 && *(uint32_t *)c->data==1);
    if(mode==4){errno=EPERM;return -1;}errno=0;return 0;
}
int main(int argc,char **argv) {
    assert(argc==2);mode=atoi(argv[1]);struct create c={0x41,0};int result;
    if(mode==0){assert(kgsl_hook_ioctl(7,0x1234,(void *)1)==17 && calls==1);return 0;}
    result=kgsl_hook_ioctl(7,0xc0080913ul,mode==5?NULL:&c);
    if(mode==2){assert(!result && c.id==123 && c.flags==0x41 && calls==2 && !sets && !logs);}
    else if(mode==3 || mode==6){assert(result==-1 && c.flags==0x41 && calls==1 && !sets && !logs && errno==(mode==3?EACCES:EINTR));}
    else if(mode==5){assert(result==-1 && errno==EFAULT && calls==1 && !sets && !logs);}
    else {assert(!result && c.id==123 && calls==2 && sets==1 && logs==1 && errno==EBUSY);}
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a-kgsl-power-') as tmp:
            p=Path(tmp);(p/'android').mkdir()
            (p/'android/log.h').write_text('#define ANDROID_LOG_INFO 4\nint __android_log_print(int,const char *,const char *,...);\n')
            (p/'check.c').write_text(code)
            r=subprocess.run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-I'+str(p),'-Dioctl=kgsl_hook_ioctl','-Dsyscall=power_test_syscall',str(ROOT/'nxnative/dm/android/driver/kgsl_power_hook.c'),str(p/'check.c'),'-o',str(p/'check')],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr)
            for mode in range(7):subprocess.run([str(p/'check'),str(mode)],check=True,timeout=5)
        patch=(ROOT/'nxnative/dm/android/adrenotools-kgsl.patch').read_text()
        self.assertIn('gpuPower && !strcmp(gpuPower, "1")',patch)
        self.assertIn('RTLD_NOW | RTLD_GLOBAL, driverNs',patch)

if __name__=='__main__':unittest.main()
