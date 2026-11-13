// Actual-Maxwell oracle ported from nxnative 1f01e28c resource_pools.inc.
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "gfx_vk.h"
#include <assert.h>
static prog_t resource_pool_program={.nstage=1,.serial=0xfeed6000};
static prog_t graphics_pool_program={.nstage=2,.serial=0xfeed6001};
static dpool_t texture_pool,alternate_texture_pool,sampler_pool,alternate_sampler_pool;
int __wrap_nvn_dpool_base(const void *p,int texture){(void)texture;return p==&texture_pool?2048:p==&alternate_texture_pool?4096:p==&sampler_pool?256:512;}
int __wrap_prog_key(const prog_t *p,unsigned stage,uint8_t key[8]){if(stage>=(p==&graphics_pool_program?2u:1u))return 0;uint64_t v=p==&graphics_pool_program?(stage?0xfeed6001:0x52):0xfeed6000;for(unsigned i=0;i<8;i++)key[7-i]=(uint8_t)(v>>(8*i));return 1;}
typedef struct {uint8_t bytes[16384];size_t used;} list_t;
static void put(list_t *l,unsigned op,const void *p,size_t n){size_t size=(sizeof(op_hdr)+n+7)&~7;assert(l->used+size<=sizeof l->bytes);op_hdr h={op,(uint32_t)size};memcpy(l->bytes+l->used,&h,sizeof h);if(n)memcpy(l->bytes+l->used+sizeof h,p,n);l->used+=size;}
#define RECORD(op,T,...) do{T value={__VA_ARGS__};put(&list,op,&value,sizeof value);}while(0)
static void resource_pools(void) {
    const unsigned texture_ids[]={1,2,31,32,301,4096,65536};
    const unsigned sampler_ids[]={1,2,31,32,127,4095};
    const unsigned queries[]={0,1,2,3,31,32,127,128,301,302,4095,4096,65535,65536,65537,0xfffff};
    tex_t textures[2];_Alignas(4096) uint32_t uniform[16384]={0};
    _Alignas(4096) uint32_t handles[8];_Alignas(4096) uint32_t output[256];
    mtl_pool_add(uniform,sizeof uniform);mtl_pool_add(handles,sizeof handles);mtl_pool_add(output,sizeof output);
    uint64_t input=(uintptr_t)handles,result=(uintptr_t)(output+64);memcpy(uniform,&input,8);memcpy(uniform+14,&result,8);
    list_t list={0};
    for (unsigned i=0;i<2;i++) {
        textures[i]=(tex_t){.magic=MAGIC('T','E','X',0),.serial=0x18100+i,.d={.width=1,.height=1,.depth=1,.levels=1,
            .target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32F}};
        nvn_tex_layout(&textures[i].d,&textures[i].L);
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&textures[i],.r={.width=1,.height=1,.depth=1},.mask=15,.c.f={i ? .75f : .25f});
    }
    put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
    tex_t target={.magic=MAGIC('T','E','X',0),.serial=0x18110,.d={.width=8,.height=1,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32F}};
    nvn_tex_layout(&target.d,&target.L);
    unsigned cases=0;
    for (unsigned pass=0;pass<4;pass++) {
        int alternate=pass>=2,empty=pass==1;
        int texture_base=alternate ? 4096 : 2048,sampler_base=alternate ? 512 : 256;
        for (unsigned i=0;i<sizeof texture_ids/sizeof *texture_ids;i++) {
            unsigned texel=(texture_ids[i]+alternate)&1;
            mtl_reg_tex(texture_base+texture_ids[i],empty ? NULL : &textures[texel],NULL);
        }
        for (unsigned i=0;i<sizeof sampler_ids/sizeof *sampler_ids;i++) {
            unsigned white=(sampler_ids[i]+pass)&1;
            samp_par sampler={.ws=NVN_WRAP_MODE_CLAMP_TO_BORDER,.wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE};
            for (unsigned c=0;c<4;c++) sampler.border[c]=(float)white;
            mtl_reg_samp(sampler_base+sampler_ids[i],&sampler);
        }
        for (unsigned test=0;test<16;test++) for (unsigned outside=0;outside<2;outside++) for(unsigned graphics=0;graphics<2;graphics++) {
            uniform[4]=outside ? 0x3fc00000 : 0x3f000000;memset(output,0xcc,sizeof output);
            for (unsigned lane=0;lane<8;lane++) {
                unsigned tid=queries[(test+lane)%16],sid=queries[(3*test+lane)%16]&4095;
                handles[lane]=tid|(sid<<20);uniform[32+lane*4]=handles[lane];
            }
            list=(list_t){0};
            RECORD(OP_SET_TEX_POOL,p_set_pool,.pool=alternate ? &alternate_texture_pool : &texture_pool);
            RECORD(OP_SET_SAMP_POOL,p_set_pool,.pool=alternate ? &alternate_sampler_pool : &sampler_pool);
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=graphics?1:5,.index=0,.addr=(uintptr_t)uniform,.size=sizeof uniform);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=graphics?&graphics_pool_program:&resource_pool_program,.stages=graphics?3:0x20);
            if(graphics) {
                RECORD(OP_SET_RT,p_set_rt,.n=1,.color={&target});
                RECORD(OP_VIEWPORTS,p_viewports,.n=1,.v={{0,0,8,1}});
                RECORD(OP_SCISSOR,p_scissor,.w=8,.h=1);
                p_bind_state state={.kind=1};color_st color={.logic_op=NVN_LOGIC_OP_COPY,.alpha_func=NVN_ALPHA_FUNC_ALWAYS};
                memcpy(state.bytes,&color,sizeof color);put(&list,OP_BIND_STATE,&state,sizeof state);
                RECORD(OP_DRAW,p_draw,.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1);
                RECORD(OP_COPY_T2B,p_copy_b2t,.tex=&target,.src=(uintptr_t)(output+64),.r={.width=8,.height=1,.depth=1});
            }else RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
            put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for (unsigned word=0;word<256;word++) {
                uint32_t expected=0xcccccccc;
                if (word>=64 && word<72) {
                    unsigned tid=handles[word-64]&0xfffff,sid=handles[word-64]>>20,real_texture=0,real_sampler=0;
                    for (unsigned i=0;i<sizeof texture_ids/sizeof *texture_ids;i++) real_texture|=tid==texture_ids[i];
                    for (unsigned i=0;i<sizeof sampler_ids/sizeof *sampler_ids;i++) real_sampler|=sid==sampler_ids[i];
                    expected=real_texture && !empty ? ((tid+alternate)&1 ? 0x3f400000 : 0x3e800000) : 0;
                    if (outside && real_sampler) expected=(sid+pass)&1 ? 0x3f800000 : 0;
                }
                if (output[word]!=expected) die("resource pool pass%u case%u outside%u graphics%u word%u returned %#x expected %#x",pass,test,outside,graphics,word,output[word],expected);
            }
            cases+=8;
        }
    }
    for (unsigned pool=0;pool<2;pool++) for (unsigned i=0;i<sizeof texture_ids/sizeof *texture_ids;i++)
        mtl_reg_tex((pool ? 4096 : 2048)+texture_ids[i],NULL,NULL);
    for (unsigned i=0;i<2;i++) mtl_tex_release(&textures[i]);
    mtl_tex_release(&target);
    mtl_pool_del(uniform);mtl_pool_del(handles);mtl_pool_del(output);
    printf("Resource pools passed %u Maxwell compute/GLSL fragment samples across sparse IDs, pool changes, empty entries and re-registration\n",cases);
}

static PFN_vkCreateDescriptorPool real_pool_create;
static VkResult check_pool_size(VkDevice d,const VkDescriptorPoolCreateInfo *ci,const VkAllocationCallbacks *a,VkDescriptorPool *p) {
    unsigned capacity=0;for(unsigned i=0;i<ci->poolSizeCount;i++)if(ci->pPoolSizes[i].type==VK_DESCRIPTOR_TYPE_SAMPLER)capacity=ci->pPoolSizes[i].descriptorCount;
    assert(capacity>=2049);return real_pool_create(d,ci,a,p);
}
static void allocation_limits(void) {
    real_pool_create=vkCreateDescriptorPool;vkCreateDescriptorPool=check_pool_size;
    VkDescriptorSetLayoutBinding b={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER,.descriptorCount=2049,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo ci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,1,&b};VkDescriptorSetLayout l;
    VKCHK(vkCreateDescriptorSetLayout(V.dev,&ci,NULL,&l));void vk_dsl_register(VkDescriptorSetLayout,const VkDescriptorSetLayoutBinding *,uint32_t);vk_dsl_register(l,&b,1);
    for(unsigned frame=0;frame<4;frame++){mtl_sub_begin();assert(vk_alloc_set(l));assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));}
    vkCreateDescriptorPool=real_pool_create;puts("oversized descriptor pool allocation and submission reuse PASS");
}
static PFN_vkCreateImageView original_view_create;
static PFN_vkUpdateDescriptorSets original_descriptor_update;
static struct{VkImageView view;VkImage image;VkFormat format;VkImageViewType type;VkComponentMapping components;}views[512];
static unsigned view_count;static VkImage observed_image;static unsigned observed_descriptor;
static VkResult inspect_view_create(VkDevice device,const VkImageViewCreateInfo *ci,const VkAllocationCallbacks *a,VkImageView *view){
    VkResult r=original_view_create(device,ci,a,view);if(!r){assert(view_count<512);views[view_count++]=(typeof(views[0])){*view,ci->image,ci->format,ci->viewType,ci->components};}return r;
}
static void inspect_descriptor_update(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t nc,const VkCopyDescriptorSet *c){
    for(unsigned i=0;i<n;i++)if(w[i].descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)for(unsigned j=0;j<w[i].descriptorCount;j++){
        VkImageView view=w[i].pImageInfo[j].imageView;
        for(unsigned k=0;k<view_count;k++)if(views[k].view==view && views[k].image==observed_image){
            assert(views[k].type==VK_IMAGE_VIEW_TYPE_2D);assert(views[k].format==VK_FORMAT_R32_SFLOAT);
            assert(views[k].components.r==VK_COMPONENT_SWIZZLE_ONE);observed_descriptor++;
        }
    }
    original_descriptor_update(d,n,w,nc,c);
}
static void sampled_view_adapters(void){
    original_view_create=vkCreateImageView;vkCreateImageView=inspect_view_create;
    original_descriptor_update=vkUpdateDescriptorSets;vkUpdateDescriptorSets=inspect_descriptor_update;
    _Alignas(4096) uint32_t uniform[256]={0},handles[8],output[256];
    mtl_pool_add(uniform,sizeof uniform);mtl_pool_add(handles,sizeof handles);mtl_pool_add(output,sizeof output);
    uint64_t input=(uintptr_t)handles,result=(uintptr_t)(output+64);memcpy(uniform,&input,8);memcpy(uniform+14,&result,8);uniform[4]=0x3f000000;
    for(unsigned i=0;i<8;i++)handles[i]=1|(1<<20);
    samp_par sampler={0};mtl_reg_samp(257,&sampler);
    int listed=getenv("DM_VK_UBWC") && atoi(getenv("DM_VK_UBWC"));
    for(unsigned test=0;test<3;test++){
        if(getenv("NX2A_POOL_VIEW_CASE") && test!=(unsigned)atoi(getenv("NX2A_POOL_VIEW_CASE")))continue;
        tex_t texture={.magic=MAGIC('T','E','X',0),.serial=0x18200+test,.set_swz=1,.swz=1|(3<<3)|(4<<6)|(5<<9),
            .d={.width=1,.height=1,.depth=1,.levels=1,.target=test==2?NVN_TEXTURE_TARGET_1D:NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R32UI}};
        view_t view={.set_swizzle=1,.swz={1,3,4,5}};
        if(test==1)texture.swz=0|(3<<3)|(4<<6)|(5<<9);
        nvn_tex_layout(&texture.d,&texture.L);list_t list={0};
        RECORD(OP_CLEAR_TEX,p_clear_tex,.tex=&texture,.r={.width=1,.height=1,.depth=1},.mask=15,.is_uint=1,.c.u={0x3f000000});
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);observed_image=vk_tex(&texture)->img;assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        observed_descriptor=0;mtl_reg_tex(2049,&texture,test==1?&view:NULL);memset(output,0xcc,sizeof output);list=(list_t){0};
        RECORD(OP_SET_TEX_POOL,p_set_pool,.pool=&texture_pool);RECORD(OP_SET_SAMP_POOL,p_set_pool,.pool=&sampler_pool);
        RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)uniform,.size=sizeof uniform);
        RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&resource_pool_program,.stages=0x20);RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
        put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        uint32_t expected=test==2 || listed?0:0x3f800000;
        for(unsigned i=0;i<256;i++)assert(output[i]==(i>=64 && i<72?expected:0xcccccccc));
        assert(observed_descriptor==(expected?1:0));mtl_reg_tex(2049,NULL,NULL);mtl_tex_release(&texture);
    }
    mtl_pool_del(uniform);mtl_pool_del(handles);mtl_pool_del(output);puts("integer texture/view swizzles, listed-format fallback and 1D mismatch PASS");
}

int __wrap_main(void){setenv("DM_VK_NOPREWARM","1",1);mtl_init();
if(getenv("NX2A_POOL_VIEW_ONLY")){sampled_view_adapters();return 0;}
if(getenv("NX2A_POOL_MAP_ONLY")){vkshader_t *a=vk_shader(0xfeed6020,5),*b=vk_shader(0xfeed6021,5);assert(!a && !b);puts("serialized internal maps rejected PASS");return 0;}
if(getenv("NX2A_POOL_ALLOC_ONLY")){allocation_limits();return 0;}
if(getenv("NX2A_POOL_LIMIT_ONLY")){V.props.limits.maxPerStageDescriptorStorageBuffers=3;assert(!vk_shader(0xfeed6000,5));puts("base pool map descriptor limit rejected PASS");return 0;}
if(getenv("NX2A_POOL_FEATURE_ONLY")){V.has_resource_pools=0;assert(!vk_shader(0xfeed6000,5));puts("unsupported pool feature rejected PASS");return 0;}
resource_pools();mtl_finish();return 0;}
