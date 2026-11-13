// The Maxwell exporter declares standalone numeric fragment outputs. Reflect
// their locations before choosing attachment operations and shader variants.
#pragma once
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uint32_t alpha_var, alpha_vec, alpha_scalar;
    uint8_t targets, uint_targets, sint_targets;
} fragment_outputs;

enum { OUT_FLOAT, OUT_UINT, OUT_SINT };
typedef struct {
    uint32_t type, element;
    uint8_t bits, kind, lanes, location, located, component, index, builtin, variable, cast;
} fragment_id;

// Return the declaration graph as well when specializing attachment types.
static fragment_id *spv_output_ids(const uint32_t *w, size_t nw, fragment_outputs *out, size_t *functions) {
    enum { EntryPoint=15, TypeInt=21, TypeFloat=22, TypeVector=23, TypePointer=32, Function=54,
        Variable=59, Decorate=71, GroupDecorate=74, GroupMemberDecorate=75, Output=3 };
    *out=(fragment_outputs){.targets=UINT8_MAX};
    if (nw<5 || w[0]!=0x07230203 || !w[3] || w[3]>0x3fffff) return NULL;
    uint32_t bound=w[3];
    fragment_id *id=calloc(bound,sizeof *id);
    if (!id) return NULL;
    fragment_outputs found={0};
    unsigned entries=0;
    for (size_t i=5;i<nw;) {
        const uint32_t *p=w+i;
        unsigned op=p[0]&0xffff, n=p[0]>>16;
        if (!n || n>nw-i || op==GroupDecorate || op==GroupMemberDecorate) goto unknown_interface;
        if (op==Function) { *functions=i; break; }
        if (op==EntryPoint && (n<4 || p[1]!=4 || ++entries!=1)) goto unknown_interface;
        if (((op==TypeFloat && n==3) || (op==TypeInt && n==4)) && p[1]<bound) {
            id[p[1]].bits=p[2]<=32 ? (uint8_t)p[2] : 0;
            id[p[1]].kind=op==TypeFloat ? OUT_FLOAT : p[3] ? OUT_SINT : OUT_UINT;
            id[p[1]].lanes=1;
        }
        if (op==TypeVector && n==4 && p[1]<bound && p[2]<bound && p[3]<=4) {
            id[p[1]].element=p[2]; id[p[1]].lanes=(uint8_t)p[3];
            id[p[1]].bits=id[p[2]].bits; id[p[1]].kind=id[p[2]].kind;
        }
        if (op==TypePointer && n==4 && p[1]<bound && p[3]<bound && p[2]==Output) id[p[1]].type=p[3];
        if (op==Decorate && n==4 && p[1]<bound) {
            fragment_id *v=id+p[1];
            switch (p[2]) {
                case 11: v->builtin=1; break;
                case 30: v->located=1; v->location=p[3]<8 ? (uint8_t)p[3] : 8; break;
                case 31: v->component=!!p[3]; break;
                case 32: v->index=!!p[3]; break;
            }
        }
        if (op==Variable && n>=4 && p[3]==Output && p[1]<bound && p[2]<bound) {
            const fragment_id *v=id+p[2];
            uint32_t type=id[p[1]].type;
            id[p[2]].variable=1; id[p[2]].type=type;
            if (!v->builtin && !v->index) {
                if (!v->located || !id[type].bits) found.targets=UINT8_MAX;
                else if (v->location<8) {
                    uint8_t bit=(uint8_t)(1u<<v->location);
                    found.targets|=bit;
                    if (id[type].kind==OUT_UINT) found.uint_targets|=bit;
                    if (id[type].kind==OUT_SINT) found.sint_targets|=bit;
                }
                uint32_t element=id[type].element;
                if (v->located && !v->location && !v->component && id[type].lanes==4 && id[element].kind==OUT_FLOAT && (id[element].bits==16 || id[element].bits==32)) {
                    found.alpha_var=p[2]; found.alpha_vec=type; found.alpha_scalar=element;
                }
            }
        }
        i+=n;
    }
    if (entries!=1 || !*functions) goto unknown_interface;
    *out=found; return id;
unknown_interface:
    free(id); return NULL;
}

static fragment_outputs spv_fragment_outputs(const uint32_t *w, size_t nw) {
    fragment_outputs out;
    size_t functions=0;
    free(spv_output_ids(w,nw,&out,&functions));
    return out;
}
