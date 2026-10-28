"""Exercise the real optional loader without loading an Android driver on the host."""
from pathlib import Path
import shutil,subprocess,tempfile,unittest

ROOT=Path(__file__).resolve().parent.parent

@unittest.skipUnless(shutil.which('clang'),'host clang needed')
class CustomDriver(unittest.TestCase):
    def test_loader_and_failures(self):
        source=(ROOT/'nxnative/dm/android/gfx_vk_core.c').read_text()
        start=source.index('static void *vk_open_driver(')
        function=source[start:source.index('\n}',start)+2]
        program=r'''
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
enum { RTLD_NOW=2, RTLD_LOCAL=0 };
typedef struct { const char *dli_fname; } Dl_info;
static int mode,opens,driver_calls,logs;static jmp_buf failure;
static char message[256];
static void vk_init(void) {}
#define VKLOG(...) ((void)(logs++))
static void die(const char *fmt,...) {va_list ap;va_start(ap,fmt);vsnprintf(message,sizeof message,fmt,ap);va_end(ap);longjmp(failure,1);}
static const char *option(const char *key) {
    assert(!strcmp(key,"DM_VK_DRIVER"));return mode==0?NULL:mode==1?"system":mode==3?"invalid":"turnip";
}
#define getenv option
static const char *dlerror(void){return "mock missing library or symbol";}
static int dladdr(void *address,Dl_info *info) {
    assert(address==(void *)&vk_init);info->dli_fname=mode==8?"without-slash":"/data/app/example/lib/arm64/libdmhost.so";return mode!=4;
}
static void *dlopen(const char *path,int flags) {
    assert(flags==(RTLD_NOW|RTLD_LOCAL));opens++;
    if(mode<=1)assert(!strcmp(path,"libvulkan.so"));
    else assert(!strcmp(path,"/data/app/example/lib/arm64/libadrenotools.so"));
    return mode==5?NULL:(void *)1;
}
static void *open_driver(int flags,int features,const char *tmp,const char *hooks,const char *custom,const char *name,const char *redirect,void **mapping) {
    assert(flags==(RTLD_NOW|RTLD_LOCAL) && features==1 && !tmp && !redirect && !mapping);
    assert(!strcmp(hooks,"/data/app/example/lib/arm64/") && !strcmp(custom,hooks) && !strcmp(name,"libvulkan_freedreno.so"));
    driver_calls++;return mode==7?NULL:(void *)2;
}
static void *dlsym(void *lib,const char *name) {
    assert(lib==(void *)1 && !strcmp(name,"adrenotools_open_libvulkan"));
    return mode==6?NULL:(void *)(uintptr_t)&open_driver;
}
'''+function+r'''
int main(int argc,char **argv) {
    assert(argc==2);mode=atoi(argv[1]);
    if(setjmp(failure)) {
        assert(mode>=3 && !logs);
        assert(strstr(message,mode==3?"system or turnip":mode==4?"locate native":mode==5?"adrenotools:":mode==6?"adrenotools_open_libvulkan:":mode==7?"could not be loaded":"no directory"));
        return 0;
    }
    void *result=vk_open_driver();
    assert(mode<=2 && opens==1);
    assert(mode<=1?(result==(void *)1 && !driver_calls && !logs):(result==(void *)2 && driver_calls==1 && logs==1));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a-driver-') as tmp:
            p=Path(tmp);(p/'check.c').write_text(program)
            result=subprocess.run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror',str(p/'check.c'),'-o',str(p/'check')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            for mode in range(9):subprocess.run([str(p/'check'),str(mode)],check=True,timeout=5)

if __name__=='__main__':unittest.main()
