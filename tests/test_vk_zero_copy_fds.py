"""Inspect failed zero-copy handles without changing their state or selection."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ZeroCopyFds(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_metadata_disabled_errors_and_limit(self):
        source = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = source.index('static void mem_diag_zc_fds(')
        helper = source[start:source.index('\n}\n', start) + 3]
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int enabled, calls, messages;
static char lines[20][1200];
static int mem_diag_on(void) { return enabled; }
static void log_message(const char *fmt, ...) {
    assert(messages < 20); va_list a; va_start(a, fmt);
    vsnprintf(lines[messages++], sizeof lines[0], fmt, a); va_end(a);
    errno = ERANGE; // logging must not replace the caller's error
}
#define VKLOG log_message
static int observed_flags(int fd, int cmd) { calls++; return fcntl(fd, cmd); }
static int observed_stat(int fd, struct stat *s) { calls++; return fstat(fd, s); }
static ssize_t observed_link(const char *p, char *b, size_t n) { calls++; return readlink(p, b, n); }
#define fcntl observed_flags
#define fstat observed_stat
#define readlink observed_link
HELPER
#undef fcntl
#undef fstat
#undef readlink
int main(void) {
    int pipefd[2]; assert(!pipe(pipefd));
    char path[] = "/tmp/nx2a-fd-test-XXXXXX"; int file = mkstemp(path);
    assert(file >= 0 && !unlink(path) && write(file, "abc", 3) == 3);
    int fds[] = {pipefd[0], pipefd[1], file, -1};
    int before[3]; for (int i=0;i<3;i++) before[i]=fcntl(fds[i], F_GETFL);
    errno=EACCES; mem_diag_zc_fds(fds, 4, 4096, EACCES, 0);
    assert(errno==EACCES && !calls && !messages); // diagnostics off: no FD inspection
    enabled=1; mem_diag_zc_fds(fds, 4, 4096, EACCES, 0);
    assert(errno==EACCES && calls==12 && messages==5);
    assert(strstr(lines[0], "errno=13") && strstr(lines[0], "received=4, selected=0"));
    assert(strstr(lines[1], "access=read-only") && strstr(lines[1], "kind=fifo"));
    assert(strstr(lines[2], "access=write-only") && strstr(lines[2], "kind=fifo"));
    assert(strstr(lines[3], "access=read-write") && strstr(lines[3], "kind=regular") && strstr(lines[3], "size=3 "));
    assert(strstr(lines[4], "access=unknown") && strstr(lines[4], "flags_errno=9") &&
           strstr(lines[4], "stat_errno=9") && strstr(lines[4], "target=unavailable") && strstr(lines[4], "link_errno=2"));
    for (int i=0;i<6;i++) mem_diag_zc_fds(fds, 4, 4096, EACCES, 0);
    assert(errno==EACCES && calls==36 && messages==15); // first three failed pools only
    for (int i=0;i<3;i++) assert(fcntl(fds[i], F_GETFL)==before[i]);
    assert(lseek(file, 0, SEEK_CUR)==3); // inspection neither closes nor reads the handles
    assert(!close(file) && !close(pipefd[0]) && !close(pipefd[1]));
}
'''.replace('HELPER', helper)
        with tempfile.TemporaryDirectory(prefix='nx2a-fd-diag-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)

    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_payload_selection_and_conservative_fallback(self):
        source = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = source.index('static int pool_data_fd(')
        helper = source[start:source.index('\n}\n', start) + 3]
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static struct { int flags, stat_error, link_error; off_t size; const char *target; } meta[3];
static int calls;
static int mock_flags(int fd, int cmd) { assert(cmd==F_GETFL); calls++; return meta[fd-10].flags; }
static int mock_stat(int fd, struct stat *st) { calls++; st->st_size=meta[fd-10].size; return meta[fd-10].stat_error; }
static ssize_t mock_link(const char *path, char *out, size_t cap) {
    int fd; assert(sscanf(path,"/proc/self/fd/%d",&fd)==1); calls++;
    if (meta[fd-10].link_error) return -1;
    size_t n=strlen(meta[fd-10].target); if(n>cap)n=cap; memcpy(out,meta[fd-10].target,n); return (ssize_t)n;
}
#define fcntl mock_flags
#define fstat mock_stat
#define readlink mock_link
HELPER
int main(int argc,char **argv) {
    assert(argc==2 && !setenv("DM_VK_ZC_FD_SCAN",argv[1],1));
    int fds[]={10,11,12};
    meta[0].flags=O_RDONLY; meta[0].target="anon_inode:gralloc_extra";
    meta[1].flags=O_RDWR; meta[1].size=4096; meta[1].target="/dmabuf:G:pid [123]";
    meta[2].flags=O_RDWR; meta[2].size=6480; meta[2].target="/memfd:gralloc_shared_memory (deleted)";
    if (!atoi(argv[1])) {
        assert(pool_data_fd(fds,3,4096)==0 && !calls);
        assert(!setenv("DM_VK_ZC_FD_SCAN","1",1) && pool_data_fd(fds,3,4096)==0 && !calls);
        return 0;
    }
    assert(pool_data_fd(fds,3,4096)==1); // observed Mali layout, even with a larger metadata memfd
    assert(!setenv("DM_VK_ZC_FD_SCAN","0",1) && pool_data_fd(fds,3,4096)==1); // policy stays fixed for later remaps
    meta[1].size=8192; assert(pool_data_fd(fds,3,4096)==1); // allocation padding
    meta[1].size=2048; assert(pool_data_fd(fds,3,4096)==-1 && errno==ENODEV);
    meta[1].size=4096; meta[1].flags=O_RDONLY; assert(pool_data_fd(fds,3,4096)==-1);
    meta[1].flags=O_WRONLY; assert(pool_data_fd(fds,3,4096)==-1);
    meta[1].flags=-1; assert(pool_data_fd(fds,3,4096)==-1);
    meta[1].flags=O_RDWR; meta[1].stat_error=-1; assert(pool_data_fd(fds,3,4096)==-1);
    meta[1].stat_error=0; meta[1].link_error=1; assert(pool_data_fd(fds,3,4096)==-1);
    meta[1].link_error=0; meta[0]=meta[1]; assert(pool_data_fd(fds,3,4096)==-1 && errno==EEXIST);
    assert(pool_data_fd(fds,1,4096)==0); // payload first, as on the existing path
    meta[0].target="anon_inode:dmabuf"; assert(pool_data_fd(fds,1,4096)==0);
    meta[0].target="anon_inode:[dmabuf]"; assert(pool_data_fd(fds,1,4096)==0);
    meta[0].target="/memfd:unknown"; assert(pool_data_fd(fds,1,4096)==-1);
    assert(pool_data_fd(fds,0,4096)==-1);
}
'''.replace('HELPER', helper)
        with tempfile.TemporaryDirectory(prefix='nx2a-fd-select-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            for enabled in ('0', '1'):
                subprocess.run([str(exe), enabled], check=True, timeout=15)
