// Shared inputs and independent texel-address oracle for the Maxwell corpus.
#ifndef DM_TEXTURE_OPS_CASES_H
#define DM_TEXTURE_OPS_CASES_H
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define TEX_OP_VARIANTS 24
#define TEX_OP_CASES 144
static uint32_t tex_bits(float value) { uint32_t bits;memcpy(&bits,&value,4);return bits; }
static float tex_value(unsigned mip,unsigned x,unsigned y,unsigned component) {
    return (float)(10000*mip+1000*component+100*y+x+1);
}
static void tex_level(float *pixels,unsigned mip) {
    unsigned size=8u>>mip;
    for(unsigned y=0;y<size;y++)for(unsigned x=0;x<size;x++)for(unsigned c=0;c<4;c++)
        pixels[(y*size+x)*4+c]=tex_value(mip,x,y,c);
}
static unsigned tex_wrap(unsigned index) { return (index/24)%2; }
static int tex_address(int coord,int size,unsigned repeat) {
    return repeat?((coord%size)+size)%size:coord<0?0:coord>=size?size-1:coord;
}
static void tex_case(unsigned variant,unsigned index,uint32_t *uniform,uint32_t expected[4]) {
    static const float coords[6][2]={{11.f/64,29.f/64},{37.f/64,51.f/64},{-13.f/64,75.f/64},
        {77.f/64,-7.f/64},{1.f/64,63.f/64},{63.f/64,1.f/64}};
    unsigned mip=index%4,point=(index/4)%6,shift=index/48,repeat=tex_wrap(index);
    int ox=shift==0?0:shift==1?1:-1,oy=-ox;
    uint32_t packed=((unsigned)ox&15)|(((unsigned)oy&15)<<4);
    memset(uniform,0,65536);
    uniform[0]=tex_bits(coords[point][0]);uniform[1]=tex_bits(coords[point][1]);
    if(variant>=22) {
        uniform[2]=mip;expected[0]=expected[1]=8u>>mip;
        expected[2]=variant==22?4:0;expected[3]=variant==22?0x76543210:4;return;
    }
    unsigned offset=variant<10?variant%2:(variant-10)%3;
    unsigned lod=variant<8?(variant%4)/2:1;
    if(variant>=10 || !lod)mip=0;
    unsigned size=8u>>mip;
    if(!offset)ox=oy=0;
    int x,y;
    if(variant>=4 && variant<8) {
        x=(int)(point%size);y=(int)((point*3+1)%size);
        uniform[0]=(uint32_t)(x-ox);uniform[1]=(uint32_t)(y-oy);
        uniform[2]=lod?mip:packed;uniform[3]=packed;
    } else {
        uniform[2]=tex_bits((float)mip);uniform[3]=packed;
        if(variant<4 && !lod)uniform[2]=packed;
        if(variant==8 || variant==9) {
            uniform[2]=packed<<16;uniform[4]=uniform[7]=tex_bits((float)(1u<<mip)/8);
        }
        x=(int)floorf(coords[point][0]*(float)size)+ox;
        y=(int)floorf(coords[point][1]*(float)size)+oy;
    }
    if(variant>=10) {
        unsigned component=(variant-10)/3;
        x=(int)floorf(coords[point][0]*8-.5f);y=(int)floorf(coords[point][1]*8-.5f);
        int dx[4]={0,1,1,0},dy[4]={1,1,0,0};
        uniform[2]=((unsigned)ox&63)|(((unsigned)oy&63)<<8);
        if(offset==2) {
            // PTP supplies an independent signed 6-bit (x,y) pair per result.
            dx[0]=ox;dy[0]=oy;dx[1]=-ox;dy[1]=oy;
            dx[2]=ox;dy[2]=-oy;dx[3]=-ox;dy[3]=-oy;
            uniform[2]=uniform[3]=0;
            for(unsigned c=0;c<4;c++)uniform[2+c/2]|=(((unsigned)dx[c]&63)|(((unsigned)dy[c]&63)<<8))<<(16*(c%2));
            ox=oy=0;
        }
        for(unsigned c=0;c<4;c++)expected[c]=tex_bits(tex_value(0,
            (unsigned)tex_address(x+ox+dx[c],8,repeat),(unsigned)tex_address(y+oy+dy[c],8,repeat),component));
    } else {
        x=tex_address(x,(int)size,repeat);y=tex_address(y,(int)size,repeat);
        for(unsigned c=0;c<4;c++)expected[c]=tex_bits(tex_value(mip,(unsigned)x,(unsigned)y,c));
    }
}
static int tex_check(unsigned variant,unsigned index,const uint32_t *result,const uint32_t expected[4]) {
    for(unsigned word=0;word<256;word++) {
        uint32_t want=word>=64 && word<68?expected[word-64]:0xcccccccc;
        if(result[word]!=want) { fprintf(stderr,"Texture variant%u case%u word%u: %08x expected %08x\n",variant,index,word,result[word],want);return 1; }
    }
    return fwrite(result,4,256,stdout)==256?0:1;
}
#endif
