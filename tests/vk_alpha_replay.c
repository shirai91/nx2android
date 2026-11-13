// Android replay adapter for the source Android-parity alpha fixture.
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include <assert.h>
#include <math.h>
#include <sys/mman.h>
static prog_t alpha_program[3]={{.nstage=2,.serial=1},{.nstage=2,.serial=2},{.nstage=2,.serial=3}};
static uint32_t *write_memory;
int __wrap_prog_key(const prog_t *p,unsigned stage,uint8_t key[8]) {
    if(stage>=2) return 0;memset(key,0,8);key[7]=stage?0x61+(p-alpha_program):0x52;return 1;
}
typedef struct { uint8_t bytes[16384]; size_t used; } list_t;
static void put(list_t *l,unsigned op,const void *p,size_t n) {
    size_t size=(sizeof(op_hdr)+n+7)&~7;assert(l->used+size<=sizeof l->bytes);
    op_hdr h={op,(uint32_t)size};memcpy(l->bytes+l->used,&h,sizeof h);
    if(n) memcpy(l->bytes+l->used+sizeof h,p,n);l->used+=size;
}
#define RECORD(op,T,...) do { T value={__VA_ARGS__};put(&list,op,&value,sizeof value); } while(0)
// Match the Metal alpha fixture's ramp and references. Read color and depth
// independently, including early fragment tests and pre-discard storage writes.
static int alpha_passes(unsigned function, float alpha, float reference) {
    switch (function) {
        case NVN_ALPHA_FUNC_NEVER: return 0;
        case NVN_ALPHA_FUNC_LESS: return alpha < reference;
        case NVN_ALPHA_FUNC_EQUAL: return alpha == reference;
        case NVN_ALPHA_FUNC_LEQUAL: return alpha <= reference;
        case NVN_ALPHA_FUNC_GREATER: return alpha > reference;
        case NVN_ALPHA_FUNC_NOTEQUAL: return alpha != reference;
        case NVN_ALPHA_FUNC_GEQUAL: return alpha >= reference;
        default: return 1;
    }
}
static void alpha_references(void) {
    tex_t color={.magic=MAGIC('T','E','X',0),.serial=0x8150,.d={.width=8,.height=6,.depth=1,.samples=1,.levels=1,
        .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8}};
    tex_t depth=color; depth.serial++; depth.d.format=NVN_FORMAT_DEPTH32F;
    nvn_tex_layout(&color.d,&color.L); nvn_tex_layout(&depth.d,&depth.L);
    const float refs[]={0,.3125f,.5f,1,NAN,.5f}, width=8;
    for (unsigned bound=0; bound<2; ++bound) {
        for (unsigned f=1; f<=8; ++f) {
            uint8_t (*pixels)[8][4]=(void *)((uint8_t *)write_memory+256); memset(pixels,0,192);
            float (*z)[8]=(void *)((uint8_t *)write_memory+512); memset(z,0,192);
            uint32_t *writes=write_memory; *writes=0;
            list_t list={0};
            RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&color},.depth=&depth);
            RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,8,6}});
            RECORD(OP_SCISSOR,p_scissor,.w=8,.h=6);
            RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15,.rgba={0,0,1,1});
            RECORD(OP_CLEAR_DS,p_clear_ds,.depth=1,.depth_mask=1);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&alpha_program[bound],.stages=3);
            if (bound) {
                RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)write_memory+2048,.size=sizeof width);
                RECORD(OP_BIND_SSBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)writes,.size=sizeof *writes);
            }
            p_bind_state state={.kind=5};
            ds_st ds={.dt=1,.dw=1,.df=NVN_COMPARE_FUNC_LESS};
            memcpy(state.bytes,&ds,sizeof ds); put(&list,OP_BIND_STATE,&state,sizeof state);
            state.kind=1;
            color_st cs={.logic_op=NVN_LOGIC_OP_COPY,.alpha_func=f};
            memcpy(state.bytes,&cs,sizeof cs); put(&list,OP_BIND_STATE,&state,sizeof state);
            for (int row=0; row<6; ++row) {
                RECORD(OP_ALPHA_REF,p_alpha_ref,.ref=refs[row]);
                RECORD(OP_SCISSOR,p_scissor,.y=row,.w=8,.h=1);
                RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            }
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&color,.src=(uintptr_t)pixels,.r={.width=8,.height=6,.depth=1});
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&depth,.src=(uintptr_t)z,.r={.width=8,.height=6,.depth=1});
            put(&list,OP_END,NULL,0);
            mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for (int row=0; row<6; ++row) for (int x=0; x<8; ++x) {
                float alpha=(x+.5f)/8;
                int pass=alpha_passes(f,alpha,refs[row]);
                uint8_t *p=pixels[row][x];
                if (p[0]!=(pass?255:0) || p[2]!=(pass?0:255) || z[row][x]!=((bound||pass)?.25f:1))
                    die("alpha bound %u func %u ref %g pixel %d: red %u blue %u depth %g expected pass %d",
                        bound,f,refs[row],x,p[0],p[2],z[row][x],pass);
            }
            if (bound) assert(*writes==48);
        }
    }
    // ColorState NEVER also reaches integer-only passes. All integer
    // channels must still be written, even when the reference rejects floats.
    tex_t integer=color; integer.serial=0x8152; integer.d.format=NVN_FORMAT_RGBA8UI;
    nvn_tex_layout(&integer.d,&integer.L); list_t list={0};
    RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&integer});
    RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,8,6}});
    RECORD(OP_SCISSOR,p_scissor,.w=8,.h=6);
    RECORD(OP_CLEAR_COLOR,p_clear_color,.mask=15);
    RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&alpha_program[2],.stages=3);
    p_bind_state state={.kind=1}; color_st cs={.logic_op=NVN_LOGIC_OP_COPY,.alpha_func=NVN_ALPHA_FUNC_NEVER};
    memcpy(state.bytes,&cs,sizeof cs); put(&list,OP_BIND_STATE,&state,sizeof state);
    RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
    uint8_t (*pixels)[4]=(void *)((uint8_t *)write_memory+1024);
    RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&integer,.src=(uintptr_t)pixels,.r={.width=8,.height=6,.depth=1});
    put(&list,OP_END,NULL,0); mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    for (int i=0;i<48;++i) assert(!memcmp(pixels[i],(uint8_t[]){17,32,64,128},4));
    mtl_tex_release(&integer);
    mtl_tex_release(&color); mtl_tex_release(&depth);
    puts("Vulkan alpha tests: 768 ramp pixels, NaN, early/late depth, storage writes passed");
}

int __wrap_main(void) {
    setenv("DM_VK_NOPREWARM","1",1);setenv("DM_VK_ASYNC_PIPE","0",1);
    mtl_init();mtl_set_depth_zero(1);mtl_set_origin_lower_left(0);
    write_memory=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(write_memory!=MAP_FAILED);
    *(float *)((uint8_t *)write_memory+2048)=8;mtl_pool_add(write_memory,65536);alpha_references();mtl_finish();mtl_pool_del(write_memory);munmap(write_memory,65536);return 0;
}
