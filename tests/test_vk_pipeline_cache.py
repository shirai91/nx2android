"""Exercise the real disk-cache boundary with a controlled Vulkan driver."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from host_cc import ubsan

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


@unittest.skipUnless(shutil.which('cc') and os.name != 'nt', 'POSIX C compiler required')
class PipelineCache(unittest.TestCase):
    def test_disk_cache_boundary(self):
        helper = DM / 'android/gfx_vk_pipeline_cache.inc'
        self.assertTrue(helper.is_file(), 'pipeline cache must validate persisted products')
        code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>
static struct { VkDevice dev; VkPhysicalDevice pd; VkPhysicalDeviceProperties props; VkPipelineCache pcache; } V;
static unsigned char payload[64];
static size_t loaded;
static int reject, creates, incomplete, fail_rename, fail_sync;
static VkResult create(VkDevice d, const VkPipelineCacheCreateInfo *p, const VkAllocationCallbacks *a, VkPipelineCache *c) {
    (void)d; (void)a; creates++; loaded=p->initialDataSize;
    if (loaded) assert(!memcmp(p->pInitialData, payload, loaded));
    if (reject && loaded) return VK_ERROR_UNKNOWN;
    *c=(VkPipelineCache)(uintptr_t)1; return VK_SUCCESS;
}
static VkResult get(VkDevice d, VkPipelineCache c, size_t *n, void *p) {
    (void)d; (void)c;
    if (!p) { *n=sizeof payload; return VK_SUCCESS; }
    assert(*n>=sizeof payload); memcpy(p,payload,sizeof payload); *n=sizeof payload;
    return incomplete ? VK_INCOMPLETE : VK_SUCCESS;
}
static void properties(VkPhysicalDevice d, VkPhysicalDeviceProperties2 *p) {
    (void)d; VkPhysicalDeviceIDProperties *ids=p->pNext;
    memset(ids->deviceUUID, 5, VK_UUID_SIZE); memset(ids->driverUUID, 6, VK_UUID_SIZE);
}
static int rename_test(const char *a,const char *b) { return fail_rename ? -1 : rename(a,b); }
static int sync_test(int fd) { return fail_sync ? -1 : fsync(fd); }
#define vkCreatePipelineCache create
#define vkGetPipelineCacheData get
#define vkGetPhysicalDeviceProperties2 properties
#define rename rename_test
#define fsync sync_test
#define VKLOG(...) ((void)0)
#define VKCHK(x) assert((x)==VK_SUCCESS)
#include "gfx_vk_pipeline_cache.inc"
static void disk(const void *data,size_t n) { FILE *f=fopen(g_pcache_path,"wb"); assert(f); assert(fwrite(data,1,n,f)==n); assert(!fclose(f)); }
static void *save_thread(void *unused) { (void)unused; vk_pipeline_cache_save(); return NULL; }
int main(int argc,char **argv) {
    assert(argc==2); setenv("DM_ROOT",argv[1],1);
    V.props.vendorID=123; V.props.deviceID=456; V.props.driverVersion=7; V.props.apiVersion=VK_API_VERSION_1_1;
    memset(V.props.pipelineCacheUUID,9,VK_UUID_SIZE);
    uint32_t h[4]={32,VK_PIPELINE_CACHE_HEADER_VERSION_ONE,123,456};
    memcpy(payload,h,sizeof h); memcpy(payload+16,V.props.pipelineCacheUUID,16);
    pcache_load(); assert(loaded==0);
    vk_pipeline_cache_changed(); vk_pipeline_cache_save();
    unsigned char original[sizeof(pcache_header)+sizeof payload];
    FILE *f=fopen(g_pcache_path,"rb"); assert(f); assert(fread(original,1,sizeof original,f)==sizeof original); fclose(f);
    pcache_load(); assert(loaded==sizeof payload);
    // Any identity or payload change, truncation, trailing bytes, or old raw blob must stay out of the driver.
    for(size_t i=0;i<sizeof original;i++) {
        unsigned char bad[sizeof original]; memcpy(bad,original,sizeof bad); bad[i]^=1;
        disk(bad,sizeof bad); pcache_load(); assert(loaded==0);
    }
    for(size_t n=0;n<sizeof original;n++) { disk(original,n); pcache_load(); assert(loaded==0); }
    disk(payload,sizeof payload); pcache_load(); assert(loaded==0);
    disk(original,sizeof original); f=fopen(g_pcache_path,"ab"); fputc(1,f); fclose(f); pcache_load(); assert(loaded==0);
    disk(original,sizeof original); reject=1; creates=0; pcache_load(); assert(loaded==0 && creates==2); reject=0;
    disk(original,sizeof original); setenv("DM_VK_NOPCACHE","1",1); pcache_load(); assert(loaded==0); unsetenv("DM_VK_NOPCACHE");
    // Valid digest cannot excuse a driver header for a different device.
    unsigned char bad[sizeof original]; memcpy(bad,original,sizeof bad); bad[sizeof(pcache_header)+8]^=1;
    CC_SHA256(bad+sizeof(pcache_header),sizeof payload,((pcache_header*)bad)->digest);
    disk(bad,sizeof bad); pcache_load(); assert(loaded==0);
    disk(original,sizeof original);
    // Same-sized changed content is saved, and failed snapshots/writes remain dirty for a later retry.
    payload[63]=42; vk_pipeline_cache_changed(); incomplete=1; vk_pipeline_cache_save();
    f=fopen(g_pcache_path,"rb"); assert(f); assert(fread(bad,1,sizeof bad,f)==sizeof bad); fclose(f); assert(!memcmp(bad,original,sizeof bad));
    incomplete=0; fail_sync=1; vk_pipeline_cache_save(); fail_sync=0; fail_rename=1; vk_pipeline_cache_save();
    f=fopen(g_pcache_path,"rb"); assert(f); assert(fread(bad,1,sizeof bad,f)==sizeof bad); fclose(f); assert(!memcmp(bad,original,sizeof bad));
    fail_rename=0;
    pthread_t a,b; assert(!pthread_create(&a,NULL,save_thread,NULL)); assert(!pthread_create(&b,NULL,save_thread,NULL));
    pthread_join(a,NULL); pthread_join(b,NULL); pcache_load(); assert(loaded==sizeof payload);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_pcache_') as tmp:
            path = Path(tmp); (path/'test.c').write_text(code)
            flags = ['-fsanitize=undefined', '-fno-sanitize-recover=all'] if ubsan() else []
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-pthread',*flags,
                            '-I'+str(DM/'third_party/vulkan-headers/include'),'-I'+str(DM/'android'),
                            '-I'+str(DM/'android/compat/include'),str(path/'test.c'),str(DM/'android/compat/sha256.c'),
                            '-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test'),tmp],check=True)
            self.assertEqual(sorted(p.name for p in path.glob('vk_pipeline_cache.bin*')), ['vk_pipeline_cache.bin'])


if __name__ == '__main__': unittest.main()
