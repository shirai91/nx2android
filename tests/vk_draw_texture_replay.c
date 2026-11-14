// Public fixture from nxnative 1134e0e75; target host supplies its runtime.
#include "../nxnative/dm/gfx/nvn/hle_nvn.c"
#include "../nxnative/dm/android/gfx_vk.h"
// Verify the production pool allocator switches before the driver's capacity,
// even if a driver would accept an excessive allocation (the Adreno failure).
static PFN_vkCreateDescriptorSetLayout real_create_layout;
static PFN_vkAllocateDescriptorSets real_allocate_sets;
static PFN_vkResetDescriptorPool real_reset_pool;
static struct { VkDescriptorSetLayout layout; unsigned samplers; } budgets[32];
static struct { VkDescriptorPool pool; unsigned used; } pool_budgets[32];
static unsigned nbudgets,npool_budgets;
static VkResult checked_create_layout(VkDevice dev,const VkDescriptorSetLayoutCreateInfo *info,const VkAllocationCallbacks *allocator,VkDescriptorSetLayout *out) {
    VkResult r=real_create_layout(dev,info,allocator,out);
    if(!r) {
        unsigned cost=0; for(unsigned i=0;i<info->bindingCount;i++)
            if(info->pBindings[i].descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) cost+=info->pBindings[i].descriptorCount;
        unsigned i=0;while(i<nbudgets && budgets[i].layout!=*out)i++;
        assert(i<32);if(i==nbudgets)nbudgets++;
        budgets[i].layout=*out;budgets[i].samplers=cost;
    }
    return r;
}
static VkResult checked_allocate_sets(VkDevice dev,const VkDescriptorSetAllocateInfo *info,VkDescriptorSet *sets) {
    unsigned cost=0;
    for(unsigned i=0;i<info->descriptorSetCount;i++) for(unsigned j=0;j<nbudgets;j++)
        if(info->pSetLayouts[i]==budgets[j].layout) cost+=budgets[j].samplers;
    unsigned i=0;while(i<npool_budgets && pool_budgets[i].pool!=info->descriptorPool)i++;
    assert(i<32);if(i==npool_budgets){npool_budgets++;pool_budgets[i].pool=info->descriptorPool;}
    if(pool_budgets[i].used+cost>65536) die("DrawTexture exceeded a mixed descriptor pool: %u + %u",pool_budgets[i].used,cost);
    VkResult r=real_allocate_sets(dev,info,sets);if(!r)pool_budgets[i].used+=cost;return r;
}
static VkResult checked_reset_pool(VkDevice dev,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags) {
    VkResult r=real_reset_pool(dev,pool,flags);
    if(!r)for(unsigned i=0;i<npool_budgets;i++)if(pool_budgets[i].pool==pool)pool_budgets[i].used=0;
    return r;
}
static VkDescriptorSetLayout padding_layouts[2];
VkDescriptorSet __real_vk_alloc_set(VkDescriptorSetLayout layout);
VkDescriptorSet __wrap_vk_alloc_set(VkDescriptorSetLayout layout) {
    static int primed;
    if(!primed++) {
        void vk_dsl_register(VkDescriptorSetLayout,const VkDescriptorSetLayoutBinding *,uint32_t);
        for(unsigned j=0;j<2;j++) {
            VkDescriptorSetLayoutBinding b={0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,j?256:384,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
            VkDescriptorSetLayoutCreateInfo info={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,1,&b};
            assert(!vkCreateDescriptorSetLayout(V.dev,&info,NULL,&padding_layouts[j]));
            vk_dsl_register(padding_layouts[j],&b,1);
            for(unsigned k=0;k<(j?1:170);k++)assert(__real_vk_alloc_set(padding_layouts[j]));
        }
        // 170*384+256 = 65536 combined samplers, only 171/4096 sets.
    }
    return __real_vk_alloc_set(layout);
}
static void check_descriptor_budgets(void) {
    mtl_init();
    real_create_layout=vkCreateDescriptorSetLayout;vkCreateDescriptorSetLayout=checked_create_layout;
    real_allocate_sets=vkAllocateDescriptorSets;vkAllocateDescriptorSets=checked_allocate_sets;
    real_reset_pool=vkResetDescriptorPool;vkResetDescriptorPool=checked_reset_pool;
}
// Reuse of a sampler slot between recording and submission must keep the
// same existing nx2android sampler history semantics as ordinary texture binds.
static NVNsamplerPool *first_sampler_pool;
static NVNsamplerBuilder first_sampler;
static int sampler_rewritten;
static void register_sampler(NVNsamplerPool *pool, int id, const NVNsamplerBuilder *builder) {
    if (!first_sampler_pool && id == 256) { first_sampler_pool=pool; first_sampler=*builder; }
    ((NVNT_nvnSamplerPoolRegisterSamplerBuilder)h_nvnBootstrapLoader("nvnSamplerPoolRegisterSamplerBuilder"))(pool,id,builder);
}
static void submit_with_sampler_rewrite(NVNqueue *queue, int n, const NVNcommandHandle *handles) {
    if (!sampler_rewritten++) {
        h_nvnSamplerBuilderSetMinMagFilter(&first_sampler,NVN_MIN_FILTER_LINEAR,NVN_MAG_FILTER_LINEAR);
        h_nvnSamplerPoolRegisterSamplerBuilder(first_sampler_pool,256,&first_sampler);
    }
    ((NVNT_nvnQueueSubmitCommands)h_nvnBootstrapLoader("nvnQueueSubmitCommands"))(queue,n,handles);
}
static void *fixture_api(const char *name) {
    if (!strcmp(name,"nvnSamplerPoolRegisterSamplerBuilder")) return register_sampler;
    if (!strcmp(name,"nvnQueueSubmitCommands")) return submit_with_sampler_rewrite;
    return h_nvnBootstrapLoader(name);
}
#define API(name) ((NVNT_##name)fixture_api(#name))
#define DM_VK_PUBLIC_BINDINGS 1
#include "fixtures/recorded_draw_texture.h"
// Synthetic guest shader keys: production program selection and descriptor replay
// remain in the linked backend; no game binaries are needed.
int __wrap_prog_key(const prog_t *p, unsigned stage, uint8_t key[8]) {
    (void)p; if (stage >= 2) return 0; memset(key,0,8); key[7] = stage ? 0x62 : 0x52; return 1;
}
static void check_guest_draw_after_texture(NVNdevice *device, NVNqueue *queue) {
    static _Alignas(16384) uint8_t bytes[0x10000];
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb); API(nvnMemoryPoolBuilderSetDevice)(&pb,device);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,bytes,sizeof bytes);
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    assert(API(nvnMemoryPoolInitialize)(&pool,&pb));
    NVNbufferAddress address=API(nvnMemoryPoolGetBufferAddress)(&pool);
    NVNtexture images[3]; NVNtextureBuilder tb;
    API(nvnTextureBuilderSetDefaults)(&tb); API(nvnTextureBuilderSetDevice)(&tb,device);
    API(nvnTextureBuilderSetTarget)(&tb,NVN_TEXTURE_TARGET_2D); API(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);
    API(nvnTextureBuilderSetSize2D)(&tb,8,6);
    for(int i=0;i<3;i++) {
        API(nvnTextureBuilderSetStorage)(&tb,&pool,0x2000+0x1000*i);
        assert(API(nvnTextureInitialize)(&images[i],&tb));
    }
    NVNtexturePool tp; NVNsamplerPool sp; NVNsamplerBuilder sb;
    assert(API(nvnTexturePoolInitialize)(&tp,&pool,0x6000,257));
    assert(API(nvnSamplerPoolInitialize)(&sp,&pool,0xa000,257));
    API(nvnTexturePoolRegisterTexture)(&tp,256,&images[0],NULL);
    API(nvnSamplerBuilderSetDefaults)(&sb); API(nvnSamplerPoolRegisterSamplerBuilder)(&sp,256,&sb);
    const NVNtexture *targets[]={&images[1],&images[2]};
    NVNchannelMaskState masks; API(nvnChannelMaskStateSetDefaults)(&masks);
    API(nvnChannelMaskStateSetChannelMask)(&masks,0,1,0,1,0); API(nvnChannelMaskStateSetChannelMask)(&masks,1,0,1,0,1);
    float *uniform=(void *)(bytes+0x5500); for(int c=0;c<4;c++) uniform[c]=(17.f*(c+1))/255.f;
    memset(bytes+0x5000,255,8*6*4);
    NVNcopyRegion full={0,0,0,8,6,1};
    NVNprogram program; assert(API(nvnProgramInitialize)(&program,device)); ((prog_t *)&program)->nstage=2;
    NVNcommandBuffer cb; assert(API(nvnCommandBufferInitialize)(&cb,device));
    API(nvnCommandBufferAddCommandMemory)(&cb,&pool,0,0x2000); API(nvnCommandBufferBeginRecording)(&cb);
    API(nvnCommandBufferCopyBufferToTexture)(&cb,address+0x5000,&images[0],NULL,&full,0);
    API(nvnCommandBufferSetRenderTargets)(&cb,2,targets,NULL,NULL,NULL);
    API(nvnCommandBufferSetScissor)(&cb,0,0,8,6);
    for(int i=0;i<2;i++) API(nvnCommandBufferClearColor)(&cb,i,(float[]){0,0,0,0},15);
    API(nvnCommandBufferBindChannelMaskState)(&cb,&masks);
    API(nvnCommandBufferSetViewport)(&cb,1,1,2,2);
    API(nvnCommandBufferBindProgram)(&cb,&program,3);
    API(nvnCommandBufferBindUniformBuffer)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,address+0x5500,16);
    API(nvnCommandBufferDrawArrays)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
    API(nvnCommandBufferSetTexturePool)(&cb,&tp); API(nvnCommandBufferSetSamplerPool)(&cb,&sp);
    API(nvnCommandBufferDrawTexture)(&cb,API(nvnDeviceGetTextureHandle)(device,256,256),
        &(NVNdrawTextureRegion){0,0,8,6},&(NVNdrawTextureRegion){0,0,8,6});
    API(nvnCommandBufferDrawArrays)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
    for(int i=0;i<2;i++) API(nvnCommandBufferCopyTextureToBuffer)(&cb,targets[i],NULL,&full,address+0x5600+i*256,0);
    NVNcommandHandle list=API(nvnCommandBufferEndRecording)(&cb);
    API(nvnQueueSubmitCommands)(queue,1,&list); API(nvnQueueFinish)(queue);
    for(int i=0;i<2;i++) for(int y=0;y<6;y++) for(int x=0;x<8;x++) for(int c=0;c<4;c++) {
        int expected=(c&1)!=i?0:x>=1&&x<3&&y>=1&&y<3?17*(c+1):255;
        int actual=bytes[0x5600+i*256+4*(8*y+x)+c];
        if(actual!=expected) die("guest draw after DrawTexture target %d at %d,%d channel %d: %d expected %d",i,x,y,c,actual,expected);
    }
    API(nvnCommandBufferFinalize)(&cb); API(nvnProgramFinalize)(&program);
    API(nvnTexturePoolFinalize)(&tp); API(nvnSamplerPoolFinalize)(&sp);
    for(int i=0;i<3;i++) API(nvnTextureFinalize)(&images[i]); API(nvnMemoryPoolFinalize)(&pool);
    puts("DrawTexture preserved guest MRT, channel masks, viewport, program and uniform binding");
}
int __wrap_main(void) {
    void crash_install(void); crash_install();
    setenv("DM_VK_ASYNC_PIPE","0",1); setenv("DM_VK_NOPREWARM","1",1);
    check_descriptor_budgets();
    NVNdevice device;
    NVNdeviceBuilder db;
    API(nvnDeviceBuilderSetDefaults)(&db);
    assert(API(nvnDeviceInitialize)(&device, &db));
    NVNqueue queue;
    NVNqueueBuilder qb;
    API(nvnQueueBuilderSetDevice)(&qb, &device);
    API(nvnQueueBuilderSetDefaults)(&qb);
    assert(API(nvnQueueInitialize)(&queue, &qb));
    check_draw_texture(&device, &queue);
    API(nvnDeviceSetWindowOriginMode)(&device,NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
    check_guest_draw_after_texture(&device,&queue);
    API(nvnQueueFinalize)(&queue);
    API(nvnDeviceFinalize)(&device);
    return 0;
}
