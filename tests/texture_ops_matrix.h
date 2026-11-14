// Additional paired-backend cases; mode zero retains the independent texel oracle.
#ifndef DM_TEXTURE_OPS_MATRIX_H
#define DM_TEXTURE_OPS_MATRIX_H
#include "texture_ops_cases.h"
typedef struct { unsigned width,height,levels,base,min_linear,mag_linear,mip_linear;float min_lod,max_lod; } tex_config_t;
static tex_config_t tex_config(unsigned mode) {
    tex_config_t c={8,8,4,0,0,0,0,0,3};
    if(mode) { c.min_linear=1;c.mag_linear=1;c.mip_linear=mode!=1; }
    if(mode==3)c.mag_linear=0;
    if(mode==4) { c.min_linear=0;c.min_lod=1.25f;c.max_lod=2.5f; }
    if(mode==5) { c.width=16;c.levels=5;c.base=1; }
    if(mode>=6) { c.width=11;c.height=7; }
    if(mode==7) { c.base=1;c.min_lod=.5f;c.max_lod=1.75f; }
    return c;
}
static unsigned tex_dimension(unsigned base,unsigned level) { unsigned v=base>>level;return v?v:1; }
static void tex_matrix_level(float *pixels,unsigned mode,unsigned mip,tex_config_t c) {
    if(!mode) { tex_level(pixels,mip);return; }
    unsigned w=tex_dimension(c.width,mip),h=tex_dimension(c.height,mip);
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)for(unsigned component=0;component<4;component++)
        pixels[(y*w+x)*4+component]=tex_value(mip,x,y,component);
}
static unsigned tex_matrix_count(unsigned mode) { return mode?400:TEX_OP_CASES; }
static unsigned tex_matrix_wrap(unsigned mode,unsigned index) { return !mode?tex_wrap(index):index<256?index%2:tex_wrap(index-256); }
static void tex_matrix_inputs(unsigned variant,unsigned mode,unsigned index,tex_config_t c,uint32_t *uniform,uint32_t expected[4]) {
    if(!mode) { tex_case(variant,index,uniform,expected);return; }
    static const float coords[6][2]={{11.f/64,29.f/64},{37.f/64,51.f/64},{-13.f/64,75.f/64},
        {77.f/64,-7.f/64},{1.f/64,63.f/64},{63.f/64,1.f/64}};
    static const float lods[]={-.25f,.375f,1.375f,2.375f};
    memset(uniform,0,65536);unsigned level=2,point=0;int ox,oy;
    if(index<256) { ox=(int)(index%16)-8;oy=(int)(index/16)-8; }
    else { unsigned n=index-256;level=n%4;point=(n/4)%6;ox=n/48==0?0:n/48==1?1:-1;oy=-ox; }
    uniform[0]=tex_bits(coords[point][0]);uniform[1]=tex_bits(coords[point][1]);
    uint32_t packed=((unsigned)ox&15)|(((unsigned)oy&15)<<4);
    uniform[2]=tex_bits(lods[level]);uniform[3]=packed;
    if(variant==1)uniform[2]=packed;
    if(variant==9) {
        float footprint=level==0?.25f:(float)(1u<<(level-1));
        uniform[2]=packed<<16;
        uniform[4]=tex_bits(footprint/(float)tex_dimension(c.width,c.base));
        uniform[7]=tex_bits(footprint/(float)tex_dimension(c.height,c.base));
    }
}
static int tex_matrix_check(unsigned variant,unsigned mode,unsigned index,const uint32_t *result,const uint32_t expected[4]) {
    if(!mode)return tex_check(variant,index,result,expected);
    for(unsigned word=0;word<256;word++) {
        if(word>=64 && word<68) { float value;memcpy(&value,&result[word],4);if(result[word]!=0xcccccccc && isfinite(value))continue; }
        else if(result[word]==0xcccccccc)continue;
        fprintf(stderr,"Texture variant%u mode%u case%u invalid word%u: %08x\n",variant,mode,index,word,result[word]);return 1;
    }
    return fwrite(result,4,256,stdout)==256?0:1;
}
#endif
