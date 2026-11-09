"""Compile the real converter setup and PCM writer; check the optional reorder."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPAT = ROOT / 'nxnative/dm/android/compat'


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class AacLayout(unittest.TestCase):
    def test_asc_channel_configuration(self):
        code = r'''
#include <assert.h>
#include "aac_layout.h"
static void bits(uint8_t *p,int *at,unsigned value,int n) {
    for(int i=n-1;i>=0;i--,(*at)++)p[*at/8]|=((value>>i)&1)<<(7-*at%8);
}
int main(void) {
    for(int escaped=0;escaped<2;escaped++)for(int explicit_rate=0;explicit_rate<2;explicit_rate++)for(int config=0;config<=7;config++) {
        uint8_t asc[8]={0};int at=0;bits(asc,&at,escaped?31:2,5);
        if(escaped)bits(asc,&at,1,6);
        bits(asc,&at,explicit_rate?15:3,4);if(explicit_rate)bits(asc,&at,48000,24);
        bits(asc,&at,config,4);int n=(at+7)/8;
        assert(aac_channel_config(asc,n)==config);
        for(int short_n=0;short_n<n;short_n++)assert(aac_channel_config(asc,short_n)==-1);
    }
    assert(aac_channel_config(NULL,0)==-1);return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'asc.c'; exe = Path(folder) / 'asc'; path.write_text(code)
            result = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(COMPAT),
                                     str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_pcm_order_opt_in(self):
        source = (COMPAT / 'audioconverter_mediacodec.c').read_text()
        setup = source[source.index('struct dm_audio_converter {'):source.index('// one descriptor')]
        start = source.index('static void keep(')
        keep = source[start:source.index('// collect the codec', start)]
        code = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <AudioToolbox/AudioToolbox.h>
#include "aac_layout.h"
typedef struct AMediaCodec AMediaCodec;
#define die(...) abort()
''' + setup + keep + r'''
static AudioConverterRef converter(int ch) {
    AudioStreamBasicDescription in={.mFormatID=kAudioFormatMPEG4AAC,.mChannelsPerFrame=(UInt32)ch};
    AudioStreamBasicDescription out={.mFormatID=kAudioFormatLinearPCM,.mChannelsPerFrame=(UInt32)ch,.mBitsPerChannel=16};
    AudioConverterRef c=NULL;assert(AudioConverterNew(&in,&out,&c)==noErr);return c;
}
int main(int argc,char **argv) {
    assert(argc==2);int enabled=atoi(argv[1]);
    static const int configs[]={1,2,3,4,5,6,7,0,7,6};
    static const int channels[]={1,2,3,4,5,6,8,6,8,2};
    static const int masks[]={0,0,0,0,0,0,0x18fc,0,0,0};
    static const int orders[][8]={{0},{0,1},{2,0,1},{2,0,1,3},{2,0,1,3,4},{2,0,1,4,5,3},
        {2,6,7,0,1,4,5,3},{0,1,2,3,4,5},{0,1,2,3,4,5,6,7},{0,1}};
    for(int k=0;k<10;k++) {
        int ch=channels[k];AudioConverterRef c=converter(ch);
        c->channel_config=configs[k];c->channel_mask=masks[k];
        int16_t pcm[24];for(int i=0;i<3*ch;i++)pcm[i]=(int16_t)(100+i);
        keep(c,(uint8_t*)pcm,2*ch);keep(c,(uint8_t*)(pcm+ch),4*ch);
        assert(c->npend==(size_t)(6*ch));
        for(int f=0;f<3;f++)for(int i=0;i<ch;i++) {
            int16_t got;memcpy(&got,c->pend+2*(f*ch+i),2);
            int want=pcm[f*ch+(enabled?orders[k][i]:i)];
            if(got!=want) {
                fprintf(stderr,"enabled=%d config=%d channels=%d mask=%#x frame=%d speaker=%d got=%d want=%d\n",enabled,configs[k],ch,masks[k],f,i,got,want);return 1;
            }
        }
        free(c->pend);free(c);
    }
    // An actual decoder downmix retains the existing channel-count fallback.
    AudioConverterRef c=converter(6);c->och=2;c->channel_config=6;
    int16_t stereo[2]={71,73};keep(c,(uint8_t*)stereo,sizeof stereo);
    int16_t expected[6]={71,73,73,73,73,73};assert(c->npend==sizeof expected && !memcmp(c->pend,expected,sizeof expected));
    free(c->pend);free(c);return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'aac.c'; exe = Path(folder) / 'aac'
            path.write_text(code)
            result = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                     '-Wno-unused-function', '-I', str(COMPAT), '-I', str(COMPAT / 'include'),
                                     str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for value in (None, '', '0', '1', 'invalid'):
                with self.subTest(env=value):
                    env = os.environ.copy(); env.pop('DM_MOVIE_AAC_MPEG_ORDER', None)
                    if value is not None: env['DM_MOVIE_AAC_MPEG_ORDER'] = value
                    result = subprocess.run([str(exe), '1' if value == '1' else '0'], env=env,
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
