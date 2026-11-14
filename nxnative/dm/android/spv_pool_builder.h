// Required builder subset from nxnative 1f01e28c spv_texel_bias.h.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t *w; size_t n, cap; } texel_words;

static int texel_append(texel_words *dst, const uint32_t *words, size_t n) {
    if (n>SIZE_MAX/sizeof *words-dst->n) return 0;
    size_t end=dst->n+n;
    if (end>dst->cap) {
        size_t cap=end;
        if (dst->cap<=SIZE_MAX/(2*sizeof *words) && cap<dst->cap*2) cap=dst->cap*2;
        if (cap>SIZE_MAX/sizeof *words) return 0;
        uint32_t *w=realloc(dst->w,cap*sizeof *w);
        if (!w) return 0;
        dst->w=w; dst->cap=cap;
    }
    memcpy(dst->w+dst->n,words,n*sizeof *words); dst->n=end;
    return 1;
}

typedef struct {
    texel_words globals, body;
    uint32_t next, boolean, bool4, uint_type, uint4, sint_type, sint4;
} texel_module;

#define TEXEL_OP(dst, op, ...) do { \
    uint32_t ins[]={0,__VA_ARGS__}; size_t len=sizeof ins/sizeof *ins; \
    ins[0]=(uint32_t)(len<<16)|(op); if (!texel_append(dst,ins,len)) goto fail; \
} while (0)

static uint32_t texel_constant(texel_module *m,uint32_t type,uint32_t value) {
    for (size_t i=0;i<m->globals.n;i+=m->globals.w[i]>>16) {
        const uint32_t *p=m->globals.w+i;
        if ((p[0]&0xffff)==43 && p[1]==type && p[3]==value) return p[2];
    }
    uint32_t id=m->next++;
    TEXEL_OP(&m->globals,43,type,id,value);
    return id;
fail:
    return 0;
}


#undef TEXEL_OP
