// Existing nxnative e6c73832b / pinned1f01e28c logic.inc oracle.
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "gfx_vk.h"
#include <assert.h>
#include <sys/mman.h>
static uint8_t *memory;
static int logic_supported;
static PFN_vkCreateGraphicsPipelines real_create;
static VkResult checked_create(VkDevice device,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo *infos,const VkAllocationCallbacks *allocator,VkPipeline *pipelines) {
    for(unsigned i=0;i<count;i++) if(infos[i].pColorBlendState && infos[i].pColorBlendState->logicOpEnable && !logic_supported)
        die("persisted logic pipeline requested a disabled device feature");
    return real_create(device,cache,count,infos,allocator,pipelines);
}
static prog_t logic_program[2]={{.nstage=2,.serial=1},{.nstage=2,.serial=2}},sparse_mrt_program={.nstage=2,.serial=3};
int __wrap_prog_key(const prog_t *p,unsigned stage,uint8_t key[8]) {
 if(stage>=2)return 0;memset(key,0,8);key[7]=stage?(p==&sparse_mrt_program?0x63:p==&logic_program[1]?0x62:0x61):0x52;return 1;
}
typedef struct {uint8_t bytes[16384];size_t used;} list_t;
static void put(list_t *l,unsigned op,const void *p,size_t n) {
    size_t size=(sizeof(op_hdr)+n+7)&~7;assert(l->used+size<=sizeof l->bytes);
    op_hdr h={op,(uint32_t)size};memcpy(l->bytes+l->used,&h,sizeof h);
    if(n)memcpy(l->bytes+l->used+sizeof h,p,n);l->used+=size;
}
#define RECORD(op,T,...) do{T value={__VA_ARGS__};put(&list,op,&value,sizeof value);}while(0)
// The four bits of the guest operation select the truth table entries for
// (source,destination) = (1,1), (1,0), (0,1), (0,0) in order.
static uint32_t logic_word(unsigned op, uint32_t src, uint32_t dst) {
    return ((op&1)?src&dst:0) | ((op&2)?src&~dst:0) | ((op&4)?~src&dst:0) | ((op&8)?~src&~dst:0);
}
static void logic_operations(void) {
    const struct { int fmt, width, channels, integer; } cases[]={
        {NVN_FORMAT_RGBA8,1,4,0}, {NVN_FORMAT_RGBA16,2,4,0},
        {NVN_FORMAT_RG32UI,4,2,1}, {NVN_FORMAT_RGBA8UI,1,4,1},
    };
    const uint32_t src[]={0x8926a396,0x54a5cc36,0x75d3c30f,0x8aa65c1b};
    const uint32_t dst[]={0x12533c65,0x96569253,0x27816346,0x587be3a4};
    for (int trial=0;trial<4;++trial) {
        int t=trial; // Target integer-output reinterpretation is a separate open PR.
        int width=cases[t].width, channels=cases[t].channels;
        uint32_t mask=UINT32_MAX>>(32-8*width), *uniform=(void *)(memory+256);
        uint8_t *seed=memory+512;memset(seed,0,64);
        size_t pixel_bytes=channels*width;
        for (int c=0;c<4;++c) {
            uniform[c]=src[c]&mask;
            if (!cases[t].integer) { float value=(float)uniform[c]/mask; memcpy(&uniform[c],&value,4); }
        }
        for (int x=0;x<4;++x) for (int c=0;c<channels;++c)
            memcpy(seed+x*pixel_bytes+c*width,&dst[c],width);
        tex_t target={.magic=MAGIC('T','E','X',0),.serial=0x8180u+t,.d={.width=4,.height=1,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=cases[t].fmt}};
        nvn_tex_layout(&target.d,&target.L);
        for (unsigned op=0;op<16;++op) {
            list_t list={0}; uint8_t *pixels=memory+1024;memset(pixels,0,64);
            RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&target,.src=(uintptr_t)seed,.r={.width=4,.height=1,.depth=1});
            RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
            RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,4,1}});
            RECORD(OP_SCISSOR,p_scissor,.x=1,.w=2,.h=1);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&logic_program[trial<4 && cases[t].integer],.stages=3);
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)uniform,.size=16);
            // Non-COPY logic replaces blending. Both blend factors zero make
            // this observable, including on an integer attachment.
            p_bind_state state={.kind=0};
            blend_st blend={.src_rgb=1,.dst_rgb=1,.src_a=1,.dst_a=1,.eq_rgb=1,.eq_a=1};
            memcpy(state.bytes,&blend,sizeof blend); put(&list,OP_BIND_STATE,&state,sizeof state);
            color_st color={.logic_op=op,.blend_en=op!=NVN_LOGIC_OP_COPY,.alpha_func=NVN_ALPHA_FUNC_ALWAYS};
            state.kind=1; memcpy(state.bytes,&color,sizeof color); put(&list,OP_BIND_STATE,&state,sizeof state);
            unsigned write_mask=(op&1)?15:5;
            chmask_st cm={.mask=write_mask}; state.kind=2; memcpy(state.bytes,&cm,sizeof cm); put(&list,OP_BIND_STATE,&state,sizeof state);
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)pixels,.r={.width=4,.height=1,.depth=1});
            put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for (int x=0;x<4;++x) for (int c=0;c<channels;++c) {
                uint32_t got=0,want=dst[c]&mask;
                if ((x==1||x==2) && (write_mask>>c&1)) {
                    unsigned effective=logic_supported ? op : NVN_LOGIC_OP_COPY;
                    want=logic_word(effective,src[c],dst[c])&mask;
                }
                memcpy(&got,pixels+x*pixel_bytes+c*width,width);
                if (got!=want) die("logic op %u format %#x pixel %d channel %d: %#x expected %#x",op,cases[t].fmt,x,c,got,want);
            }
        }
        mtl_tex_release(&target);
    }
    puts(logic_supported ? "Native logic: 16 truth tables on four formats, scissor and masks passed" : "COPY fallback: all 16 operation requests on four formats, scissor and masks passed");
}

// An attached floating-point target in the gap between two shader outputs
// must keep its data. Its format cannot disable the neighboring XOR writes.
static void sparse_mrt_outputs(void) {
    enum { width=4 };
    tex_t rt[3];
    uint32_t (*seed)[width]=(void *)(memory+512), (*result)[width]=(void *)(memory+1024);
    const uint32_t colors[]={0xff4080bf,0,0x00bf8040};
    list_t list={0};
    for (unsigned i=0;i<3;++i) {
        rt[i]=(tex_t){.magic=MAGIC('T','E','X',0),.serial=0x81b0u+i,.d={.width=width,.height=1,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=i==1 ? NVN_FORMAT_R32F : NVN_FORMAT_RGBA8}};
        nvn_tex_layout(&rt[i].d,&rt[i].L);
        for (unsigned x=0;x<width;++x) seed[i][x]=0x3f123456u+16*i+x;
        RECORD(OP_COPY_B2T,p_copy_b2t,.tex=&rt[i],.src=(uintptr_t)seed[i],.r={.width=width,.height=1,.depth=1});
    }
    RECORD(OP_SET_RT,p_set_rt,.n=3,.color={&rt[0],&rt[1],&rt[2]});
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&sparse_mrt_program,.stages=3);
    p_bind_state mask_state={.kind=2};chmask_st masks={.mask=0xfff};memcpy(mask_state.bytes,&masks,sizeof masks);put(&list,OP_BIND_STATE,&mask_state,sizeof mask_state);
    p_bind_state state={.kind=1}; color_st color={.blend_en=7,.logic_op=NVN_LOGIC_OP_XOR,.alpha_func=NVN_ALPHA_FUNC_ALWAYS};
    memcpy(state.bytes,&color,sizeof color); put(&list,OP_BIND_STATE,&state,sizeof state);
    RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,4,1}});
            RECORD(OP_SCISSOR,p_scissor,.x=1,.w=2,.h=1);
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    for (unsigned i=0;i<3;++i)
        RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&rt[i],.src=(uintptr_t)result[i],.r={.width=width,.height=1,.depth=1});
    put(&list,OP_END,NULL,0);
    mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (int i=0;i<3;++i) for (int x=0;x<width;++x) {
        uint32_t expected=seed[i][x];
        if (i!=1 && x>=1 && x<=2) expected=logic_supported ? expected^colors[i] : colors[i];
        if (result[i][x]!=expected) die("sparse MRT %d at %d: %#x expected %#x",i,x,result[i][x],expected);
    }
    for (int i=0;i<3;++i) mtl_tex_release(&rt[i]);

    puts("Sparse MRT: output locations zero and two matched the active logic/fallback mode; unwritten float attachment preserved");
}

int __wrap_main(void) {
 setbuf(stdout,NULL);if(getenv("NX2A_LOGIC_PREWARM"))unsetenv("DM_VK_NOPREWARM");else setenv("DM_VK_NOPREWARM","1",1);setenv("DM_VK_ASYNC_PIPE","0",1);
 mtl_init();logic_supported=V.has_logic_op;printf("Native logicOp available: %s\n",logic_supported?"yes":"no; validating donor COPY fallback");mtl_set_depth_zero(1);mtl_set_origin_lower_left(0);
 real_create=vkCreateGraphicsPipelines;vkCreateGraphicsPipelines=checked_create;
 memory=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(memory!=MAP_FAILED);mtl_pool_add(memory,65536);
 logic_operations();sparse_mrt_outputs();mtl_finish();if(getenv("NX2A_LOGIC_PREWARM"))usleep(500000);mtl_pool_del(memory);munmap(memory,65536);return 0;
}
