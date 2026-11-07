// Decode synthetic ADTS through the production AudioConverter compatibility API.
#include <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
size_t dm_aac_tail(AudioConverterRef c, void *dst, size_t cap);
typedef struct {const unsigned char *data;unsigned size,channels;int taken;} packet;
static OSStatus input(AudioConverterRef c,UInt32 *n,AudioBufferList *b,AudioStreamPacketDescription **desc,void *arg) {
    (void)c;(void)desc;packet *p=arg;
    if(p->taken){*n=0;return 0;}
    p->taken=1;*n=1;b->mNumberBuffers=1;b->mBuffers[0]=(AudioBuffer){p->channels,p->size,(void*)p->data};return 0;
}
static void require(int ok,const char *message) {if(!ok){fprintf(stderr,"%s\n",message);exit(1);}}
int main(int argc,char **argv) {
    require(argc==3,"usage: aac_decode input.aac output.pcm");
    FILE *in=fopen(argv[1],"rb");require(in!=NULL,"open input");
    require(!fseek(in,0,SEEK_END),"seek input");long length=ftell(in);require(length>=7 && length<10000000,"input size");rewind(in);
    unsigned char *data=malloc((size_t)length);require(data!=NULL,"allocate input");require(fread(data,1,(size_t)length,in)==(size_t)length,"read input");fclose(in);
    require(data[0]==0xff && (data[1]&0xf6)==0xf0,"ADTS header");
    static const int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    int fi=(data[2]>>2)&15,config=((data[2]&1)<<2)|(data[3]>>6),object=(data[2]>>6)+1;
    require(fi<13 && config>0 && config<=7,"ADTS layout");unsigned channels=config==7?8:(unsigned)config;
    AudioStreamBasicDescription src={.mSampleRate=rates[fi],.mFormatID=kAudioFormatMPEG4AAC,.mChannelsPerFrame=channels};
    AudioStreamBasicDescription dst={.mSampleRate=rates[fi],.mFormatID=kAudioFormatLinearPCM,.mChannelsPerFrame=channels,.mBitsPerChannel=16,
        .mBytesPerFrame=channels*2,.mBytesPerPacket=channels*2,.mFramesPerPacket=1,.mFormatFlags=kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked};
    AudioConverterRef converter;require(!AudioConverterNew(&src,&dst,&converter),"AudioConverterNew");
    unsigned char cookie[24]={3,22,0,0,0,4,17};cookie[20]=5;cookie[21]=2;
    cookie[22]=(unsigned char)((object<<3)|(fi>>1));cookie[23]=(unsigned char)(((fi&1)<<7)|(config<<3));
    require(!AudioConverterSetProperty(converter,kAudioConverterDecompressionMagicCookie,sizeof cookie,cookie),"AAC cookie");
    FILE *out=fopen(argv[2],"wb");require(out!=NULL,"open output");
    int16_t pcm[8192];size_t at=0,frames=0;
    while(at<(size_t)length) {
        unsigned char *p=data+at;require(at+7<=(size_t)length && p[0]==0xff && (p[1]&0xf6)==0xf0,"ADTS packet header");
        size_t size=((size_t)(p[3]&3)<<11)|((size_t)p[4]<<3)|(p[5]>>5),header=(p[1]&1)?7:9;
        require(size>=header && size<=(size_t)length-at && !(p[6]&3),"ADTS packet bounds");
        packet arg={p+header,(unsigned)(size-header),channels,0};UInt32 count=1024;
        AudioBufferList b={1,{{channels,sizeof pcm,pcm}}};
        require(!AudioConverterFillComplexBuffer(converter,input,&arg,&count,&b,NULL),"decode AAC");
        require(fwrite(pcm,1,b.mBuffers[0].mDataByteSize,out)==b.mBuffers[0].mDataByteSize,"write PCM");frames+=count;at+=size;
    }
    for(;;){size_t n=dm_aac_tail(converter,pcm,sizeof pcm);if(!n)break;require(fwrite(pcm,1,n,out)==n,"write tail");frames+=n/(2*channels);}
    require(!fclose(out),"close output");AudioConverterDispose(converter);free(data);
    printf("AAC: %u channels, %zu PCM frames\n",channels,frames);return 0;
}
