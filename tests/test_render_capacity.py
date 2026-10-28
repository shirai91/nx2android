"""The optional render affinity uses allowed CPU capacities, without assuming core numbers."""
from pathlib import Path
import os,shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parent.parent

@unittest.skipUnless(shutil.which('clang'),'host clang needed')
class RenderCapacity(unittest.TestCase):
    def test_allowed_capacity_selection_and_safe_failures(self):
        source=(ROOT/'nxnative/dm/android/perf_android.c').read_text()
        start=source.index('static void render_big_core(')
        function=source[start:source.index('\n}',start)+2]
        code=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t cpu_set_t;
#define CPU_SETSIZE 8
#define CPU_ZERO(p) (*(p)=0)
#define CPU_ISSET(i,p) ((*(p)>>(i))&1)
#define CPU_SET(i,p) (*(p)|=1ull<<(i))
static char message[256];
#define LOG(...) ((void)snprintf(message,sizeof message,__VA_ARGS__))
static int test_case,calls,opens;
static uint64_t requested;
static int gettid(void){return 123;}
static const char *option(const char *name){assert(!strcmp(name,"DM_RENDER_BIG_CORE"));return test_case==0?NULL:test_case==7?"invalid":"1";}
#define getenv option
static int sched_getaffinity(int pid,size_t size,cpu_set_t *set){assert(pid==0 && size==sizeof *set);if(test_case==4)return -1;*set=test_case==2?0x70:0xff;return 0;}
static int sched_setaffinity(int pid,size_t size,const cpu_set_t *set){assert(pid==0 && size==sizeof *set);calls++;requested=*set;return test_case==6?-1:0;}
static FILE *capacity_file(const char *path,const char *mode) {
    int cpu=-1;assert(!strcmp(mode,"r") && sscanf(path,"/sys/devices/system/cpu/cpu%d/cpu_capacity",&cpu)==1 && cpu>=0 && cpu<8);opens++;
    if(test_case==3 && cpu==5)return NULL;
    FILE *f=tmpfile();assert(f);
    int capacity=test_case==5?512:cpu<4?277:cpu<7?832:1024;
    if(test_case==8 && cpu==5)fputs("invalid",f);else fprintf(f,"%d",capacity);
    rewind(f);return f;
}
#define fopen capacity_file
'''+function+r'''
int main(int argc,char **argv) {
    assert(argc==2);test_case=atoi(argv[1]);render_big_core();render_big_core();
    if(test_case==1 || test_case==6)assert(calls==1 && requested==0x80);
    else if(test_case==2)assert(calls==0); /* allowed subset is homogeneous, CPU 7 is forbidden */
    else assert(calls==0);
    if(test_case==0 || test_case==7)assert(opens==0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a-render-capacity-') as tmp:
            p=Path(tmp);(p/'check.c').write_text(code)
            result=subprocess.run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror',str(p/'check.c'),'-o',str(p/'check')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            for case in range(9):subprocess.run([str(p/'check'),str(case)],check=True)
if __name__=='__main__':unittest.main()
