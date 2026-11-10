// Writable driver products are separate from portable shader assets. Validate
// our envelope and Vulkan's version-one header before handing bytes to a driver.
// https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheHeaderVersionOne.html
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>

enum { PCACHE_ABI=1, PCACHE_MAX_BYTES=256<<20 };
typedef struct {
    char magic[8];
    uint32_t schema,abi,vendor,device,driver,api;
    uint8_t uuid[VK_UUID_SIZE],device_uuid[VK_UUID_SIZE],driver_uuid[VK_UUID_SIZE];
    char chain[16];
    uint64_t bytes;
    uint8_t digest[32];
} pcache_header;
_Static_assert(sizeof(pcache_header)==136,"pipeline cache envelope layout changed");
static VkPhysicalDeviceIDProperties g_pcache_ids={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
static char g_pcache_path[1024];
static _Atomic uint64_t g_pcache_generation;
static uint64_t g_pcache_saved;
static pthread_mutex_t g_pcache_io=PTHREAD_MUTEX_INITIALIZER;

static pcache_header pcache_identity(void) {
    pcache_header h={.magic="DMVKPC1",.schema=1,.abi=PCACHE_ABI,.vendor=V.props.vendorID,
        .device=V.props.deviceID,.driver=V.props.driverVersion,.api=V.props.apiVersion};
    memcpy(h.uuid,V.props.pipelineCacheUUID,sizeof h.uuid);
    memcpy(h.device_uuid,g_pcache_ids.deviceUUID,sizeof h.device_uuid);
    memcpy(h.driver_uuid,g_pcache_ids.driverUUID,sizeof h.driver_uuid);
    snprintf(h.chain,sizeof h.chain,"%s","nx2android");
    return h;
}
static uint32_t pcache_u32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static int pcache_driver_header(const void *data,size_t bytes) {
    const uint8_t *p=data;
    return bytes>=32 && pcache_u32(p)==32 && pcache_u32(p+4)==VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
        pcache_u32(p+8)==V.props.vendorID && pcache_u32(p+12)==V.props.deviceID && !memcmp(p+16,V.props.pipelineCacheUUID,VK_UUID_SIZE);
}
static void pcache_path(void) {
    const char *root=getenv("DM_ROOT");g_pcache_path[0]=0;
    if (!root || !*root) return;
    int n=snprintf(g_pcache_path,sizeof g_pcache_path,"%s/vk_pipeline_cache.bin",root);
    if (n<0 || (size_t)n>=sizeof g_pcache_path) g_pcache_path[0]=0;
}
static size_t pcache_load(void) {
    VkPhysicalDeviceProperties2 props={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&g_pcache_ids};
    vkGetPhysicalDeviceProperties2(V.pd,&props);
    pcache_path();pcache_header identity=pcache_identity(),header;size_t bytes=0;void *data=NULL;
    FILE *file=g_pcache_path[0] && !getenv("DM_VK_NOPCACHE") ? fopen(g_pcache_path,"rb") : NULL;
    if (file) {
        struct stat st;int valid=!fstat(fileno(file),&st) && S_ISREG(st.st_mode) &&
            fread(&header,1,sizeof header,file)==sizeof header && !memcmp(&header,&identity,offsetof(pcache_header,bytes)) &&
            header.bytes>=32 && header.bytes<=PCACHE_MAX_BYTES && (uint64_t)st.st_size==sizeof header+header.bytes;
        if (valid) {
            data=malloc(header.bytes);
            if (data && fread(data,1,header.bytes,file)==header.bytes && pcache_driver_header(data,header.bytes)) {
                uint8_t digest[32];CC_SHA256(data,(CC_LONG)header.bytes,digest);
                if (!memcmp(digest,header.digest,sizeof digest)) bytes=header.bytes;
            }
        }
        fclose(file);
        if (!bytes) { free(data);data=NULL;VKLOG("vk: ignored incompatible or corrupt pipeline cache"); }
    }
    VkPipelineCacheCreateInfo info={VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,NULL,0,bytes,data};
    VkResult result=vkCreatePipelineCache(V.dev,&info,NULL,&V.pcache);
    if (result!=VK_SUCCESS) {
        V.pcache=VK_NULL_HANDLE;
        if (bytes) {
            info.initialDataSize=0;info.pInitialData=NULL;
            VKLOG("vk: driver rejected saved pipeline cache (%d); creating an empty cache",result);
            if (vkCreatePipelineCache(V.dev,&info,NULL,&V.pcache)!=VK_SUCCESS) V.pcache=VK_NULL_HANDLE;
        }
        bytes=0;
    }
    if (bytes) VKLOG("vk: loaded %zu pipeline-cache bytes for this device, driver and shader chain",bytes);
    free(data);return bytes;
}
void vk_pipeline_cache_changed(void) { atomic_fetch_add_explicit(&g_pcache_generation,1,memory_order_relaxed); }
void vk_pipeline_cache_save(void) {
    if (!V.pcache || !g_pcache_path[0]) return;
    pthread_mutex_lock(&g_pcache_io);
    uint64_t generation=atomic_load_explicit(&g_pcache_generation,memory_order_relaxed);
    if (generation==g_pcache_saved) { pthread_mutex_unlock(&g_pcache_io);return; }
    size_t bytes=0;void *data=NULL;char temporary[1100]={0};FILE *file=NULL;
    if (vkGetPipelineCacheData(V.dev,V.pcache,&bytes,NULL)!=VK_SUCCESS || bytes<32 || bytes>PCACHE_MAX_BYTES) goto done;
    size_t capacity=bytes;data=malloc(bytes);if (!data) goto done;
    // A concurrent pipeline build can grow the cache between calls. A partial
    // snapshot is valid Vulkan data, but leave it dirty for the next save.
    if (vkGetPipelineCacheData(V.dev,V.pcache,&bytes,data)!=VK_SUCCESS || bytes>capacity || !pcache_driver_header(data,bytes)) goto done;
    pcache_header header=pcache_identity();header.bytes=bytes;CC_SHA256(data,(CC_LONG)bytes,header.digest);
    int length=snprintf(temporary,sizeof temporary,"%s.XXXXXX",g_pcache_path);
    if (length<0 || (size_t)length>=sizeof temporary) goto done;
    int fd=mkstemp(temporary);if (fd<0) goto done;
    file=fdopen(fd,"wb");if (!file) { close(fd);goto done; }
    int ok=fwrite(&header,1,sizeof header,file)==sizeof header && fwrite(data,1,bytes,file)==bytes;
    if (ok) ok=fflush(file)==0 && fsync(fileno(file))==0;
    if (fclose(file)) ok=0;file=NULL;
    if (ok && !rename(temporary,g_pcache_path)) { temporary[0]=0;g_pcache_saved=generation; }
    else VKLOG("vk: pipeline-cache write was not published: %s",strerror(errno));
done:
    if (file) fclose(file);
    if (temporary[0]) unlink(temporary);
    free(data);pthread_mutex_unlock(&g_pcache_io);
}
