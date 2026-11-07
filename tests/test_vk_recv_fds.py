"""Receive real SCM_RIGHTS packets and reject truncation without leaking FDs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ReceiveFDs(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_socket_packets_and_cleanup(self):
        source = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c').read_text()
        start = source.index('static int recv_fds(')
        function = source[start:source.index('\n}\n', start) + 3]
        harness = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static int closed;
static int tracked_close(int fd) { closed++;int r=close(fd);errno=EBADF;return r; }
#define close tracked_close
FUNCTION
#undef close
static int count_fds(void) {
    DIR *d=opendir("/proc/self/fd");assert(d);int n=0;
    while(readdir(d))n++;
    closedir(d);return n;
}
static void send_packet(int sock,int original,int count,size_t bytes) {
    union { struct cmsghdr align;char bytes[CMSG_SPACE(32*sizeof(int))]; } ctl;
    char data[5000]={0};assert(bytes<=sizeof data && count<=32);
    struct iovec iov={data,bytes};struct msghdr m={0};m.msg_iov=&iov;m.msg_iovlen=1;
    if(count) {
        m.msg_control=ctl.bytes;m.msg_controllen=CMSG_SPACE(count*sizeof(int));
        struct cmsghdr *c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(count*sizeof(int));
        for(int i=0;i<count;i++)((int *)CMSG_DATA(c))[i]=original;
    }
    assert(sendmsg(sock,&m,0)==(ssize_t)bytes);
}
int main(void) {
    int sv[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,sv));
    int original=open("/dev/null",O_RDWR);assert(original>=0);int fds[16];
    const int counts[]={0,1,3,16};
    for(unsigned t=0;t<sizeof counts/sizeof *counts;t++) {
        int before=count_fds();closed=0;send_packet(sv[0],original,counts[t],32);
        assert(recv_fds(sv[1],fds,16)==counts[t] && !closed);
        assert(count_fds()==before+counts[t]);
        for(int i=0;i<counts[t];i++) { assert(fds[i]!=original && fcntl(fds[i],F_GETFL)>=0);close(fds[i]); }
        assert(count_fds()==before);
    }
    // Caller capacity overflow, ancillary truncation, payload truncation, both truncations.
    const int sent[]={3,32,3,32},capacity[]={1,16,16,16};
    const size_t bytes[]={32,32,5000,5000};
    for(int t=0;t<4;t++) {
        int before=count_fds();closed=0;send_packet(sv[0],original,sent[t],bytes[t]);
        assert(recv_fds(sv[1],fds,capacity[t])==-1 && errno==EMSGSIZE);
        assert(closed>0 && closed<=16 && count_fds()==before);
        assert(fcntl(original,F_GETFL)>=0);
    }
    send_packet(sv[0],original,1,32);closed=0;
    assert(recv_fds(sv[1],fds,0)==-1 && errno==EINVAL && !closed);
    assert(recv_fds(sv[1],fds,-1)==-1 && errno==EINVAL && !closed);
    assert(recv_fds(sv[1],fds,17)==-1 && errno==EINVAL && !closed);
    assert(recv_fds(sv[1],fds,16)==1);close(fds[0]);
    closed=0;assert(recv_fds(-1,fds,16)==-1 && errno==EBADF && !closed);
    close(original);close(sv[0]);close(sv[1]);
}
'''.replace('FUNCTION', function)
        with tempfile.TemporaryDirectory(prefix='nx2a-recv-fds-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)
