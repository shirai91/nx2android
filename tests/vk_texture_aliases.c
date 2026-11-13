#include "../nxnative/dm/gfx/nvn/hle_nvn.c"
#include "../nxnative/dm/android/gfx_vk.h"
#define API(name) ((NVNT_##name)h_nvnBootstrapLoader(#name))
static NVNprogram alias_program;
#include "fixtures/recorded_texture_srgb.h"
int __wrap_prog_key(const prog_t *p,unsigned stage,uint8_t key[8]) {
    (void)p;if(stage>=2)return 0;memset(key,0,8);key[7]=stage?0x72:0x71;return 1;
}
int __wrap_main(void) {
    void crash_install(void);crash_install();
    setenv("DM_VK_ASYNC_PIPE","0",1);setenv("DM_VK_NOPREWARM","1",1);
    NVNdeviceBuilder db;NVNdevice device;
    API(nvnDeviceBuilderSetDefaults)(&db);assert(API(nvnDeviceInitialize)(&device,&db));
    NVNqueueBuilder qb;NVNqueue queue;
    API(nvnQueueBuilderSetDefaults)(&qb);API(nvnQueueBuilderSetDevice)(&qb,&device);assert(API(nvnQueueInitialize)(&queue,&qb));
    assert(API(nvnProgramInitialize)(&alias_program,&device));((prog_t *)&alias_program)->nstage=2;
    check_texture_srgb_aliases(&device,&queue);
    API(nvnProgramFinalize)(&alias_program);API(nvnQueueFinalize)(&queue);API(nvnDeviceFinalize)(&device);
    puts("PASS: 4096 pixels across 8 format pairs, both writer orders and cached alternating writes");return 0;
}
