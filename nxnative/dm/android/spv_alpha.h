// Run Maxwell's alpha comparison on the final float color 0 before returning
// from the fragment entry point. Keep the guest's control flow, discard,
// storage writes and EarlyFragmentTests execution mode intact.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { DM_VK_ALPHA_PUSH_OFFSET = 16, DM_VK_ALPHA_PUSH_BYTES = 8 };
#include "spv_fragment_outputs.h"

static uint32_t *spv_alpha(const uint32_t *w, size_t nw, size_t *out_words, const char **reason) {
    enum { EntryPoint=15, TypeVoid=19, TypeBool=20, TypeInt=21, TypeFloat=22, TypeVector=23,
        TypeStruct=30, TypePointer=32, Constant=43, Function=54, FunctionEnd=56,
        FunctionCall=57, Variable=59, Load=61, AccessChain=65, Decorate=71, MemberDecorate=72,
        CompositeExtract=81, FConvert=115, Select=169, INotEqual=171, FOrdEqual=180,
        FOrdLessThan=184, FOrdGreaterThan=186, BitwiseAnd=199, SelectionMerge=247, Label=248,
        BranchConditional=250, Kill=252, Return=253, Output=3, PushConstant=9 };
    *reason="invalid fragment module";
    if (nw<5 || w[0]!=0x07230203 || w[1]>0x10300 || !w[3] || w[3]>0x3fffff || nw>(SIZE_MAX/4-256)/5) return NULL;
    uint32_t bound=w[3], entry=0, void_type=0, bool_type=0, uint_type=0, float_type=0, fn_type=0;
    size_t types=0, functions=0, returns=0;
    int in_entry=0;
    for (size_t i=5; i<nw;) {
        const uint32_t *p=w+i;
        unsigned op=p[0]&0xffff, n=p[0]>>16;
        if (!n || n>nw-i) return NULL;
        if (op==EntryPoint) {
            if (n<4 || p[1]!=4 || entry) return NULL;
            entry=p[2];
        }
        if (op>=TypeVoid && op<=39 && !types) types=i;
        if (op==Function) {
            if (n!=5) return NULL;
            if (!functions) functions=i;
            in_entry=p[2]==entry;
            if (in_entry) fn_type=p[4];
        }
        if (op==FunctionEnd) in_entry=0;
        if (in_entry && op==Return) ++returns;
        if (op==TypeVoid && n==2) void_type=p[1];
        if (op==TypeBool && n==2) bool_type=p[1];
        if (op==TypeInt && n==4 && p[2]==32 && !p[3]) uint_type=p[1];
        if (op==TypeFloat && n==3 && p[2]==32) float_type=p[1];
        if (op==Variable && n>=4 && p[3]==PushConstant) {
            *reason="fragment already declares host push constants"; return NULL;
        }
        i+=n;
    }
    if (!types || types>=functions || !entry || !fn_type || !void_type) return NULL;
    fragment_outputs output=spv_fragment_outputs(w,nw);
    if (!output.alpha_var) { *reason="color 0 has no float alpha"; return NULL; }
    uint32_t color=output.alpha_var, vec=output.alpha_vec, scalar=output.alpha_scalar;
    uint32_t *out=malloc((nw+256+4*returns)*sizeof *out);
    if (!out) { *reason="out of memory"; return NULL; }
    size_t at=types;
    memcpy(out,w,types*sizeof *w);
    // Emit SPIR-V words with their operand count taken from the instruction.
#define ALPHA_EMIT(op, ...) do { const uint32_t args[]={__VA_ARGS__}; size_t count=sizeof args/sizeof *args; \
    out[at++]=((uint32_t)(1+count)<<16)|(op); memcpy(out+at,args,sizeof args); at+=count; } while (0)
    uint32_t block=bound++, block_ptr=bound++, uint_ptr=bound++, float_ptr=bound++, push=bound++, helper=bound++;
    ALPHA_EMIT(Decorate,block,2); // Block
    ALPHA_EMIT(MemberDecorate,block,0,35,DM_VK_ALPHA_PUSH_OFFSET);
    ALPHA_EMIT(MemberDecorate,block,1,35,DM_VK_ALPHA_PUSH_OFFSET+4);
    memcpy(out+at,w+types,(functions-types)*sizeof *w); at+=functions-types;
    if (!bool_type) { bool_type=bound++; ALPHA_EMIT(TypeBool,bool_type); }
    if (!uint_type) { uint_type=bound++; ALPHA_EMIT(TypeInt,uint_type,32,0); }
    if (!float_type) { float_type=bound++; ALPHA_EMIT(TypeFloat,float_type,32); }
    ALPHA_EMIT(TypeStruct,block,uint_type,float_type);
    ALPHA_EMIT(TypePointer,block_ptr,PushConstant,block);
    ALPHA_EMIT(TypePointer,uint_ptr,PushConstant,uint_type);
    ALPHA_EMIT(TypePointer,float_ptr,PushConstant,float_type);
    ALPHA_EMIT(Variable,block_ptr,push,PushConstant);
    uint32_t zero=bound++, one=bound++, two=bound++, four=bound++, eight=bound++;
    ALPHA_EMIT(Constant,uint_type,zero,0); ALPHA_EMIT(Constant,uint_type,one,1);
    ALPHA_EMIT(Constant,uint_type,two,2); ALPHA_EMIT(Constant,uint_type,four,4); ALPHA_EMIT(Constant,uint_type,eight,8);
    for (size_t i=functions; i<nw; i+=w[i]>>16) {
        unsigned op=w[i]&0xffff, n=w[i]>>16;
        if (op==Function) in_entry=w[i+2]==entry;
        if (op==FunctionEnd) in_entry=0;
        if (in_entry && op==Return) ALPHA_EMIT(FunctionCall,void_type,bound++,helper);
        memcpy(out+at,w+i,n*sizeof *w); at+=n;
    }
    ALPHA_EMIT(Function,void_type,helper,0,fn_type);
    ALPHA_EMIT(Label,bound++);
    uint32_t value=bound++, alpha=bound++;
    ALPHA_EMIT(Load,vec,value,color); ALPHA_EMIT(CompositeExtract,scalar,alpha,value,3);
    if (scalar!=float_type) { uint32_t wide=bound++; ALPHA_EMIT(FConvert,float_type,wide,alpha); alpha=wide; }
    uint32_t mp=bound++, rp=bound++, mask=bound++, ref=bound++;
    ALPHA_EMIT(AccessChain,uint_ptr,mp,push,zero); ALPHA_EMIT(Load,uint_type,mask,mp);
    ALPHA_EMIT(AccessChain,float_ptr,rp,push,one); ALPHA_EMIT(Load,float_type,ref,rp);
    // Outcomes are less=1, equal=2, greater=4, unordered=8. NOTEQUAL
    // accepts unordered (NaN); all ordered comparisons reject it.
    uint32_t lt=bound++, eq=bound++, gt=bound++, high=bound++, equal=bound++, outcome=bound++;
    ALPHA_EMIT(FOrdLessThan,bool_type,lt,alpha,ref); ALPHA_EMIT(FOrdEqual,bool_type,eq,alpha,ref);
    ALPHA_EMIT(FOrdGreaterThan,bool_type,gt,alpha,ref); ALPHA_EMIT(Select,uint_type,high,gt,four,eight);
    ALPHA_EMIT(Select,uint_type,equal,eq,two,high); ALPHA_EMIT(Select,uint_type,outcome,lt,one,equal);
    uint32_t bits=bound++, pass=bound++, keep=bound++, reject=bound++;
    ALPHA_EMIT(BitwiseAnd,uint_type,bits,outcome,mask); ALPHA_EMIT(INotEqual,bool_type,pass,bits,zero);
    ALPHA_EMIT(SelectionMerge,keep,0); ALPHA_EMIT(BranchConditional,pass,keep,reject);
    ALPHA_EMIT(Label,reject); out[at++]=1u<<16|Kill;
    ALPHA_EMIT(Label,keep); out[at++]=1u<<16|Return; out[at++]=1u<<16|FunctionEnd;
#undef ALPHA_EMIT
    out[3]=bound; *out_words=at; *reason=NULL; return out;
}
