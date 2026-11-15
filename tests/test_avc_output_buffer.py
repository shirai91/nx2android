"""Existing donor media_drain bounds and codec-config behavior at the codec seam."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('cc'),'host C compiler required')
class AVCOutputBuffer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source=(ROOT/'nxnative/dm/android/movie_avc.c').read_text(encoding='utf-8')
        at=source.index('static int drain(')
        production=source[at:source.index('\n}',at)+2]
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#define LOG(...) ((void)0)
#define AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED -2
#define AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED -3
#define AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG 2
#define AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM 4
typedef struct AMediaCodec AMediaCodec;
typedef struct {int32_t offset,size;int64_t presentationTimeUs;uint32_t flags;} AMediaCodecBufferInfo;
typedef struct {AMediaCodec *mc;int fw,nout,red;} mavc;
static AMediaCodecBufferInfo info;
static uint8_t bytes[16];static int dequeued,copied,released,missing;
static ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *mc,AMediaCodecBufferInfo *out,int64_t wait) {
    (void)mc;(void)wait;if(dequeued++)return -1;*out=info;return 0;
}
static uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *mc,size_t i,size_t *n) {
    (void)mc;assert(!i);*n=sizeof bytes;return missing?NULL:bytes;
}
static int AMediaCodec_releaseOutputBuffer(AMediaCodec *mc,size_t i,bool render) {
    (void)mc;assert(!i && !render);released++;return 0;
}
static void read_format(mavc *a) {a->fw=1;}
static void keep_picture(mavc *a,const uint8_t *src,size_t n,int64_t pts) {
    (void)a;(void)src;(void)n;assert(pts==123);copied++;
}
''' + production + r'''
int main(int argc,char **argv){assert(argc==2);int mode=atoi(argv[1]);
    mavc a={.fw=1};info=(AMediaCodecBufferInfo){.offset=3,.size=4,.presentationTimeUs=123};int expected=0;
    switch(mode){
    case 0:expected=1;break;case 1:info.offset=12;expected=1;break;
    case 2:info.offset=-1;break;case 3:info.offset=17;break;case 4:info.offset=13;break;
    case 5:info.offset=INT32_MAX;info.size=INT32_MAX;break;
    case 6:info.size=-1;break;case 7:missing=1;break;case 8:info.flags=2;break;
    case 9:info.size=0;break;case 10:info.offset=13;info.flags=4;break;
    }
    assert(drain(&a,0)==(info.flags==4));assert(copied==expected && released==1);
}
'''
        cls.temp=tempfile.TemporaryDirectory(prefix='nx2a-avc-buffer-');cls.addClassCleanup(cls.temp.cleanup)
        c=Path(cls.temp.name)/'test.c';cls.binary=c.with_suffix('.exe' if os.name=='nt' else '');c.write_text(code,encoding='utf-8')
        subprocess.run(['cc','-std=c11','-O1','-Wall','-Werror',str(c),'-o',str(cls.binary)],check=True,capture_output=True,text=True)

    def test_valid_buffers_include_exact_capacity_end(self):
        for case in (0,1):
            r=subprocess.run([str(self.binary),str(case)],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr)

    def test_invalid_and_config_buffers_are_skipped_and_released(self):
        for case in range(2,11):
            with self.subTest(case=case):
                r=subprocess.run([str(self.binary),str(case)],capture_output=True,text=True)
                self.assertEqual(r.returncode,0,r.stderr)

if __name__=='__main__':unittest.main()
