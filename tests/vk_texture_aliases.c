#include "../nxnative/dm/gfx/nvn/hle_nvn.c"
#include "../nxnative/dm/android/gfx_vk.h"
#define API(name) ((NVNT_##name)h_nvnBootstrapLoader(#name))
static NVNprogram alias_program;
#include "fixtures/recorded_texture_srgb.h"
int __wrap_prog_key(const prog_t *p,unsigned stage,uint8_t key[8]) {
    (void)p;if(stage>=2)return 0;memset(key,0,8);key[7]=stage?0x72:0x71;return 1;
}
static void check_packaged_identity(NVNdevice *device) {
    static _Alignas(16384) uint8_t storage[0x10000],seed[0x10000];
    memset(seed,0x80,sizeof seed);
    NVNmemoryPoolBuilder pb;NVNmemoryPool pool;
    API(nvnMemoryPoolBuilderSetDefaults)(&pb);API(nvnMemoryPoolBuilderSetDevice)(&pb,device);
    API(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    API(nvnMemoryPoolBuilderSetStorage)(&pb,storage,sizeof storage);assert(API(nvnMemoryPoolInitialize)(&pool,&pb));
    NVNtextureBuilder tb;NVNtexture objects[3];
    API(nvnTextureBuilderSetDefaults)(&tb);API(nvnTextureBuilderSetDevice)(&tb,device);
    API(nvnTextureBuilderSetTarget)(&tb,NVN_TEXTURE_TARGET_2D);API(nvnTextureBuilderSetSize2D)(&tb,8,8);
    for(int first=0;first<2;first++) for(int plain_first=0;plain_first<2;plain_first++) {
        // Separate storage keeps each creation/lookup order independent.
        API(nvnTextureBuilderSetStorage)(&tb,&pool,(first*2+plain_first)*0x4000);
        NVNformat aformat=first?NVN_FORMAT_RGBA8_SRGB:NVN_FORMAT_RGBA8;
        NVNformat bformat=first?NVN_FORMAT_RGBA8:NVN_FORMAT_RGBA8_SRGB;
        if(plain_first) {
            API(nvnTextureBuilderSetFormat)(&tb,bformat);
            API(nvnTextureBuilderSetPackagedTextureData)(&tb,NULL);assert(API(nvnTextureInitialize)(&objects[2],&tb));
        }
        API(nvnTextureBuilderSetFormat)(&tb,aformat);
        API(nvnTextureBuilderSetPackagedTextureData)(&tb,seed);assert(API(nvnTextureInitialize)(&objects[0],&tb));
        API(nvnTextureBuilderSetFormat)(&tb,bformat);assert(API(nvnTextureInitialize)(&objects[1],&tb));
        if(!plain_first) {
            API(nvnTextureBuilderSetPackagedTextureData)(&tb,NULL);assert(API(nvnTextureInitialize)(&objects[2],&tb));
        }
        assert(!((tex_t*)&objects[0])->has_layout && !((tex_t*)&objects[1])->has_layout);
        mtl_sub_begin();
        vkimg_t *gpu=plain_first?vk_tex((tex_t*)&objects[2]):NULL;
        vkimg_t *a=vk_tex((tex_t*)&objects[0]),*b=vk_tex((tex_t*)&objects[1]);
        if(!plain_first)gpu=vk_tex((tex_t*)&objects[2]);
        assert(a && b && gpu && a!=b && b==gpu);
        assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
        for(int i=0;i<3;i++)API(nvnTextureFinalize)(&objects[i]);
    }
    API(nvnMemoryPoolFinalize)(&pool);
    puts("PASS: packaged/plain same-format peers share in both creation orders; new format peers stay separate");
}
int __wrap_main(void) {
    void crash_install(void);crash_install();
    setenv("DM_VK_ASYNC_PIPE","0",1);setenv("DM_VK_NOPREWARM","1",1);
    NVNdeviceBuilder db;NVNdevice device;
    API(nvnDeviceBuilderSetDefaults)(&db);assert(API(nvnDeviceInitialize)(&device,&db));
    NVNqueueBuilder qb;NVNqueue queue;
    API(nvnQueueBuilderSetDefaults)(&qb);API(nvnQueueBuilderSetDevice)(&qb,&device);assert(API(nvnQueueInitialize)(&queue,&qb));
    assert(API(nvnProgramInitialize)(&alias_program,&device));((prog_t *)&alias_program)->nstage=2;
    if(getenv("NX2A_ALIAS_PACKAGED"))check_packaged_identity(&device);
    else {check_texture_srgb_aliases(&device,&queue);puts("PASS: 4096 pixels across 8 format pairs, both writer orders and cached alternating writes");}
    API(nvnProgramFinalize)(&alias_program);API(nvnQueueFinalize)(&queue);API(nvnDeviceFinalize)(&device);
    return 0;
}
