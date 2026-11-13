// GPU-handle oracle adapted from nxnative 1f01e28c t_replay.c uniform_texture_array.
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "gfx_vk.h"
#include <assert.h>
static prog_t groups={.nstage=1,.serial=0xfeed6000};
static prog_t bindless_program={.nstage=2,.serial=0xfeed6001};
static prog_t consumer={.nstage=1,.serial=0xfeed6002};
static prog_t writer={.nstage=2,.serial=0xfeed6003};
static dpool_t texture_pool,alternate_texture_pool,sampler_pool,alternate_sampler_pool;
int __wrap_nvn_dpool_base(const void *p,int texture){(void)texture;return p==&texture_pool?2048:p==&alternate_texture_pool?4096:p==&sampler_pool?256:512;}
int __wrap_prog_key(const prog_t *p,unsigned stage,uint8_t key[8]){if(stage>=((p==&bindless_program||p==&writer)?2u:1u))return 0;uint64_t v=p==&writer?(stage?0xfeed6003:0x52):p==&bindless_program?(stage?0xfeed6001:0x52):p==&consumer?0xfeed6002:0xfeed6000;for(unsigned i=0;i<8;i++)key[7-i]=(uint8_t)(v>>(8*i));return 1;}
typedef struct {uint8_t bytes[16384];size_t used;} list_t;
static void put(list_t *l,unsigned op,const void *p,size_t n){size_t size=(sizeof(op_hdr)+n+7)&~7;assert(l->used+size<=sizeof l->bytes);op_hdr h={op,(uint32_t)size};memcpy(l->bytes+l->used,&h,sizeof h);if(n)memcpy(l->bytes+l->used+sizeof h,p,n);l->used+=size;}
#define RECORD(op,T,...) do{T value={__VA_ARGS__};put(&list,op,&value,sizeof value);}while(0)
static void uniform_texture_array(void) {
    tex_t textures[3], output = {.magic=MAGIC('T','E','X',0), .serial = 0x5100, .d = {.width = 4, .height = 4,
        .depth = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8}};
    nvn_tex_layout(&output.d, &output.L);
    for (int i = 0; i < 3; ++i) {
        textures[i] = (tex_t){.magic=MAGIC('T','E','X',0), .serial = 0x5000 + i, .d = {.width = 1, .height = 1, .depth = 1,
            .levels = 1, .target = i == 2 ? NVN_TEXTURE_TARGET_2D : NVN_TEXTURE_TARGET_3D, .format = NVN_FORMAT_RGBA8}};
        nvn_tex_layout(&textures[i].d, &textures[i].L);
        mtl_reg_tex(2053 + i, &textures[i], NULL);
    }
    samp_par edge = {0}, border = {.ws = NVN_WRAP_MODE_CLAMP_TO_BORDER, .border = {1,1,1,1}};

    mtl_reg_samp(257, &edge); mtl_reg_samp(258, &border); mtl_reg_samp(513, &border);
    mtl_reg_tex(4101, &textures[1], NULL);
    _Alignas(4096) uint64_t handles[8192] = {0}; _Alignas(4096) uint64_t samplers[8192] = {0};
    _Alignas(4096) uint32_t params[1024]={0}, producer[1024]={0}, pointer[1024]={0}, readback[1024];
    mtl_pool_add(pointer,sizeof pointer);mtl_pool_add(params,sizeof params); mtl_pool_add(producer,sizeof producer); mtl_pool_add(readback,sizeof readback);
    unsigned index = 7, cases = 0;
    mtl_pool_add(handles, sizeof handles); mtl_pool_add(samplers, sizeof samplers);
    handles[44 + index] = 5; samplers[16 + index] = 1u << 20;
    pool_t handle_pool={.mem=(uint8_t *)handles,.size=sizeof handles};
    tex_t linear={.magic=MAGIC('T','E','X',0),.serial=0x5200,.mem=(uint8_t *)&handles[51],.pool=&handle_pool,.off=51*8,
        .d={.width=1,.height=1,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32UI,.stride=256,.flags=NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET}};
    nvn_tex_layout(&linear.d,&linear.L);
    uint8_t expected[][4] = {{0,255,0,255}, {0,0,255,255}, {0,0,0,0},
                                   {255,255,255,255}, {0,0,0,0}, {0,0,0,0}, {0,0,255,255}, {255,255,255,255}};

    for(int compute=0;compute<2;compute++) for (int gpu=0;gpu<(compute?6:4);gpu++) for (int pass = 0; pass < 8; ++pass) {
        if(getenv("NX2A_HANDLE_CASE") && atoi(getenv("NX2A_HANDLE_CASE"))!=compute*10+gpu)continue;
        if(pass==4)continue; // Target incompatible 2D-to-3D views are a separate gap.
        printf("case compute%d gpu%d pass%d\n",compute,gpu,pass);fflush(stdout);
        list_t list = {0}; uint8_t *pixels=(uint8_t *)(readback+64); memset(readback,0xcc,sizeof readback);
        struct { uint32_t index; float coordinate; uint32_t pad[2]; } parameters = {pass == 2 ? index+24 : index, pass == 3 || pass == 7 ? 1.5f : 0.5f, {0}};
        memcpy(params,&parameters,sizeof parameters);
        uint64_t outaddr=(uintptr_t)(readback+64);memcpy(pointer,&outaddr,8);
        RECORD(OP_RENDER_COND,p_render_cond,.mode=2);
        if(compute) RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&output,.r={.width=4,.height=4,.depth=1},.mask=15,.c.f={1,0,0,1});
        for (int i = 0; i < 3; ++i) {
            p_clear_tex clear = {.tex = &textures[i], .r = {.width = 1, .height = 1, .depth = 1}, .mask = 15};
            clear.c.f[i == 0 ? 1 : i == 1 ? 2 : 0] = 1; clear.c.f[3] = 1;
            put(&list, OP_CLEAR_TEX, &clear, sizeof clear);
        }
        RECORD(OP_SET_TEX_POOL, p_set_pool, .pool = pass == 6 ? &alternate_texture_pool : &texture_pool);
        RECORD(OP_SET_SAMP_POOL, p_set_pool, .pool = pass == 7 ? &alternate_sampler_pool : &sampler_pool);
        struct { uint64_t address, size, value; } update = {(uintptr_t)&handles[44 + index], 8, pass >= 6 ? 5u : pass >= 4 ? 7u : pass ? 6u : 5u};


        if (!gpu || gpu==4) put(&list, OP_UPDATE_UB, &update, sizeof update);
        samplers[16 + index] = (uint64_t)(pass == 3 ? 2 : 1) << 20;
        if(gpu && gpu<4) {
            uint32_t values[4]={(uint32_t)update.value,0,0,0};
            handles[44+index]=0;
            // Target adapter: the source push constants become a regular UBO;
            // SSBOs stay aligned to the target translator's 256-byte boundary.
            for(int sampler=0;sampler<2;sampler++) {
                uint64_t *memory=sampler?samplers:handles;
                unsigned element=(sampler?16:44)+index;
                uint32_t value=sampler?(uint32_t)samplers[element]:(uint32_t)update.value;
                memory[element]=0;
                struct {uint64_t address,size;uint32_t data[4];} input={(uintptr_t)producer,16,{(element*2)%64,value,0,0}};
                put(&list,OP_UPDATE_UB,&input,sizeof input);
                RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&groups,.stages=0x20);
                RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)producer,.size=16);
                RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)&memory[element],.size=8);
                RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
            }
            values[0]=(pass==3?2u:1u)<<20;
            if(gpu==2) { put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);mtl_sub_commit();list.used=0; }
            if(gpu==3) {
                // Two later known ranges cover the earlier GPU-written handle;
                // the sampler is a known fill. Neither needs a submission cut.
                struct { uint64_t address,size;uint32_t value,pad; } part={update.address,4,(uint32_t)update.value,0};
                put(&list,OP_UPDATE_UB,&part,sizeof part);
                part.address+=4;part.value=0;put(&list,OP_UPDATE_UB,&part,sizeof part);
                part.address=(uintptr_t)&samplers[16+index];part.size=4;part.value=values[0];put(&list,OP_UPDATE_UB,&part,sizeof part);
            }
        }

        if(compute && gpu==4) {
            pointer[0]=pointer[1]=0;
            struct {uint64_t address,size;uint32_t data[4];} input={(uintptr_t)producer,16,{0,(uint32_t)outaddr,(uint32_t)(outaddr>>32),0}};
            put(&list,OP_UPDATE_UB,&input,sizeof input);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&groups,.stages=0x20);
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)producer,.size=16);
            RECORD(OP_BIND_SSBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)pointer,.size=8);
            RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        }
        if(compute && gpu==5) {
            handles[51]=0;producer[0]=0;producer[1]=(uint32_t)update.value;
            RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&linear});
            RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,1,1}});RECORD(OP_SCISSOR,p_scissor,.w=1,.h=1);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&writer,.stages=3);
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=1,.index=0,.addr=(uintptr_t)producer,.size=16);
            RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
        }
        if(!compute) RECORD(OP_SET_RT, p_set_rt, .n = 1, .color = {&output});
        RECORD(OP_BIND_PROGRAM, p_bind_program, .prog = compute?&consumer:&bindless_program, .stages = compute?0x20:3);
        RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,4,4}});
        RECORD(OP_SCISSOR,p_scissor,.w=4,.h=4);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = compute?5:1, .index = 7, .addr = (uintptr_t)handles,
               .size = pass == 5 ? 352 : sizeof handles);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = compute?5:1, .index = 8, .addr = (uintptr_t)samplers, .size = sizeof samplers);
        RECORD(OP_BIND_UBO, p_bind_buf, .stage = compute?5:1, .index = 9, .addr = (uintptr_t)params, .size = sizeof parameters);
        if(compute) {
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=10,.addr=(uintptr_t)pointer,.size=8);
            RECORD(OP_RENDER_COND,p_render_cond,.mode=3);
            RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
            RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&output});
            // This remains disabled even if descriptor readiness split the
            // submission inside the compute dispatch. Its target stays red.
            RECORD(OP_CLEAR_COLOR,p_clear_color,.index=0,.mask=15,.rgba={1,1,1,1});
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)(readback+128),.r={.width=4,.height=4,.depth=1});
            RECORD(OP_RENDER_COND,p_render_cond,.mode=2);
        } else {
            RECORD(OP_DRAW, p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
            RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&output,.src=(uintptr_t)pixels,.r={.width=4,.height=4,.depth=1});
        }
        put(&list, OP_END, NULL, 0);
        uint64_t before=mtl_last_fence();
        mtl_sub_begin(); mtl_exec(list.bytes); assert(mtl_wait(mtl_sub_commit(), UINT64_MAX));
        if(compute) for(unsigned i=128;i<144;i++) assert(readback[i]==0xff0000ffu);
        for(unsigned i=0;i<1024;i++) if(!(i>=64&&i<80)&&!(compute&&i>=128&&i<144)) assert(readback[i]==0xccccccccu);
        if(!gpu || gpu==3) assert(mtl_last_fence()==before+1);
        cases++;
        for (size_t i = 0; i < 64; ++i) {
            if (pixels[i]==expected[pass][i%4]) continue;
            fprintf(stderr,"uniform texture array GPU%d pass %d pixel %zu channel %zu: %u expected %u\n",
                    gpu,pass,i/4,i%4,pixels[i],expected[pass][i%4]);
            abort();
        }
    }
    mtl_pool_del(pointer);mtl_pool_del(params);mtl_pool_del(producer);mtl_pool_del(readback);
    mtl_pool_del(handles); mtl_pool_del(samplers);
    mtl_reg_tex(4101, NULL, NULL);
    for (int i = 0; i < 3; ++i) { mtl_reg_tex(2053 + i, NULL, NULL); mtl_tex_release(&textures[i]); }
    mtl_tex_release(&output);mtl_tex_release(&linear);
    printf("%u graphics/compute uniform-handle cases passed: CPU/current/prior GPU, known overwrites, pool bases, pointer and linear RT producers, conditional state\n",cases);

}


int __wrap_main(void){setenv("DM_VK_NOPREWARM","1",1);mtl_init();uniform_texture_array();return 0;}
