// Donor 742318ae5 tests/logic.inc sparse_mrt_outputs: seeded MRT holes survive.
// Target adaptation uses COPY (logic operations are a separate missing port).
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "gfx_vk.h"
#include <assert.h>
#include <sys/mman.h>
static prog_t programs[3]={{.nstage=2,.serial=1},{.nstage=2,.serial=2},{.nstage=1,.serial=3}};
static unsigned expected_outputs,observed;
static PFN_vkCreateGraphicsPipelines real_create;
static VkResult check_pipeline(VkDevice device,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo *infos,const VkAllocationCallbacks *allocator,VkPipeline *pipelines) {
    for(unsigned n=0;n<count;n++) {
        const VkPipelineColorBlendStateCreateInfo *blend=infos[n].pColorBlendState;
        if(!blend || blend->attachmentCount!=4)continue;
        for(unsigned i=0;i<4;i++) {
            unsigned mask=(expected_outputs>>i&1)?15:0;
            if(blend->pAttachments[i].colorWriteMask!=mask || blend->pAttachments[i].blendEnable!=!!mask)
                die("output mask %#x target %u: write=%#x blend=%u",expected_outputs,i,blend->pAttachments[i].colorWriteMask,blend->pAttachments[i].blendEnable);
        }
        observed++;
    }
    return real_create(device,cache,count,infos,allocator,pipelines);
}
int __wrap_prog_key(const prog_t *p,unsigned stage,uint8_t key[8]) {
    if(stage>=2 || (stage==1 && p==&programs[2]))return 0;
    memset(key,0,8);key[7]=stage?0x61+(p-programs):0x52;return 1;
}
typedef struct {uint8_t bytes[16384];size_t used;} list_t;
static void put(list_t *l,unsigned op,const void *p,size_t n) {
    size_t size=(sizeof(op_hdr)+n+7)&~7;assert(l->used+size<=sizeof l->bytes);
    op_hdr h={op,(uint32_t)size};memcpy(l->bytes+l->used,&h,sizeof h);
    if(n)memcpy(l->bytes+l->used+sizeof h,p,n);l->used+=size;
}
#define RECORD(op,T,...) do{T value={__VA_ARGS__};put(&list,op,&value,sizeof value);}while(0)
int __wrap_main(void) {
    setenv("DM_VK_NOPREWARM","1",1);setenv("DM_VK_ASYNC_PIPE","0",1);
    mtl_init();mtl_set_depth_zero(1);mtl_set_origin_lower_left(0);
    real_create=vkCreateGraphicsPipelines;vkCreateGraphicsPipelines=check_pipeline;
    uint32_t *memory=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(memory!=MAP_FAILED);
    mtl_pool_add(memory,65536);uint32_t (*seed)[4]=(void *)memory,*result=memory+64;
    tex_t rt[4];
    for(unsigned i=0;i<4;i++) {
        rt[i]=(tex_t){.magic=MAGIC('T','E','X',0),.serial=0x81b0+i,.d={.width=4,.height=1,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=i%2?NVN_FORMAT_R32F:NVN_FORMAT_RGBA8}};
        nvn_tex_layout(&rt[i].d,&rt[i].L);for(unsigned x=0;x<4;x++)seed[i][x]=0x3f123456+16*i+x;
    }
    int flat=getenv("DM_VK_X_FLAT_VS")!=NULL;
    for(unsigned pass=0;pass<4;pass++) {
        unsigned index=pass==3?0:pass;expected_outputs=index==2?0:flat?1:index==0?5:10;
        memset(result-4,0xa5,24*sizeof *result);list_t list={0};
        for(unsigned i=0;i<4;i++)RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&rt[i],.src=(uintptr_t)seed[i],.r={.width=4,.height=1,.depth=1});
        RECORD(OP_SET_RT,p_set_rt,.n=4,.color={&rt[0],&rt[1],&rt[2],&rt[3]});
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,4,1}});
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&programs[index],.stages=3);
        p_bind_state state={.kind=1};color_st color={.blend_en=15,.logic_op=NVN_LOGIC_OP_COPY,.alpha_func=NVN_ALPHA_FUNC_ALWAYS};
        memcpy(state.bytes,&color,sizeof color);put(&list,OP_BIND_STATE,&state,sizeof state);
        for(unsigned i=0;i<4;i++) {
            blend_st blend={.target=i,.src_rgb=NVN_BLEND_FUNC_ONE,.dst_rgb=NVN_BLEND_FUNC_ZERO,.src_a=NVN_BLEND_FUNC_ONE,.dst_a=NVN_BLEND_FUNC_ZERO,.eq_rgb=NVN_BLEND_EQUATION_ADD,.eq_a=NVN_BLEND_EQUATION_ADD};
            state.kind=0;memcpy(state.bytes,&blend,sizeof blend);put(&list,OP_BIND_STATE,&state,sizeof state);
        }
        RECORD(OP_SCISSOR,p_scissor,.x=1,.w=2,.h=1);
        RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        for(unsigned i=0;i<4;i++)RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&rt[i],.src=(uintptr_t)(result+4*i),.r={.width=4,.height=1,.depth=1});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for(unsigned i=0;i<4;i++)for(unsigned x=0;x<4;x++) {
            uint32_t want=seed[i][x];if((expected_outputs>>i&1) && x>=1 && x<=2)want=flat?0xffff00ff:i%2?0x3f800000:i==0?0xff0000ff:0xff00ff00;
            if(result[4*i+x]!=want)die("MRT pass%u target%u pixel%u got%#x expected%#x",pass,i,x,result[4*i+x],want);
        }
        for(unsigned x=0;x<4;x++)assert(result[-4+(int)x]==0xa5a5a5a5 && result[16+x]==0xa5a5a5a5);
    }
    assert(observed>=3);for(unsigned i=0;i<4;i++)mtl_tex_release(&rt[i]);mtl_finish();mtl_pool_del(memory);munmap(memory,65536);
    puts("Sparse MRT: 64 pixels, seeded float holes, shader changes, cache reuse and pipeline masks passed");return 0;
}
