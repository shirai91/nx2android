// Compact an exported fixed descriptor array through a per-draw index map.
// The existing shader bounds stay in place. Each map uses the same length,
// logical entries and trailing dummy entry as the uniform handle-table ABI.
#pragma once
#include "spv_pool_builder.h"

typedef struct { uint32_t binding, map_binding, count; } descriptor_table_edit;
typedef struct {
    size_t declaration;
    uint32_t binding, set, map, pointer;
    unsigned selected:1, nonuniform:1, decorated:1, accessed:1;
} descriptor_table_id;

#define TABLE_OP(dst, op, ...) do { \
    uint32_t instruction[]={0,__VA_ARGS__}; size_t length=sizeof instruction/sizeof *instruction; \
    instruction[0]=(uint32_t)(length<<16)|(op); \
    if (!texel_append(dst,instruction,length)) goto fail; \
} while (0)

static uint32_t *spv_descriptor_table(const uint32_t *words,size_t count,
        const descriptor_table_edit *edits,size_t edit_count,size_t *out_count,const char **reason) {
    *out_count=0;*reason="invalid descriptor table module";
    if (count<5 || words[0]!=0x07230203 || words[1]>0x10300 || !words[3] || words[3]>count*8) return NULL;
    uint32_t bound=words[3];
    descriptor_table_id *ids=calloc(bound,sizeof *ids);
    texel_module module={.next=bound};
    texel_words annotations={0},prefix={0},output={0};
    uint32_t bool_type=0,uint_type=0,map_pointer=0,map_struct=0,word_pointer=0;
    unsigned caps[4]={5301,5302,5307,5311},have_caps[4]={0},cap_count=3,has_extension=0;
    if (!ids) goto fail;
    for (size_t i=5;i<count;) {
        const uint32_t *p=words+i;unsigned length=p[0]>>16,op=p[0]&0xffff;
        if (!length || length>count-i) goto fail;
        if (op>=19 && op<=39 && length>=2 && p[1]<bound) ids[p[1]].declaration=i;
        if ((op==43 || op==59) && length>=4 && p[2]<bound) ids[p[2]].declaration=i;
        if (op==20 && length==2) bool_type=p[1];
        if (op==21 && length==4 && p[2]==32 && !p[3]) uint_type=p[1];
        if (op==71 && length>=3 && p[1]<bound) {
            if (p[2]==33 && length==4) ids[p[1]].binding=p[3]+1;
            if (p[2]==34 && length==4) ids[p[1]].set=p[3];
            if (p[2]==5300) ids[p[1]].decorated=1;
        }
        if (op==10 && length==8 && !memcmp(p+1,"SPV_EXT_descriptor_indexing\0",27)) has_extension=1;
        i+=length;
    }
    if (!uint_type) { uint_type=module.next++;TABLE_OP(&module.globals,21,uint_type,32,0); }
    if (!bool_type) { bool_type=module.next++;TABLE_OP(&module.globals,20,bool_type); }
    uint32_t array=module.next++;map_struct=module.next++;map_pointer=module.next++;word_pointer=module.next++;
    TABLE_OP(&module.globals,29,array,uint_type);
    TABLE_OP(&module.globals,30,map_struct,array);
    TABLE_OP(&module.globals,32,map_pointer,12,map_struct);
    TABLE_OP(&module.globals,32,word_pointer,12,uint_type);
    TABLE_OP(&annotations,71,array,6,4);
    TABLE_OP(&annotations,71,map_struct,2);
    TABLE_OP(&annotations,72,map_struct,0,35,0);
    TABLE_OP(&annotations,72,map_struct,0,24);
    uint32_t zero=texel_constant(&module,uint_type,0),one=texel_constant(&module,uint_type,1);
    if (!zero || !one) goto fail;
    for (size_t e=0;e<edit_count;e++) {
        uint32_t variable=0;
        for (uint32_t id=1;id<bound;id++) if (ids[id].binding==edits[e].binding+1) {
            if (variable || ids[id].set) goto fail;
            variable=id;
        }
        if (!variable || !ids[variable].declaration || ids[variable].selected) goto fail;
        const uint32_t *var=words+ids[variable].declaration;
        if ((var[0]&0xffff)!=59 || var[0]>>16!=4 || var[3] || var[1]>=bound || !ids[var[1]].declaration) goto fail;
        const uint32_t *pointer=words+ids[var[1]].declaration;
        if ((pointer[0]&0xffff)!=32 || pointer[0]>>16!=4 || pointer[2] || pointer[3]>=bound || !ids[pointer[3]].declaration) goto fail;
        const uint32_t *fixed=words+ids[pointer[3]].declaration;
        if (edits[e].count) {
            if ((fixed[0]&0xffff)!=28 || fixed[0]>>16!=4 || fixed[3]>=bound || !ids[fixed[3]].declaration) goto fail;
            const uint32_t *size=words+ids[fixed[3]].declaration;
            if ((size[0]&0xffff)!=43 || size[0]>>16!=4 || size[3]!=edits[e].count) goto fail;
        } else if ((fixed[0]&0xffff)!=29 || fixed[0]>>16!=3) goto fail;
        // These arrays contain images, combined images or samplers. The
        // wrapped-buffer pass subsequently changes selected 2D interfaces.
        if (fixed[2]>=bound || !ids[fixed[2]].declaration) goto fail;
        const uint32_t *element=words+ids[fixed[2]].declaration;
        unsigned op=element[0]&0xffff;
        if (op!=25 && op!=26 && op!=27) goto fail;
        if (op==25 && element[3]==5) cap_count=4;
        uint32_t runtime=edits[e].count ? 0 : pointer[3],new_pointer=edits[e].count ? 0 : var[1];
        for (size_t at=0;at<module.globals.n;at+=module.globals.w[at]>>16) {
            const uint32_t *p=module.globals.w+at;
            if ((p[0]&0xffff)==29 && p[2]==fixed[2]) runtime=p[1];
            if ((p[0]&0xffff)==32 && p[2]==0 && p[3]==runtime) new_pointer=p[1];
        }
        if (!runtime) { runtime=module.next++;TABLE_OP(&module.globals,29,runtime,fixed[2]); }
        if (!new_pointer) { new_pointer=module.next++;TABLE_OP(&module.globals,32,new_pointer,0,runtime); }
        descriptor_table_id *selected=&ids[variable];
        selected->selected=1;selected->pointer=new_pointer;selected->map=module.next++;
        TABLE_OP(&module.globals,59,new_pointer,variable,0);
        TABLE_OP(&module.globals,59,map_pointer,selected->map,12);
        TABLE_OP(&annotations,71,selected->map,34,0);
        TABLE_OP(&annotations,71,selected->map,33,edits[e].map_binding);
    }
    for (size_t i=5;i<count;i+=words[i]>>16) {
        const uint32_t *p=words+i;unsigned length=p[0]>>16,op=p[0]&0xffff;
        if (op==59 && p[2]<bound && ids[p[2]].selected) continue;
        if ((op==65 || op==66) && length>=5 && p[3]<bound && ids[p[3]].selected) {
            if (length!=5) goto fail;
            descriptor_table_id *selected=&ids[p[3]];selected->accessed=1;
            uint32_t index=module.next++,limit_pointer=module.next++,limit=module.next++,valid=module.next++;
            uint32_t bounded=module.next++,offset=module.next++,entry=module.next++,mapped=module.next++;
            TABLE_OP(&module.body,124,uint_type,index,p[4]);
            TABLE_OP(&module.body,65,word_pointer,limit_pointer,selected->map,zero,zero);
            TABLE_OP(&module.body,61,uint_type,limit,limit_pointer);
            TABLE_OP(&module.body,176,bool_type,valid,index,limit);
            TABLE_OP(&module.body,169,uint_type,bounded,valid,index,limit);
            TABLE_OP(&module.body,128,uint_type,offset,bounded,one);
            TABLE_OP(&module.body,65,word_pointer,entry,selected->map,zero,offset);
            TABLE_OP(&module.body,61,uint_type,mapped,entry);
            TABLE_OP(&module.body,op,p[1],p[2],p[3],mapped);
            TABLE_OP(&annotations,71,mapped,5300);
            if (p[2]>=bound) goto fail;
            ids[p[2]].nonuniform=1;
            continue;
        }
        if ((op==61 || op==83 || op==100) && length>=4 && p[3]<bound && ids[p[3]].selected) goto fail;
        if (!texel_append(&module.body,p,length)) goto fail;
    }
    int changed;
    do {
        changed=0;
        for (size_t i=5;i<count;i+=words[i]>>16) {
            const uint32_t *p=words+i;unsigned length=p[0]>>16,op=p[0]&0xffff;
            if (length<4 || p[2]>=bound || ids[p[2]].nonuniform) continue;
            unsigned affected=0;
            if (op==61 || op==83 || op==100) affected=p[3]<bound && ids[p[3]].nonuniform;
            else if (op==86 && length==5) affected=(p[3]<bound && ids[p[3]].nonuniform) || (p[4]<bound && ids[p[4]].nonuniform);
            if (affected) { ids[p[2]].nonuniform=1;changed=1; }
        }
    } while (changed);
    for (uint32_t id=1;id<bound;id++) {
        if (ids[id].selected && !ids[id].accessed) goto fail;
        if (ids[id].nonuniform && !ids[id].decorated) TABLE_OP(&annotations,71,id,5300);
    }
    // Re-read capabilities after choosing the sampled-image/texel category.
    memset(have_caps,0,sizeof have_caps);
    for (size_t i=5;i<count;i+=words[i]>>16) if ((words[i]&0xffff)==17)
        for (unsigned c=0;c<cap_count;c++) if (words[i+1]==caps[c]) have_caps[c]=1;
    for (unsigned c=0;c<cap_count;c++) if (!have_caps[c]) TABLE_OP(&prefix,17,caps[c]);
    if (!texel_append(&output,words,5)) goto fail;
    int inserted_caps=0,inserted_annotations=0,inserted_globals=0;
    for (size_t i=0;i<module.body.n;i+=module.body.w[i]>>16) {
        const uint32_t *p=module.body.w+i;unsigned length=p[0]>>16,op=p[0]&0xffff;
        if (!inserted_caps && op!=17) {
            if (!texel_append(&output,prefix.w,prefix.n)) goto fail;
            if (!has_extension) {
                uint32_t extension[8]={8u<<16|10};memcpy(extension+1,"SPV_EXT_descriptor_indexing",27);
                if (!texel_append(&output,extension,8)) goto fail;
            }
            inserted_caps=1;
        }
        if (!inserted_annotations && op>=19 && op<=39) {
            if (!texel_append(&output,annotations.w,annotations.n)) goto fail;
            inserted_annotations=1;
        }
        if (!inserted_globals && op==54) {
            if (!texel_append(&output,module.globals.w,module.globals.n)) goto fail;
            inserted_globals=1;
        }
        if (!texel_append(&output,p,length)) goto fail;
    }
    if (!inserted_annotations || !inserted_globals) goto fail;
    output.w[3]=module.next;*out_count=output.n;*reason=NULL;
    free(ids);free(module.globals.w);free(module.body.w);free(annotations.w);free(prefix.w);
    return output.w;
fail:
    free(ids);free(module.globals.w);free(module.body.w);free(annotations.w);free(prefix.w);free(output.w);
    return NULL;
}
#undef TABLE_OP
