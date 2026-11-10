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
    def check_case(self, mode):
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
static int reject, creates, incomplete, fail_rename, fail_sync, reject_empty, reds, syncs, renames, queries;
static VkResult create(VkDevice d, const VkPipelineCacheCreateInfo *p, const VkAllocationCallbacks *a, VkPipelineCache *c) {
    (void)d; (void)a; creates++; loaded=p->initialDataSize;
    if (loaded) assert(!memcmp(p->pInitialData, payload, loaded));
    if ((reject && loaded) || (reject_empty && !loaded)) return VK_ERROR_UNKNOWN;
    *c=(VkPipelineCache)(uintptr_t)1; return VK_SUCCESS;
}
static VkResult get(VkDevice d, VkPipelineCache c, size_t *n, void *p) {
    (void)d; (void)c; queries++;
    if (!p) { *n=sizeof payload; return VK_SUCCESS; }
    assert(*n>=sizeof payload); memcpy(p,payload,sizeof payload); *n=sizeof payload;
    return incomplete ? VK_INCOMPLETE : VK_SUCCESS;
}
static void properties(VkPhysicalDevice d, VkPhysicalDeviceProperties2 *p) {
    (void)d; VkPhysicalDeviceIDProperties *ids=p->pNext;
    memset(ids->deviceUUID, 5, VK_UUID_SIZE); memset(ids->driverUUID, 6, VK_UUID_SIZE);
}
static int rename_test(const char *a,const char *b) { renames++; return fail_rename ? -1 : rename(a,b); }
static int sync_test(int fd) { syncs++; return fail_sync ? -1 : fsync(fd); }
#define vkCreatePipelineCache create
#define vkGetPipelineCacheData get
#define vkGetPhysicalDeviceProperties2 properties
#define rename rename_test
#define fsync sync_test
#define VKLOG(...) ((void)0)
#define LOG(format, ...) do { assert(strstr(format,"[RED] [vk] could not create an empty pipeline cache")); reds++; } while (0)
#define VKCHK(x) assert((x)==VK_SUCCESS)
#include "gfx_vk_pipeline_cache.inc"
static void disk(const void *data,size_t n) { FILE *f=fopen(g_pcache_path,"wb"); assert(f); assert(fwrite(data,1,n,f)==n); assert(!fclose(f)); }
static void *save_thread(void *unused) { (void)unused; vk_pipeline_cache_save(); return NULL; }
int main(int argc,char **argv) {
    assert(argc==3); int mode=atoi(argv[2]); setenv("DM_ROOT",argv[1],1);
    V.props.vendorID=123; V.props.deviceID=456; V.props.driverVersion=7; V.props.apiVersion=VK_API_VERSION_1_1;
    memset(V.props.pipelineCacheUUID,9,VK_UUID_SIZE);
    uint32_t h[4]={32,VK_PIPELINE_CACHE_HEADER_VERSION_ONE,123,456};
    memcpy(payload,h,sizeof h); memcpy(payload+16,V.props.pipelineCacheUUID,16);
    pcache_load(); assert(loaded==0);
    vk_pipeline_cache_changed(); vk_pipeline_cache_save();
    unsigned char original[sizeof(pcache_header)+sizeof payload];
    FILE *f=fopen(g_pcache_path,"rb"); assert(f); assert(fread(original,1,sizeof original,f)==sizeof original); fclose(f);
    pcache_load(); assert(loaded==sizeof payload);
    if (mode==1) {
        // Legacy cache migration must work even without a newly created pipeline.
        disk(payload,sizeof payload); pcache_load(); assert(loaded==sizeof payload);
        incomplete=1; vk_pipeline_cache_save(); incomplete=0;
        fail_sync=1; vk_pipeline_cache_save(); fail_sync=0;
        f=fopen(g_pcache_path,"rb"); assert(f); unsigned char raw[sizeof payload];
        assert(fread(raw,1,sizeof raw,f)==sizeof raw && fgetc(f)==EOF); fclose(f);
        assert(!memcmp(raw,payload,sizeof raw)); // unsuccessful migration keeps the legacy file
        vk_pipeline_cache_save();
        f=fopen(g_pcache_path,"rb"); assert(f); unsigned char migrated[sizeof original];
        assert(fread(migrated,1,sizeof migrated,f)==sizeof migrated); fclose(f);
        assert(!memcmp(migrated,original,sizeof original));
        // Legacy compatibility is established only by Vulkan's complete 32-byte header.
        for(size_t i=0;i<32;i++) { unsigned char raw[sizeof payload]; memcpy(raw,payload,sizeof raw); raw[i]^=1;
            disk(raw,sizeof raw); pcache_load(); assert(!loaded); }
        for(size_t n=0;n<32;n++) { disk(payload,n); pcache_load(); assert(!loaded); }
        return 0;
    }
    if (mode==2) {
        int prior_syncs=syncs, prior_renames=renames;
        vk_pipeline_cache_changed(); vk_pipeline_cache_save();
        assert(syncs==prior_syncs && renames==prior_renames);
        int prior_queries=queries; vk_pipeline_cache_save(); assert(queries==prior_queries);
        payload[63]=42; vk_pipeline_cache_changed(); vk_pipeline_cache_save();
        assert(syncs==prior_syncs+1 && renames==prior_renames+1);
        vk_pipeline_cache_changed(); vk_pipeline_cache_save();
        assert(syncs==prior_syncs+1 && renames==prior_renames+1);
        return 0;
    }
    if (mode==3) {
        char stale[1100]; snprintf(stale,sizeof stale,"%s.tmp",g_pcache_path);
        f=fopen(stale,"wb"); assert(f); fputs("killed during write",f); fclose(f);
        payload[63]=43; vk_pipeline_cache_changed(); vk_pipeline_cache_save();
        assert(access(stale,F_OK)==-1); pcache_load(); assert(loaded==sizeof payload);
        return 0;
    }
    if (mode==4) {
        reject_empty=1; setenv("DM_VK_NOPCACHE","1",1); reds=0;
        pcache_load(); assert(!V.pcache && reds==1); unsetenv("DM_VK_NOPCACHE");
        reject=1; reds=0; creates=0; pcache_load(); assert(!V.pcache && reds==1 && creates==2);
        return 0;
    }
    // Any envelope identity or payload change, truncation or trailing bytes stays out of the driver.
    for(size_t i=0;i<sizeof original;i++) {
        unsigned char bad[sizeof original]; memcpy(bad,original,sizeof bad); bad[i]^=1;
        disk(bad,sizeof bad); pcache_load(); assert(loaded==0);
    }
    for(size_t n=0;n<sizeof original;n++) { disk(original,n); pcache_load(); assert(loaded==0); }
    disk(payload,sizeof payload); pcache_load(); assert(loaded==sizeof payload);
    disk(original,sizeof original); f=fopen(g_pcache_path,"ab"); fputc(1,f); fclose(f); pcache_load(); assert(loaded==0);
    disk(original,sizeof original); reject=1; creates=0; pcache_load(); assert(loaded==0 && creates==2); reject=0;
    disk(original,sizeof original); setenv("DM_VK_NOPCACHE","1",1); pcache_load(); assert(loaded==0); unsetenv("DM_VK_NOPCACHE");
    // Valid digest cannot excuse a driver header for a different device.
    unsigned char bad[sizeof original]; memcpy(bad,original,sizeof bad); bad[sizeof(pcache_header)+8]^=1;
    CC_SHA256(bad+sizeof(pcache_header),sizeof payload,bad+offsetof(pcache_header,digest));
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
            subprocess.run([str(path/'test'),tmp,str(mode)],check=True)
            self.assertEqual(sorted(p.name for p in path.glob('vk_pipeline_cache.bin*')), ['vk_pipeline_cache.bin'])

    def test_disk_cache_boundary(self): self.check_case(0)
    def test_legacy_cache_migrates_without_new_pipelines(self): self.check_case(1)
    def test_identical_loaded_and_saved_content_skips_writes(self): self.check_case(2)
    def test_next_save_reuses_interrupted_temporary_file(self): self.check_case(3)
    def test_empty_cache_creation_failure_is_logged(self): self.check_case(4)


if __name__ == '__main__': unittest.main()
