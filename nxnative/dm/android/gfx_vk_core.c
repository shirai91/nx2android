// Android port, Vulkan backend: device, memory pools, submissions, completion (see gfx_vk.h).
//   Memory pools: NVN gives the GPU the game's own memory (GPU address = CPU address). Each pool's pages are replaced by an AHardwareBuffer mapped at the same
//   address (its dma-buf mmapped MAP_FIXED, contents copied over), and the same AHardwareBuffer is imported as a VkBuffer: the game's pointer is GPU memory,
//   CPU-cached and coherent on Adreno (measured: probes/vkzc.c). A pool the allocator refuses keeps its anonymous pages and gets a GPU copy refreshed before
//   every submission (vk_pools_sync), correct but slow, reported once.
//   Submissions: a ring of slots, each with a command pool (upload + main command buffers), descriptor pools, scratch memory and objects freed once the GPU has
//   passed it. A submission signals a timeline semaphore with its fence; a completion thread waits for it and then applies what the CPU may see of its work
//   (event writes, counter reports), as nvn_mtl.m does in its completion handler.
#define _GNU_SOURCE
#include "gfx_vk.h"
#include "nvn_hmap.h"
#include "nvn_dirty.h"
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define VK_DEF(f) PFN_##f f;
VK_FNS(VK_DEF) VK_DEV_FNS(VK_DEF)
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
vkdev_t V;
#include "gfx_vk_pipeline_cache.inc"
pthread_mutex_t g_vk_queue_mu = PTHREAD_MUTEX_INITIALIZER;   // VkQueue access is externally synchronized: submissions and presents

// DM_VK_MEM_DIAG=1 at startup: observe copied-pool costs without changing memory protection or rendering.
static int mem_diag_on(void) {
    static _Thread_local int on = -1;
    if (on < 0) { const char *e = getenv("DM_VK_MEM_DIAG"); on = e && atoi(e); }
    return on;
}
static uint64_t mem_diag_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void mem_diag_zc(unsigned stage, const char *name, uint64_t size, const char *kind, int error) {
    if (!mem_diag_on()) return;
    static _Atomic unsigned seen;
    if (atomic_fetch_or(&seen, 1u << stage) & (1u << stage)) return;
    VKLOG("[memdiag] zero-copy failed at %s: %s=%d, size=%llu, page_size=%d", name, kind, error, (unsigned long long)size, getpagesize());
}
static void mem_diag_note(unsigned phase, uint64_t start, uint64_t bytes) {
    static const char *const names[] = {"completion-wait", "cpu-to-gpu", "gpu-to-cpu", "pool-flush", "pool-invalidate", "merge-scan", "sync-scan", "sync-protect", "sync-upload", "sync-baseline"};
    static _Thread_local struct { uint64_t calls, ns, bytes, last; } stats[10];
    uint64_t end = mem_diag_ns(), elapsed = end - start;
    stats[phase].calls++; stats[phase].ns += elapsed; stats[phase].bytes += bytes;
    if (stats[phase].calls > 3 && end - stats[phase].last < 1000000000ull) return;
    uint64_t faults, preps; nvn_dirty_stats(&faults, &preps);
    // These page counts are process-wide, may overlap other threads, and are not SIGSEGV event counts.
    VKLOG("[memdiag] %s: last %.2f ms / %.2f MiB, mean %.2f ms / %.2f MiB (%llu calls); process fault-released-pages=%llu prep-pages=%llu",
          names[phase], elapsed / 1e6, bytes / 1048576.0, stats[phase].ns / 1e6 / stats[phase].calls,
          stats[phase].bytes / 1048576.0 / stats[phase].calls, (unsigned long long)stats[phase].calls,
          (unsigned long long)faults, (unsigned long long)preps);
    stats[phase].last = end;
}

static void mem_diag_copyback(uint64_t total, uint64_t invalidate, uint64_t calls, uint64_t bytes) {
    static _Thread_local uint64_t n, last, total_ns, invalidate_ns, total_bytes;
    n++; total_ns += total; invalidate_ns += invalidate; total_bytes += bytes;
    uint64_t now = mem_diag_ns(); if (n > 3 && now - last < 1000000000ull) return;
    last = now;
    // Invalidation includes its existing wrapper diagnostics; bytes are logical ranges, not allocation padding.
    VKLOG("[memdiag] copyback-breakdown: last total %.2f ms, invalidate %.2f ms (%.1f%%, %llu calls / %.2f MiB), other %.2f ms; mean total %.2f ms, invalidate %.2f ms (%.1f%%, %.2f MiB), %llu submissions",
        total / 1e6, invalidate / 1e6, total ? 100.0 * invalidate / total : 0.0,
        (unsigned long long)calls, bytes / 1048576.0, (total - invalidate) / 1e6,
        total_ns / 1e6 / n, invalidate_ns / 1e6 / n, total_ns ? 100.0 * invalidate_ns / total_ns : 0.0,
        total_bytes / 1048576.0 / n, (unsigned long long)n);
}

static void mem_diag_baselines(const uint64_t count[4], const uint64_t bytes[4]) {
    static _Thread_local uint64_t calls, last, total_count[4], total_bytes[4];
    for (int i = 0; i < 4; i++) { total_count[i] += count[i]; total_bytes[i] += bytes[i]; }
    uint64_t now = mem_diag_ns(); if (++calls > 3 && now - last < 1000000000ull) return;
    last = now;
    static const char *const names[] = {"resync", "untracked", "incomplete-uncached", "incomplete-other"};
    for (int i = 0; i < 4; i++) VKLOG("[memdiag] baseline-reason %s: last %llu copies / %.2f MiB; total %llu copies / %.2f MiB (%llu syncs)",
        names[i], (unsigned long long)count[i], bytes[i] / 1048576.0, (unsigned long long)total_count[i], total_bytes[i] / 1048576.0, (unsigned long long)calls);
}

uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < V.mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (V.mem.memoryTypes[i].propertyFlags & want) == want && !(V.mem.memoryTypes[i].propertyFlags & avoid)) return i;
    for (uint32_t i = 0; i < V.mem.memoryTypeCount; i++) if ((bits >> i & 1) && (V.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static void mem_diag_pool(uint64_t size, uint32_t bits, uint32_t selected) {
    if (!mem_diag_on()) return;
    VkMemoryPropertyFlags flags = V.mem.memoryTypes[selected].propertyFlags;
    uint32_t cached = vk_mem_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    VKLOG("[memdiag] copied buffer %.2f MiB: allowed-types %#x, selected type %u flags %#x (host-cached %s); cached candidate %d (candidate lookup only)",
        size / 1048576.0, bits, selected, flags, flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "yes" : "no", cached == UINT32_MAX ? -1 : (int)cached);
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }
static int is_on(const char *const *on, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(on[i], name)) return 1; return 0; }
// a feature struct as vk_init queries it (sType, pNext, then VkBool32s; the largest, descriptor indexing, has 20); featx: its VkBool32s, all off if it is not chained
typedef struct { VkBaseOutStructure h; VkBool32 b[24]; } featx_t;
static VkBool32 *featx(featx_t *x, uint32_t n, VkStructureType s) { static VkBool32 off[24]; for (uint32_t i = 0; i < n; i++) if (x[i].h.sType == s) return x[i].b; return off; }   // (off: only 0 is written to it)

static void completion_start(void);
static void slots_init(void);
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_validation_msg(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)type; (void)u;   // each distinct message ID logged its first 3 times
    enum { NID = 512 }; static int32_t ids[NID]; static uint8_t cnt[NID]; static int nid; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu); int k = 0; for (; k < nid; k++) if (ids[k] == d->messageIdNumber) break;
    if (k == nid && nid < NID) ids[nid++] = d->messageIdNumber;
    int show = k < NID && cnt[k] < 3; if (k < NID && cnt[k] < 255) cnt[k]++; pthread_mutex_unlock(&mu);
    if (show) VKLOG("[RED] validation %s: %s", sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning", d->pMessage);
    return VK_FALSE;
}

// Whether a driver offers a Vulkan 1.3 GPU (DM_VK_DRIVER: a Turnip built for another Adreno generation loads and then has none)
static int drv_has_gpu(PFN_vkGetInstanceProcAddr g) {
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)g(NULL, "vkCreateInstance"); VkInstance in; int ok = 0;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dmhost", 1, "nxnative", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (!ci || ci(&ici, NULL, &in)) return 0;
    PFN_vkEnumeratePhysicalDevices ep = (PFN_vkEnumeratePhysicalDevices)g(in, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gp = (PFN_vkGetPhysicalDeviceProperties)g(in, "vkGetPhysicalDeviceProperties");
    PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)g(in, "vkDestroyInstance");
    VkPhysicalDevice pd[8]; uint32_t n = 8;
    if (ep && gp && ep(in, &n, pd) >= 0) for (uint32_t i = 0; i < n && !ok; i++) { VkPhysicalDeviceProperties p; gp(pd[i], &p); ok = p.apiVersion >= VK_API_VERSION_1_3;
        VKLOG("DM_VK_DRIVER: %s, Vulkan %u.%u, driver version 0x%x", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), p.driverVersion); }
    if (di) di(in, NULL);
    return ok;
}

void vk_init(void) {
    static int done; if (done) return; done = 1;
    // DM_VK_DRIVER=<library>: a Vulkan driver the APK carries (Mesa's Turnip for Adreno; porter --vulkan-driver), loaded through libadrenotools (BSD-2-Clause), which the
    // APK carries beside it with its hook libraries: a second copy of the system's loader in a linker namespace of its own, whose driver is this library. The loader keeps
    // making the surface and the swapchain, which Turnip on Android has none of (the platform loader makes them over VK_ANDROID_native_buffer). TOTKDroid, a port of Tears
    // of the Kingdom 1.4.3 that also replays NVN on Vulkan, measured its Great Sky Island forest at GPU 16-17 ms a frame on Turnip on an Adreno 830 (docs/FPS-2026-10-31.md),
    // against 22-27 ms here on Qualcomm's driver. A driver that does not load, or offers no Vulkan 1.3 GPU, leaves the system's
    const char *drv = getenv("DM_VK_DRIVER"); void *lib = NULL;
    if (drv && *drv) {
        Dl_info di; char dir[512] = "", dirs[520] = "";   // the APK's extracted libraries (mkapk.py: extractNativeLibs), where adrenotools finds its hooks and the driver
        if (dladdr((void *)vk_init, &di) && di.dli_fname && strrchr(di.dli_fname, '/')) {
            snprintf(dir, sizeof dir, "%.*s", (int)(strrchr(di.dli_fname, '/') - di.dli_fname), di.dli_fname); snprintf(dirs, sizeof dirs, "%s/", dir); }
        void *at = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
        void *(*open_vk)(int, int, const char *, const char *, const char *, const char *, const char *, void **) =
            at ? (void *(*)(int, int, const char *, const char *, const char *, const char *, const char *, void **))dlsym(at, "adrenotools_open_libvulkan") : NULL;
        if (!open_vk || !*dir) VKLOG("[RED] DM_VK_DRIVER %s: no libadrenotools in the APK (%s); the system's driver instead", drv, at ? "no adrenotools_open_libvulkan" : dlerror());
        else if (!(lib = open_vk(RTLD_NOW | RTLD_LOCAL, 1 /* ADRENOTOOLS_DRIVER_CUSTOM */, NULL, dir, dirs, drv, NULL, NULL)))
            VKLOG("[RED] DM_VK_DRIVER %s: adrenotools could not load it from %s; the system's driver instead", drv, dir);
        else if (!(p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr")) || !drv_has_gpu(p_vkGetInstanceProcAddr)) {
            VKLOG("[RED] DM_VK_DRIVER %s: no Vulkan 1.3 GPU through it; the system's driver instead", drv); lib = NULL; }   // (left loaded: a driver does not always unload cleanly)
        else VKLOG("Vulkan driver %s through adrenotools, not the system's (%s)", drv, dir);
    }
    if (!lib) {
        if (!(lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL))) die("[vk] libvulkan.so: %s", dlerror());
        p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    }
    vkCreateInstance = (PFN_vkCreateInstance)p_vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    const char *iext[] = {"VK_KHR_surface", "VK_KHR_android_surface", "VK_EXT_debug_utils"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dmhost", 1, "nxnative", 1, VK_API_VERSION_1_3};
    // DM_VK_VALIDATE=1: Khronos's validation layer, which the APK carries only when built with it (mkapk.py --extra-so); its messages go to the log
    int validate = getenv("DM_VK_VALIDATE") && atoi(getenv("DM_VK_VALIDATE"));
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, validate ? 1u : 0u, layers, validate ? 3u : 2u, iext};
    VkResult ir = vkCreateInstance(&ici, NULL, &V.inst);
    if (ir && validate) { VKLOG("[RED] validation layer not available (%d): running without it", ir); validate = 0; ici.enabledLayerCount = 0; ici.enabledExtensionCount = 2; ir = vkCreateInstance(&ici, NULL, &V.inst); }
    VKCHK(ir);
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT mk = (PFN_vkCreateDebugUtilsMessengerEXT)p_vkGetInstanceProcAddr(V.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, vk_validation_msg, NULL};
        VkDebugUtilsMessengerEXT m; if (mk && !mk(V.inst, &mi, NULL, &m)) VKLOG("validation layer on");
    }
#define VK_LOADI(f) f = (PFN_##f)p_vkGetInstanceProcAddr(V.inst, #f);
    VK_FNS(VK_LOADI)
    uint32_t n = 1; VKCHK(vkEnumeratePhysicalDevices(V.inst, &n, &V.pd)); if (!n) die("[vk] no GPU");
    vkGetPhysicalDeviceProperties(V.pd, &V.props); vkGetPhysicalDeviceMemoryProperties(V.pd, &V.mem);
    // the device names itself before any check below can stop: a Moto g 5G 2024's log (Adreno 619) ended at the timeline check with no GPU or driver named.
    // driver: as Qualcomm encodes it (VK_MAKE_VERSION: 512.530.0), with the raw value for other vendors
    { uint32_t d = V.props.driverVersion;
      VKLOG("GPU %s (vendor %#x, device %#x), Vulkan %u.%u.%u, driver %u.%u.%u (%#x)", V.props.deviceName, V.props.vendorID, V.props.deviceID, VK_API_VERSION_MAJOR(V.props.apiVersion),
            VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion), d >> 22, d >> 12 & 0x3ff, d & 0xfff, d); }
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, NULL);
    VkQueueFamilyProperties qp[16]; if (qn > 16) qn = 16; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, qp);
    V.qf = 0; while (V.qf < qn && (qp[V.qf].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) V.qf++;
    if (V.qf == qn) die("[vk] no graphics + compute queue");
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(V.pd, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne, sizeof *ex); vkEnumerateDeviceExtensionProperties(V.pd, NULL, &ne, ex);
    const char *want[] = {"VK_KHR_swapchain", "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign", "VK_EXT_custom_border_color",
                          "VK_EXT_robustness2", "VK_EXT_depth_clip_control", "VK_EXT_provoking_vertex", "VK_EXT_index_type_uint8", "VK_EXT_transform_feedback",
                          "VK_GOOGLE_display_timing"};
    const char *on[64]; uint32_t non = 0;
    for (unsigned i = 0; i < sizeof want / sizeof *want; i++) if (has_ext(ex, ne, want[i])) on[non++] = want[i]; else VKLOG("device has no %s", want[i]);
    // A Vulkan 1.1 or 1.2 device (Legion Y700 2023, Adreno 730: its stock driver reports 1.1) has what the replay uses from 1.2 / 1.3 as extensions: they are enabled
    // when present, so the features chained below and the functions loaded by their KHR names are valid there. A 1.3 device needs none of them
    int v12 = V.props.apiVersion >= VK_API_VERSION_1_2, v13 = V.props.apiVersion >= VK_API_VERSION_1_3; uint32_t npromo = 0;
    static const char *promo12[] = {"VK_KHR_create_renderpass2", "VK_KHR_imageless_framebuffer", "VK_KHR_image_format_list", "VK_KHR_depth_stencil_resolve", "VK_KHR_timeline_semaphore",
        "VK_KHR_shader_float16_int8", "VK_KHR_8bit_storage", "VK_EXT_descriptor_indexing", "VK_EXT_scalar_block_layout", "VK_KHR_uniform_buffer_standard_layout",
        "VK_KHR_shader_float_controls", "VK_KHR_spirv_1_4", "VK_KHR_vulkan_memory_model", "VK_KHR_separate_depth_stencil_layouts", "VK_EXT_host_query_reset",
        "VK_KHR_sampler_mirror_clamp_to_edge", "VK_KHR_draw_indirect_count", "VK_KHR_buffer_device_address", "VK_EXT_shader_viewport_index_layer", "VK_EXT_sampler_filter_minmax",
        "VK_KHR_shader_subgroup_extended_types", "VK_KHR_shader_atomic_int64", "VK_KHR_driver_properties"};
    static const char *promo13[] = {"VK_EXT_image_robustness", "VK_KHR_dynamic_rendering", "VK_KHR_synchronization2", "VK_EXT_shader_demote_to_helper_invocation",
        "VK_EXT_subgroup_size_control", "VK_KHR_copy_commands2", "VK_KHR_maintenance4", "VK_KHR_format_feature_flags2", "VK_EXT_extended_dynamic_state",
        "VK_KHR_zero_initialize_workgroup_memory", "VK_KHR_shader_terminate_invocation", "VK_EXT_inline_uniform_block", "VK_EXT_pipeline_creation_cache_control",
        "VK_EXT_texel_buffer_alignment", "VK_EXT_4444_formats", "VK_KHR_shader_integer_dot_product", "VK_EXT_private_data"};
    for (unsigned i = 0; !v12 && i < sizeof promo12 / sizeof *promo12 && non < 64; i++) if (has_ext(ex, ne, promo12[i])) { on[non++] = promo12[i]; npromo++; }
    for (unsigned i = 0; !v13 && i < sizeof promo13 / sizeof *promo13 && non < 64; i++) if (has_ext(ex, ne, promo13[i])) { on[non++] = promo13[i]; npromo++; }
    // features: what the replay uses when the device has it. VkPhysicalDeviceVulkan11/12/13Features are valid only on a 1.2 / 1.3 device (a 1.1 driver ignores
    // them and nothing would be enabled); below that, the same features come through Vulkan 1.1's own structs and those of the extensions enabled above (xf)
    static const struct { const char *ext; VkStructureType s; } XF[] = {   // ext NULL: Vulkan 1.1 core, VkPhysicalDeviceVulkan11Features's members
        {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES}, {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES},
        {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES}, {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES},
        {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES}, {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES},
        {"VK_KHR_imageless_framebuffer", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES}, {"VK_KHR_timeline_semaphore", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES},
        {"VK_KHR_shader_float16_int8", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES}, {"VK_KHR_8bit_storage", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES},
        {"VK_EXT_descriptor_indexing", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES}, {"VK_EXT_scalar_block_layout", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES},
        {"VK_KHR_uniform_buffer_standard_layout", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES}, {"VK_KHR_vulkan_memory_model", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES},
        {"VK_KHR_separate_depth_stencil_layouts", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES}, {"VK_EXT_host_query_reset", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES},
        {"VK_KHR_buffer_device_address", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES}, {"VK_KHR_shader_subgroup_extended_types", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES},
        {"VK_KHR_shader_atomic_int64", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES},   // (mirror clamp, indirect count, minmax, viewport layer: the extension is the feature)
        {"VK_KHR_dynamic_rendering", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES}, {"VK_KHR_synchronization2", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES},
        {"VK_EXT_shader_demote_to_helper_invocation", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES}, {"VK_EXT_subgroup_size_control", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES},
        {"VK_KHR_maintenance4", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES}, {"VK_KHR_zero_initialize_workgroup_memory", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES},
        {"VK_KHR_shader_terminate_invocation", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES}, {"VK_EXT_inline_uniform_block", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES},
        {"VK_EXT_pipeline_creation_cache_control", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES}, {"VK_KHR_shader_integer_dot_product", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES},
        {"VK_EXT_private_data", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES}};   // (VK_EXT_image_robustness: robustImageAccess stays off, as on 1.3)
    uint32_t nxf = sizeof XF / sizeof *XF; featx_t xf[sizeof XF / sizeof *XF]; memset(xf, 0, sizeof xf); VkBaseOutStructure *xs = NULL;
    for (int i = (int)nxf - 1; i >= 0; i--) if (XF[i].ext ? is_on(on, non, XF[i].ext) : !v12) { xf[i].h.sType = XF[i].s; xf[i].h.pNext = xs; xs = &xf[i].h; }
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, xs};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, v13 ? (void *)&f13 : xs};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 ff = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, v12 ? (void *)&f11 : xs};
    vkGetPhysicalDeviceFeatures2(V.pd, &ff);
    // without timeline semaphores (Vulkan 1.1 drivers: the Adreno 618 / 619 of 2021, 512.530) each submission signals a fence of a ring instead (slots_init).
    // DM_VK_X_FENCES=1 (start-up): the fences on a device that has them; with DM_VK_X_BC_CPU=1 below, a device of today stands in for those drivers
    { const char *e = getenv("DM_VK_X_FENCES");
      V.has_timeline = (v12 ? f12.timelineSemaphore : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)[0]) && !(e && atoi(e)); }
    if (!V.has_timeline) VKLOG("no timeline semaphores (Vulkan 1.2 or VK_KHR_timeline_semaphore): submissions signal fences");
    // dynamic rendering is 1.3 or its extension with the feature on; a loaded pointer does not tell, since the loader hands out a trampoline for core functions the driver lacks
    V.has_dynrender = v13 ? f13.dynamicRendering : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)[0];
    // imageless framebuffers need render pass 2 (gfx_vk_exec.c rp_get): core in 1.2, else its extension
    V.has_draw_count = v12 ? f12.drawIndirectCount : is_on(on, non, "VK_KHR_draw_indirect_count");
    V.has_imageless = v12 ? f12.imagelessFramebuffer : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES)[0] && is_on(on, non, "VK_KHR_create_renderpass2");
    if (!V.has_imageless && !V.has_dynrender) die("[vk] the device has neither dynamic rendering nor imageless framebuffers with render pass 2: passes cannot be begun");
    if (!v13) VKLOG("Vulkan %u.%u device: %u extensions enabled for the features promoted to 1.2 / 1.3; dynamic rendering %s", VK_API_VERSION_MAJOR(V.props.apiVersion),
                    VK_API_VERSION_MINOR(V.props.apiVersion), npromo, V.has_dynrender ? "yes" : "no (render pass objects only)");
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fcb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT fr2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, &fcb};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fdc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, &fr2};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT fu8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, &fdc};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT fxf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, &fu8};
    VkPhysicalDeviceFeatures2 fx = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fxf};
    vkGetPhysicalDeviceFeatures2(V.pd, &fx);
    // transform feedback (NVN BeginTransformFeedback; gfx_vk_exec.c xfb_*): Xenoblade Chronicles 3 captures particle state with it
    V.has_xfb = fxf.transformFeedback && has_ext(ex, ne, "VK_EXT_transform_feedback"); fxf.geometryStreams = 0;
    fr2.robustBufferAccess2 = 0; fr2.robustImageAccess2 = 0;   // nullDescriptor only: unbound slots read 0 without a dummy
    V.has_custom_border = fcb.customBorderColors && has_ext(ex, ne, "VK_EXT_custom_border_color");
    V.has_robust2 = fr2.nullDescriptor && has_ext(ex, ne, "VK_EXT_robustness2");
    V.has_depth_clip_ctl = fdc.depthClipControl && has_ext(ex, ne, "VK_EXT_depth_clip_control");
    V.has_dtiming = has_ext(ex, ne, "VK_GOOGLE_display_timing");   // frame pacing on the display (gfx_vk_present.c pace_*)
    V.has_bc = ff.features.textureCompressionBC; V.has_astc = ff.features.textureCompressionASTC_LDR; V.has_gs = ff.features.geometryShader;
    { const char *e = getenv("DM_VK_X_BC_CPU"); if (e && atoi(e)) V.has_bc = 0; }   // (start-up) BC textures decoded on the CPU as on a device without them (gfx_vk_tex.c bc_decode)
    V.max_aniso = V.props.limits.maxSamplerAnisotropy;
    // subgroups: where the shaders' votes, ballots and shuffles may run; a module whose stage lacks them runs as subgroups of one invocation (gfx_vk_shader.c, spv_one_lane).
    // Turnip lists graphics stages and quad operations only on a GPU with getfiberid (tu_device.cc): the Adreno 7xx have it, the Adreno 650 (a6xx gen3) not (compute alone).
    // DM_VK_SUBGROUP_PIN=<lanes> (start-up): game pipelines' fragment and compute stages run at that subgroup size (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo)
    // where requiredSubgroupSizeStages allows it; by default the driver picks per pipeline (Turnip on Adreno 6xx / 7xx: 64 or 128, only those two stages pinnable)
    { VkPhysicalDeviceSubgroupSizeControlProperties sc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
      VkPhysicalDeviceSubgroupProperties sg = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, v13 || has_ext(ex, ne, "VK_EXT_subgroup_size_control") ? (void *)&sc : NULL};
      VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg}; vkGetPhysicalDeviceProperties2(V.pd, &p2);
      int ctl = v13 ? f13.subgroupSizeControl : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES)[0];   // (enabled below with the rest of the chain)
      V.sg_size = sg.subgroupSize; V.sg_stages = sg.supportedStages; V.sg_ops = sg.supportedOperations; V.sg_quad_all = sg.quadOperationsInAllStages;
      V.sg_min = sc.minSubgroupSize ? sc.minSubgroupSize : sg.subgroupSize; V.sg_max = sc.maxSubgroupSize ? sc.maxSubgroupSize : sg.subgroupSize; V.sg_maxwgs = sc.maxComputeWorkgroupSubgroups;
      uint32_t pinnable = ctl ? sc.requiredSubgroupSizeStages : 0, st = pinnable & (VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
      VKLOG("subgroups: size %u (%u-%u), stages %#x, ops %#x, quad all stages %s, pinnable %#x", V.sg_size, V.sg_min, V.sg_max, V.sg_stages, V.sg_ops, V.sg_quad_all ? "yes" : "no", pinnable);
      const char *e = getenv("DM_VK_SUBGROUP_PIN"); uint32_t pin = e ? (uint32_t)atoi(e) : 0;
      if (pin && (!st || (pin & (pin - 1)) || pin < V.sg_min || pin > V.sg_max)) VKLOG("[RED] DM_VK_SUBGROUP_PIN=%u: the device cannot pin fragment or compute shaders to it (sizes %u-%u, pinnable stages %#x): the driver picks", pin, V.sg_min, V.sg_max, pinnable);
      else if (pin) { V.sg_pin_stages = st; V.sg_pin = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, NULL, pin};
                      VKLOG("subgroups: stages %#x pinned to %u lanes (DM_VK_SUBGROUP_PIN)", st, pin); } }
    // enable: everything the shaders may use that the device has
    VkPhysicalDeviceFeatures en = ff.features; en.robustBufferAccess = 0; en.shaderFloat64 = 0;
    f12.bufferDeviceAddressCaptureReplay = 0; f12.bufferDeviceAddressMultiDevice = 0;
    featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)[1] = featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)[2] = 0;   // (its extension's: the same two)
    f13.robustImageAccess = 0;
    fcb.pNext = NULL; fr2.pNext = V.has_custom_border ? (void *)&fcb : NULL; fdc.pNext = V.has_robust2 ? (void *)&fr2 : (V.has_custom_border ? (void *)&fcb : NULL);
    fu8.pNext = V.has_depth_clip_ctl ? (void *)&fdc : fdc.pNext;
    fxf.pNext = &fu8;
    VkBaseOutStructure *t = ff.pNext; while (t->pNext) t = t->pNext;   // the queried chain (core / promoted features), then the extensions'
    t->pNext = V.has_xfb ? (VkBaseOutStructure *)&fxf : (VkBaseOutStructure *)&fu8;
    VkPhysicalDeviceFeatures2 enf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, ff.pNext, en};
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, V.qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enf, 0, 1, &qci, 0, NULL, non, on, NULL};
    VKCHK(vkCreateDevice(V.pd, &dci, NULL, &V.dev));
    vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)p_vkGetInstanceProcAddr(V.inst, "vkGetDeviceProcAddr");
#define VK_LOADD(f) f = (PFN_##f)vkGetDeviceProcAddr(V.dev, #f); if (!f) f = (PFN_##f)vkGetDeviceProcAddr(V.dev, #f "KHR"); if (!f) f = (PFN_##f)p_vkGetInstanceProcAddr(V.inst, #f);   // a promoted function by its extension's name (Vulkan 1.1 / 1.2 devices)
    VK_DEV_FNS(VK_LOADD)
    // A driver can report the timeline-semaphore feature (and Vulkan 1.3) and still export no vkWaitSemaphores: the emulator's ranchu / gfxstream driver does
    // (both the device and the instance dispatcher return null for it and for vkGetSemaphoreCounterValue), and the first wait then called address 0
    // (SIGSEGV pc=0, lr=wait_fence+0x70, thread 'dm-vk-complete', South Park: The Stick of Truth). The feature query is not proof the entry points load:
    // without them the fence ring is used for both the submit and the wait, which V.has_timeline decides.
    if (V.has_timeline && (!vkWaitSemaphores || !vkGetSemaphoreCounterValue)) {
        VKLOG("[vk] timeline semaphores reported but vkWaitSemaphores / vkGetSemaphoreCounterValue did not load: submissions signal fences");
        V.has_timeline = 0;
    }
    { void vk_dq_init(void); vk_dq_init(); }   // DM_VK_MT_REPLAY: the vkCmd* calls through the recorder thread (gfx_vk_dq.c)
    vkGetDeviceQueue(V.dev, V.qf, 0, &V.q);
    size_t dn = pcache_load(); free(ex);
    VKLOG("%s, Vulkan %u.%u, driver %#x; timeline semaphores %s, BC %s, ASTC %s, geometry shaders %s, nullDescriptor %s, custom border colours %s, depth clip control %s, transform feedback %s; pipeline cache %zu bytes",
          V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), V.props.driverVersion, V.has_timeline ? "yes" : "no (fences)",
          V.has_bc ? "yes" : "no (decoded on the CPU)", V.has_astc ? "yes" : "no", V.has_gs ? "yes" : "no", V.has_robust2 ? "yes" : "no", V.has_custom_border ? "yes" : "no", V.has_depth_clip_ctl ? "yes" : "no", V.has_xfb ? "yes" : "no", dn);
    if (mem_diag_on()) VKLOG("[memdiag] enabled storage-image features: extended-formats %s, read-without-format %s, write-without-format %s",
        en.shaderStorageImageExtendedFormats ? "yes" : "no", en.shaderStorageImageReadWithoutFormat ? "yes" : "no", en.shaderStorageImageWriteWithoutFormat ? "yes" : "no");
    slots_init();
    completion_start();
    { void crash_handlers_check(const char *when); crash_handlers_check("after the Vulkan device was made"); }   // (again at present 600, gfx_vk_present.c: the shader compiler loads with the first pipelines)
}
const void *vk_sg_pin(VkShaderStageFlagBits stage, const uint32_t *local) {
    if (!(V.sg_pin_stages & stage)) return NULL;
    // a compute workgroup must fit in maxComputeWorkgroupSubgroups subgroups of the pinned size (VUID-VkPipelineShaderStageCreateInfo-pNext-02756): a larger one keeps the driver's size
    if (local && (uint64_t)local[0] * local[1] * local[2] > (uint64_t)V.sg_pin.requiredSubgroupSize * V.sg_maxwgs) return NULL;
    return &V.sg_pin;
}

// ---------------- memory pools ----------------
// DARK SOULS: REMASTERED made more than 4,096 pools within 3 s of boot (FromSoftware's engine gives many resources a small pool of its own: 0x1000, 0x12000 bytes), which stopped it
// with "too many memory pools"; the index (g_pool_ord) is 16-bit, so up to 65,535
#define MAXPOOL 32768
static vkpool_t g_pool[MAXPOOL]; static int g_npool;
static pthread_rwlock_t g_pool_mu = PTHREAD_RWLOCK_INITIALIZER;
static uint32_t g_pool_gen;   // bumped under the write lock when a pool is added or removed: vk_pool_of's per-thread cache holds while it is unchanged
// g_pool's indices by lo, remade under the write lock whenever a pool is added or removed: vk_pool_of searches it. A linear scan of g_pool behind the per-thread
// cache was 18% of the profiler's samples of Super Mario Bros. Wonder's replay in its first level (a draw looks up more pools than the cache holds); with the
// search its world map's replay took 4.2 ms a frame instead of 4.6-4.9
static uint16_t g_pool_ord[MAXPOOL];
// The index kept in order one pool at a time (sorting it whole at every add is O(n log n) a pool, which thousands of pools made slow): pool k (just appended) goes in at its place
static void pool_ord_insert(int k) {
    int lo = 0, hi = k; uint64_t a = g_pool[k].lo;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_pool[g_pool_ord[m]].lo <= a) lo = m + 1; else hi = m; }
    memmove(g_pool_ord + lo + 1, g_pool_ord + lo, (size_t)(k - lo) * sizeof *g_pool_ord); g_pool_ord[lo] = (uint16_t)k;
}
// pool i removed and the last pool (index g_npool, already decremented) moved into its place: i leaves the index and the moved pool's entry takes index i
static void pool_ord_remove(int i) {
    int n = g_npool + 1, w = 0;
    for (int r = 0; r < n; r++) { uint16_t v = g_pool_ord[r]; if (v == i) continue; g_pool_ord[w++] = v == g_npool ? (uint16_t)i : v; }
}
static int recv_fds(int sock, int *fds, int max) {
    char data[4096], ctl[CMSG_SPACE(sizeof(int) * 16)]; struct iovec iov = {data, sizeof data};
    struct msghdr m = {0}; m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof ctl;
    if (recvmsg(sock, &m, 0) < 0) return -1;
    int n = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            for (size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int) && n < max; i++) fds[n++] = ((int *)CMSG_DATA(c))[i];
    return n;
}
static const VkBufferUsageFlags POOL_USAGE0 = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
#define POOL_USAGE (POOL_USAGE0 | (V.has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0))   // any pool range may be a transform feedback buffer
static VkResult pool_copied_alloc(vkpool_t *p, uint64_t size, int cached) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b; VkResult r = vkCreateBuffer(V.dev, &bi, NULL, &b); if (r) return r;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
    uint32_t type = vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        (cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), 0);
    if (type == UINT32_MAX) { vkDestroyBuffer(V.dev, b, NULL); return VK_ERROR_FEATURE_NOT_PRESENT; }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    VkDeviceMemory m; r = vkAllocateMemory(V.dev, &ai, NULL, &m);
    if (r) { vkDestroyBuffer(V.dev, b, NULL); return r; }
    void *map; r = vkBindBufferMemory(V.dev, b, m, 0);
    if (!r) r = vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &map);
    if (r) { vkDestroyBuffer(V.dev, b, NULL); vkFreeMemory(V.dev, m, NULL); return r; }
    p->buf = b; p->mem = m; p->shadow = map;
    p->noncoherent = !(V.mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mem_diag_pool(size, req.memoryTypeBits, type); return VK_SUCCESS;
}
static void pool_cache_sync(vkpool_t *p, int invalidate, uint64_t lo, uint64_t hi) {
    if (!p->noncoherent) return;
    // Each pool owns/maps its allocation; round cache ranges out to whole atoms.
    VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, p->mem, 0, VK_WHOLE_SIZE};
    uint64_t atom = V.props.limits.nonCoherentAtomSize, size = p->hi - p->lo;
    range.offset = lo - lo % atom;
    uint64_t pad = (atom - hi % atom) % atom;
    // Use the allocation's end at the pool tail; subtract before rounding to avoid overflow.
    if (hi < size && pad < size - hi) range.size = hi + pad - range.offset;
    int diag = mem_diag_on(); uint64_t t = diag ? mem_diag_ns() : 0;
    if (invalidate) { VKCHK(vkInvalidateMappedMemoryRanges(V.dev, 1, &range)); }
    else { VKCHK(vkFlushMappedMemoryRanges(V.dev, 1, &range)); }
    if (diag) mem_diag_note(invalidate ? 4 : 3, t, range.size == VK_WHOLE_SIZE ? p->hi - p->lo - range.offset : range.size);
}
// Freed zero-copy pools' buffers, kept for the next pool of the same size (DM_VK_POOL_REUSE=0 turns it off): a pool made from one skips the hardware buffer's
// allocation (a binder call to the allocator service) and the driver's import, which took Pokémon Scarlet's three loading threads ~130 + ~100 ms each while a thrown
// ball's capture assets load (they drop old pools as they make new ones; the game waits on them, frames of 100-220 ms), and a pool dropped from a game thread no longer
// waits for the GPU (df_add outside a submission waits for every submission so far). An entry is taken only once the GPU has passed the submission open or next when
// it was kept; the oldest the GPU is done with make room
typedef struct { AHardwareBuffer *ahb; VkBuffer buf; VkDeviceMemory mem; uint64_t size, fence; } zcfree_t;
enum { ZCF_N = 128 };
#define ZCF_BYTES (384ull << 20)
static zcfree_t g_zcf[ZCF_N]; static int g_nzcf; static uint64_t g_zcf_bytes; static pthread_mutex_t g_zcf_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_zcf_hit, g_zcf_miss, g_zcf_kept;   // under g_zcf_mu (the pool line)
uint64_t mtl_last_fence(void);
static int zcf_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_POOL_REUSE"); v = !(e && !atoi(e)); } return v; }
static void zcf_free_now(const zcfree_t *e) { vkDestroyBuffer(V.dev, e->buf, NULL); vkFreeMemory(V.dev, e->mem, NULL); AHardwareBuffer_release(e->ahb); }
static int zcf_take(uint64_t size, zcfree_t *out) {
    if (!zcf_on()) return 0;
    uint64_t done = vk_fence_done(); int r = 0;
    pthread_mutex_lock(&g_zcf_mu);
    for (int i = 0; i < g_nzcf; i++) if (g_zcf[i].size == size && g_zcf[i].fence <= done) {
        *out = g_zcf[i]; memmove(g_zcf + i, g_zcf + i + 1, (size_t)(g_nzcf - i - 1) * sizeof *g_zcf); g_nzcf--; g_zcf_bytes -= size; r = 1; break; }
    if (r) g_zcf_hit++; else g_zcf_miss++;
    pthread_mutex_unlock(&g_zcf_mu);
    return r;
}
static int zcf_put(AHardwareBuffer *ahb, VkBuffer buf, VkDeviceMemory mem, uint64_t size) {   // 1: kept (the caller frees nothing)
    if (!zcf_on() || size > ZCF_BYTES / 4) return 0;
    zcfree_t e = {ahb, buf, mem, size, mtl_last_fence() + 1}, old[ZCF_N]; int nold = 0;
    uint64_t done = vk_fence_done();
    pthread_mutex_lock(&g_zcf_mu);
    while (g_nzcf == ZCF_N || g_zcf_bytes + size > ZCF_BYTES) {   // the oldest the GPU is done with go
        int i = 0; while (i < g_nzcf && g_zcf[i].fence > done) i++;
        if (i == g_nzcf) break;
        old[nold++] = g_zcf[i]; g_zcf_bytes -= g_zcf[i].size; memmove(g_zcf + i, g_zcf + i + 1, (size_t)(g_nzcf - i - 1) * sizeof *g_zcf); g_nzcf--;
    }
    int kept = g_nzcf < ZCF_N && g_zcf_bytes + size <= ZCF_BYTES;
    if (kept) { g_zcf[g_nzcf++] = e; g_zcf_bytes += size; g_zcf_kept++; }
    pthread_mutex_unlock(&g_zcf_mu);
    for (int i = 0; i < nold; i++) zcf_free_now(&old[i]);
    return kept;
}
// Small pools take a slot of a slab (DM_VK_POOL_SLABS=0 turns it off): one hardware buffer allocated and imported once, its slots mapped at the pools' addresses
// (its dma-buf mmapped at the slot's offset) and bound at the slot's offset (vkpool_t.boff). A hardware buffer of its own cost a 48 KB pool ~170 us to allocate (a binder
// call to the allocator service) and ~180 us to import on an AYN Thor, Turnip and Qualcomm's driver alike, ~0.9 ms a pool with the mappings, and Pokémon Scarlet makes
// ~1,000 pools of 4-256 KB a second while a thrown ball's capture assets load (1,500 a second while a save loads): its loading threads spent ~0.5 s a throw on them while
// the game waited, in frames of 100-190 ms. Neither driver offers VK_EXT_external_memory_host, which would take the game's own pages. Slots are 1, 2, 3, 4, 6, 8 ... 64 pages
// (at most 50% larger than the pool), 64 a slab, between 2 and 8 MB; slabs are kept for good, as the image blocks are, and a freed slot is taken again once the GPU has
// passed the submission open or next when it was freed
#define SLAB_POOL_MAX (256ull << 10)
static const uint32_t SLAB_PAGES[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
enum { SLAB_NCLS = sizeof SLAB_PAGES / sizeof *SLAB_PAGES, SLAB_SLOTS = 512 };
typedef struct slab_t { struct slab_t *next; AHardwareBuffer *ahb; int fd; uint8_t *map; VkBuffer buf; VkDeviceMemory mem; uint64_t slot_bytes; uint32_t nslot, nfree;
                        uint64_t bits[SLAB_SLOTS / 64], fence[SLAB_SLOTS]; } slab_t;   // bits: 1 = slot free; fence: the submission the GPU must have passed before it is taken
static slab_t *g_slab[SLAB_NCLS]; static pthread_mutex_t g_slab_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_slab_used, g_slab_bytes;   // pools placed in slabs so far, slab memory (under g_slab_mu: the pool line)
static int slab_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_POOL_SLABS"); v = !(e && !atoi(e)); } return v; }
static slab_t *slab_new(int c) {   // a slab of class c, allocated and imported (no lock held)
    uint64_t sb = (uint64_t)SLAB_PAGES[c] << 12; uint32_t n = 64;
    if (sb * n < (2u << 20)) n = (uint32_t)((2u << 20) / sb); if (sb * n > (8u << 20)) n = (uint32_t)((8u << 20) / sb); if (n > SLAB_SLOTS) n = SLAB_SLOTS;
    uint64_t size = sb * n; slab_t *s = calloc(1, sizeof *s); if (!s) return NULL;
    s->slot_bytes = sb; s->nslot = s->nfree = n; s->fd = -1;
    AHardwareBuffer_Desc d = {(uint32_t)size, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, 0, 0, 0};
    if (AHardwareBuffer_allocate(&d, &s->ahb)) { free(s); return NULL; }
    int sv[2], fds[16], nfd = -1;
    if (!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
        if (!AHardwareBuffer_sendHandleToUnixSocket(s->ahb, sv[0])) nfd = recv_fds(sv[1], fds, 16);
        close(sv[0]); close(sv[1]);
    }
    if (nfd >= 1) { s->fd = fds[0]; for (int i = 1; i < nfd; i++) close(fds[i]); }
    s->map = s->fd >= 0 ? mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0) : MAP_FAILED;
    VkAndroidHardwareBufferPropertiesANDROID ap = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    int ok = s->map != MAP_FAILED && !vkGetAndroidHardwareBufferPropertiesANDROID(V.dev, s->ahb, &ap) && !vkCreateBuffer(V.dev, &bi, NULL, &s->buf);
    if (ok) {
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, s->buf};
        VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded, s->ahb};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, ap.allocationSize, vk_mem_type(ap.memoryTypeBits, 0, 0)};
        ok = !vkAllocateMemory(V.dev, &ai, NULL, &s->mem);
        if (ok && vkBindBufferMemory(V.dev, s->buf, s->mem, 0)) { vkFreeMemory(V.dev, s->mem, NULL); ok = 0; }
        if (!ok) vkDestroyBuffer(V.dev, s->buf, NULL);
    }
    if (!ok) {
        static int said; if (!said++) VKLOG("[RED] memory pool slab of %llu KB could not be made: small pools get hardware buffers of their own", (unsigned long long)(size >> 10));
        if (s->map != MAP_FAILED) munmap(s->map, size); if (s->fd >= 0) close(s->fd); AHardwareBuffer_release(s->ahb); free(s); return NULL;
    }
    for (uint32_t i = 0; i < n; i++) s->bits[i >> 6] |= 1ull << (i & 63);
    return s;
}
static int slab_take(uint64_t size, slab_t **out, uint32_t *slot) {   // 1: a slot of at least size bytes
    if (!slab_on() || size > SLAB_POOL_MAX) return 0;
    uint64_t pages = (size + 4095) >> 12; int c = 0; while (c < SLAB_NCLS && SLAB_PAGES[c] < pages) c++;
    if (c == SLAB_NCLS) return 0;
    uint64_t done = vk_fence_done();
    for (int pass = 0; pass < 2; pass++) {
        pthread_mutex_lock(&g_slab_mu);
        for (slab_t *s = g_slab[c]; s; s = s->next) {
            if (!s->nfree) continue;
            for (uint32_t w = 0; w < (s->nslot + 63) / 64; w++) for (uint64_t b = s->bits[w]; b; b &= b - 1) {
                uint32_t i = w * 64 + (uint32_t)__builtin_ctzll(b); if (s->fence[i] > done) continue;
                s->bits[w] &= ~(1ull << (i & 63)); s->nfree--; g_slab_used++; pthread_mutex_unlock(&g_slab_mu); *out = s; *slot = i; return 1;
            }
        }
        pthread_mutex_unlock(&g_slab_mu);
        if (pass) break;
        slab_t *s = slab_new(c); if (!s) return 0;   // every slot taken or still the GPU's: a new slab, made outside the lock
        pthread_mutex_lock(&g_slab_mu); s->next = g_slab[c]; g_slab[c] = s; g_slab_bytes += s->slot_bytes * s->nslot; pthread_mutex_unlock(&g_slab_mu);
    }
    return 0;
}
static void slab_give(slab_t *s, uint32_t i, uint64_t fence) {
    pthread_mutex_lock(&g_slab_mu); s->fence[i] = fence; s->bits[i >> 6] |= 1ull << (i & 63); s->nfree++; pthread_mutex_unlock(&g_slab_mu);
}
// Zero copy: the pages of [mem, mem+size) become an AHardwareBuffer's, imported as a VkBuffer (a slab's slot for a small pool, or a kept buffer of a freed pool of that size). 0 on success
static int pool_zero_copy(vkpool_t *p, uint8_t *mem, uint64_t size) {
    slab_t *sl; uint32_t si;
    if (slab_take(size, &sl, &si)) {
        uint64_t off = (uint64_t)si * sl->slot_bytes;
        memcpy(sl->map + off, mem, size);
        if (mmap(mem, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, sl->fd, (off_t)off) == mem) {
            p->buf = sl->buf; p->mem = VK_NULL_HANDLE; p->ahb = NULL; p->boff = off; p->slab = sl; p->slot = si; p->zero_copy = 1; return 0; }
        mem_diag_zc(11, "mmap-fixed slab", size, "errno", errno); slab_give(sl, si, 0);   // (the anonymous pages are still there: a buffer of its own)
    }
    AHardwareBuffer_Desc d = {(uint32_t)size, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, 0, 0, 0};
    if (size > 0xffffffffull) { mem_diag_zc(0, "size", size, "limit", -1); return -1; }
    zcfree_t re; int reuse = zcf_take(size, &re);
    AHardwareBuffer *ahb = reuse ? re.ahb : NULL; int ar = reuse ? 0 : AHardwareBuffer_allocate(&d, &ahb);
    if (ar) { mem_diag_zc(1, "AHardwareBuffer_allocate", size, "return", ar); return -1; }
#define ZC_DROP() do { if (reuse) zcf_free_now(&re); else AHardwareBuffer_release(ahb); } while (0)
    int sv[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
        mem_diag_zc(2, "socketpair", size, "errno", errno); ZC_DROP(); return -1; }
    int fds[16], nfd = -1;
    ar = AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0]);
    if (!ar) { nfd = recv_fds(sv[1], fds, 16); if (nfd < 1) mem_diag_zc(4, "recv_fds", size, "return", nfd); }
    else mem_diag_zc(3, "AHardwareBuffer_sendHandleToUnixSocket", size, "return", ar);
    close(sv[0]); close(sv[1]);
    if (nfd < 1) { ZC_DROP(); return -1; }
    // copy the pool's current contents into the buffer through a temporary mapping, then put the buffer's pages at the pool's address
    uint8_t *tmp = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    int ok = tmp != MAP_FAILED;
    if (ok) {
        memcpy(tmp, mem, size); munmap(tmp, size);
        ok = mmap(mem, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fds[0], 0) == mem;
        if (!ok) mem_diag_zc(6, "mmap-fixed", size, "errno", errno);
    } else mem_diag_zc(5, "mmap-temporary", size, "errno", errno);
    for (int i = 0; i < nfd; i++) close(fds[i]);
    if (!ok) { ZC_DROP(); return -1; }
#undef ZC_DROP
    if (reuse) { p->buf = re.buf; p->mem = re.mem; p->ahb = ahb; p->zero_copy = 1; return 0; }   // imported already
    VkAndroidHardwareBufferPropertiesANDROID ap = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    VkResult vr = vkGetAndroidHardwareBufferPropertiesANDROID(V.dev, ahb, &ap);
    if (vr) { mem_diag_zc(7, "vkGetAndroidHardwareBufferPropertiesANDROID", size, "VkResult", vr); AHardwareBuffer_release(ahb); return -2; }
    vr = vkCreateBuffer(V.dev, &bi, NULL, &p->buf);
    if (vr) { mem_diag_zc(8, "vkCreateBuffer", size, "VkResult", vr); AHardwareBuffer_release(ahb); return -2; }
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, p->buf};   // the driver requires a dedicated allocation for an imported AHardwareBuffer
    VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded, ahb};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, ap.allocationSize, vk_mem_type(ap.memoryTypeBits, 0, 0)};
    vr = vkAllocateMemory(V.dev, &ai, NULL, &p->mem);
    if (!vr) { vr = vkBindBufferMemory(V.dev, p->buf, p->mem, 0); if (vr) mem_diag_zc(10, "vkBindBufferMemory", size, "VkResult", vr); }
    else mem_diag_zc(9, "vkAllocateMemory", size, "VkResult", vr);
    if (vr) { vkDestroyBuffer(V.dev, p->buf, NULL); AHardwareBuffer_release(ahb); return -2; }
    p->ahb = ahb; p->zero_copy = 1;
    return 0;
}
// A zero-copy pool's dma-buf mapped again over [mem, mem+len) in one piece per chunk, read-only (nvn_dirty_defrag: the mappings its protection split never merge). 0 = done
int vk_pool_remap_ro(void *mem, size_t len) {
    int rc = 0; uint64_t done = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool && !rc; i++) {
        vkpool_t *p = &g_pool[i]; if (p->base != (uint64_t)(uintptr_t)mem || p->end - p->base != len) continue;
        if (!p->zero_copy) { if (!p->shadow) { rc = -1; break; } done += p->hi - p->lo; continue; }   // a copied pool: nothing to map again (its protection stays as it is), and done, so defrag stops taking its pages busy
        if (p->slab) { void *at = (void *)(uintptr_t)p->lo; rc = mmap(at, p->hi - p->lo, PROT_READ, MAP_SHARED | MAP_FIXED, ((slab_t *)p->slab)->fd, (off_t)p->boff) == at ? 0 : -1; if (!rc) done += p->hi - p->lo; continue; }   // a slab's slot
        int sv[2], fds[16], nfd = -1; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { rc = -1; break; }
        if (!AHardwareBuffer_sendHandleToUnixSocket(p->ahb, sv[0])) nfd = recv_fds(sv[1], fds, 16);
        close(sv[0]); close(sv[1]);
        void *at = (void *)(uintptr_t)p->lo; rc = nfd >= 1 && mmap(at, p->hi - p->lo, PROT_READ, MAP_SHARED | MAP_FIXED, fds[0], 0) == at ? 0 : -1;
        for (int k = 0; k < nfd; k++) close(fds[k]);
        if (!rc) done += p->hi - p->lo;
    }
    pthread_rwlock_unlock(&g_pool_mu);
    return !rc && done == len ? 0 : -1;
}
static void pool_copy_back(vkpool_t *p) {   // the pages go back to anonymous memory with the buffer's contents (the game may reuse the memory)
    uint64_t size = p->hi - p->lo; uint8_t *mem = (uint8_t *)(uintptr_t)p->lo;
    uint8_t *save = malloc(size); if (save) memcpy(save, mem, size);
    mmap(mem, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (save) { memcpy(mem, save, size); free(save); }
}
// Pools over ZC_CHUNK are made of chunks of at most that size, each a pool entry with an AHardwareBuffer of its own: the Adreno 830's gralloc allocator service
// (vendor.qti.hardware.display.allocator-service, snapalloc) aborts on a BLOB of 812 MB ("ubsan: mul-overflow" in SnapConstraintManager::AlignmentToAlignedConstraints),
// and the whole Android UI restarts with it; 400 MB ones work (13 Sentinels: Aegis Rim's 0x32c00000-byte pool). A range across two chunks has no buffer (vk_pool_of)
#define ZC_CHUNK 0x10000000ull
static void pool_add_one(const void *mem, uint64_t size, uint64_t base, uint64_t end);
// DM_VK_ZEROCOPY_MAX_MB=<n>: pools bigger than n MB are copied, not zero-copy, and kept whole (a copied pool takes no hardware buffer, so no chunks). The Legion Y700
// 2023's gralloc aborts the process (no error returned) allocating a hardware buffer for Unicorn Overlord's 860 MB pool, while its pools up to 250 MB are fine
static int zc_over(uint64_t size) { static uint64_t m = 1; if (m == 1) { const char *e = getenv("DM_VK_ZEROCOPY_MAX_MB"); m = e && atoi(e) > 0 ? (uint64_t)atoi(e) << 20 : 0; } return m && size > m; }
void mtl_pool_add(const void *mem, uint64_t size) {
    vk_init();
    if (!size) return;
    uint64_t a = (uint64_t)(uintptr_t)mem, c = ((a | size) & 0xfff) || zc_over(size) ? size : ZC_CHUNK;
    for (uint64_t o = 0; o < size; o += c) pool_add_one((const uint8_t *)mem + o, size - o < c ? size - o : c, a, a + size);
    if (size > c) VKLOG("memory pool %p +%#llx: %llu chunks of at most %#llx", mem, (unsigned long long)size, (unsigned long long)((size + c - 1) / c), c);
}
// The pool line: pools made and dropped with their time on the calling threads and the reuse counts, at most once a second and only for a second in which
// pools took 20 ms or more (loads): "memory pools in the last 1.0 s: 212 made (148.0 MB, 640.0 ms; 150 from kept buffers), 180 dropped (...)"
static uint64_t pool_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static pthread_mutex_t g_pn_mu = PTHREAD_MUTEX_INITIALIZER; static uint64_t g_pn_t0, g_pn_n[2], g_pn_b[2], g_pn_ns[2], g_pn_hit0, g_pn_slab0;
static void pool_note(int kind, uint64_t bytes, uint64_t ns) {   // kind 0: made, 1: dropped (lock order: g_pn_mu, then g_zcf_mu or g_slab_mu)
    uint64_t now = pool_ns(); char b[400] = "";
    pthread_mutex_lock(&g_pn_mu);
    if (!g_pn_t0) g_pn_t0 = now;
    g_pn_n[kind]++; g_pn_b[kind] += bytes; g_pn_ns[kind] += ns;
    if (now - g_pn_t0 >= 1000000000ull) {
        pthread_mutex_lock(&g_zcf_mu); uint64_t hit = g_zcf_hit, kn = (uint64_t)g_nzcf, kb = g_zcf_bytes; pthread_mutex_unlock(&g_zcf_mu);
        pthread_mutex_lock(&g_slab_mu); uint64_t su = g_slab_used, sbytes = g_slab_bytes; pthread_mutex_unlock(&g_slab_mu);
        if (g_pn_ns[0] + g_pn_ns[1] >= 20000000ull)
            snprintf(b, sizeof b, "memory pools in the last %.1f s: %llu made (%.1f MB, %.1f ms; %llu in slab slots, %llu from kept buffers), %llu dropped (%.1f MB, %.1f ms); slabs %.1f MB, %llu buffers kept (%.1f MB)",
                     (now - g_pn_t0) / 1e9, (unsigned long long)g_pn_n[0], g_pn_b[0] / 1048576.0, g_pn_ns[0] / 1e6, (unsigned long long)(su - g_pn_slab0), (unsigned long long)(hit - g_pn_hit0),
                     (unsigned long long)g_pn_n[1], g_pn_b[1] / 1048576.0, g_pn_ns[1] / 1e6, sbytes / 1048576.0, (unsigned long long)kn, kb / 1048576.0);
        g_pn_t0 = now; g_pn_hit0 = hit; g_pn_slab0 = su; memset(g_pn_n, 0, sizeof g_pn_n); memset(g_pn_b, 0, sizeof g_pn_b); memset(g_pn_ns, 0, sizeof g_pn_ns);
    }
    pthread_mutex_unlock(&g_pn_mu);
    if (*b) VKLOG("%s", b);
}
// The pool's buffer is made before the write lock is taken, and the entry only put in under it: the hardware buffer (a binder call to the allocator service), the copy
// into it and the driver's import take milliseconds each, and every lookup waits on the lock (vk_pool_of: each buffer binding of each draw on the replay thread, and the
// game threads' mtl_addr, once a pool was added or removed). Pokémon Scarlet makes and drops pools on three threads at once while a thrown ball flies (its capture assets,
// ~180 MB read in 1.5 s): the replay thread stood almost still for a second with the GPU idle (simpleperf), in frames of 100-580 ms
static void pool_add_one(const void *mem, uint64_t size, uint64_t base, uint64_t end) {
    uint64_t t0 = pool_ns();
    vkpool_t q, *p = &q; memset(p, 0, sizeof *p);
    p->lo = (uint64_t)(uintptr_t)mem; p->hi = p->lo + size; p->base = base; p->end = end;
    int zc = ((uintptr_t)mem & 0xfff) || (size & 0xfff) ? -3 : getenv("DM_VK_NO_ZEROCOPY") ? -4 : zc_over(end - base) ? -5 : pool_zero_copy(p, (uint8_t *)mem, size);
    if (zc) {
        static int said; if (!said++) VKLOG("[RED] memory pool %p +%#llx could not be zero-copy (%d): a GPU copy is refreshed before each submission (slow)", mem, (unsigned long long)size, zc);
        const char *e = getenv("DM_VK_POOL_CACHED"); int cached = e && atoi(e);
        VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
        if (cached && vkFlushMappedMemoryRanges && vkInvalidateMappedMemoryRanges) r = pool_copied_alloc(p, size, 1);
        if (r == VK_ERROR_DEVICE_LOST) VKCHK(r);
        if (r) {
            if (cached && mem_diag_on()) VKLOG("[memdiag] cached pool unavailable (%d), using coherent memory", r);
            VKCHK(pool_copied_alloc(p, size, 0));
        }
    }
    pthread_rwlock_wrlock(&g_pool_mu);
    if (g_npool == MAXPOOL) { pthread_rwlock_unlock(&g_pool_mu); die("[vk] too many memory pools"); }
    { static uint64_t ser; p->serial = ++ser; }
    g_pool[g_npool] = q; pool_ord_insert(g_npool++); __atomic_fetch_add(&g_pool_gen, 1, __ATOMIC_RELEASE);
    int np = g_npool;
    pthread_rwlock_unlock(&g_pool_mu);
    if (np >= 1024 && !(np & (np - 1))) VKLOG("%d memory pools live", np);
    static uint64_t zc_bytes, cp_bytes; if (zc) cp_bytes += size; else zc_bytes += size;
    static int nlog; if (nlog++ < 24) VKLOG("memory pool %p +%#llx: %s (zero-copy %.1f MB, copied %.1f MB)", mem, (unsigned long long)size, zc ? "copied" : "zero-copy", zc_bytes / 1048576.0, cp_bytes / 1048576.0);
    pool_note(0, size, pool_ns() - t0);
}
void mtl_pool_del(const void *mem) {
    for (;;) {   // every chunk of the pool
        pthread_rwlock_wrlock(&g_pool_mu);
        int i = 0; while (i < g_npool && g_pool[i].base != (uint64_t)(uintptr_t)mem) i++;
        if (i == g_npool) { pthread_rwlock_unlock(&g_pool_mu); return; }
        vkpool_t p = g_pool[i]; g_pool[i] = g_pool[--g_npool]; pool_ord_remove(i); __atomic_fetch_add(&g_pool_gen, 1, __ATOMIC_RELEASE);
        pthread_rwlock_unlock(&g_pool_mu);
        uint64_t t0 = pool_ns();
        if (p.zero_copy) pool_copy_back(&p);
        free(p.baseline);
        { void vk_tbuf_drop_pool(uint64_t); vk_tbuf_drop_pool(p.serial); }   // the texel buffer views over its buffer (gfx_vk_tex.c), before the buffer itself goes or is kept
        if (p.slab) slab_give(p.slab, p.slot, mtl_last_fence() + 1);   // the slot goes back to its slab (the GPU may still read it until that submission is done)
        else if (!(p.zero_copy && p.ahb && zcf_put(p.ahb, p.buf, p.mem, p.hi - p.lo))) {   // (kept for the next pool of its size)
            vk_defer_free_buffer(p.buf, p.mem);   // the AHardwareBuffer goes with the imported memory's last reference
            if (p.ahb) AHardwareBuffer_release(p.ahb);
        }
        pool_note(1, p.hi - p.lo, pool_ns() - t0);
    }
}
vkpool_t *vk_pool_of(uint64_t addr, uint64_t len) {
    // the last pools this thread found, while none was added or removed: the replay looks up every buffer binding of every draw, and the read lock alone was ~6% of it
    static __thread struct { uint32_t gen; vkpool_t *p[4]; } c;   // gen: g_pool_gen + 1 (0: empty)
    uint32_t g = __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE) + 1;
    if (c.gen == g) for (int i = 0; i < 4; i++) { vkpool_t *p = c.p[i]; if (p && addr >= p->lo && addr + len <= p->hi) return p; }
    vkpool_t *r = NULL; int across = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    int lo = 0, hi = g_npool;   // the last pool by lo that starts at or below addr
    while (lo < hi) { int m = (lo + hi) / 2; if (g_pool[g_pool_ord[m]].lo <= addr) lo = m + 1; else hi = m; }
    if (lo > 0) { vkpool_t *p = &g_pool[g_pool_ord[lo - 1]]; if (addr + len <= p->hi) r = p; }
    if (!r)   // not in that one: the scan as before (pools over the same memory, and the range across two chunks it reports)
        for (int i = 0; i < g_npool; i++) if (addr >= g_pool[i].lo && addr + len <= g_pool[i].hi) { r = &g_pool[i]; break; }
                                           else if (addr >= g_pool[i].lo && addr < g_pool[i].hi && addr + len <= g_pool[i].end) across = 1;
    pthread_rwlock_unlock(&g_pool_mu);
    if (r) { if (c.gen != g) { memset(&c, 0, sizeof c); c.gen = g; } memmove(&c.p[1], &c.p[0], 3 * sizeof *c.p); c.p[0] = r; }
    if (!r && across) { static uint64_t n; uint64_t k = ++n; if (!(k & (k - 1))) LOG("[RED] [vk] %llu ranges across two chunks of a memory pool (ZC_CHUNK; this one %#llx +%#llx): no buffer for them", (unsigned long long)k, (unsigned long long)addr, (unsigned long long)len); }
    return r;
}
uint32_t vk_pool_gen(void) { return __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE); }   // pools added or removed (gfx_vk_exec.c's descriptor set memo: vk_pool_of's results hold while it is unchanged)
int vk_buf_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    vkpool_t *p = vk_pool_of(addr, len ? len : 1);
    if (!p) return 0;
    *buf = p->buf; *off = addr - p->lo + p->boff; return 1;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    vkpool_t *p = vk_pool_of(addr, len ? len : 1);
    if (!p) return 0;
    if (buf) *buf = (void *)(uintptr_t)p->lo; if (off) *off = addr - p->lo; return 1;
}
void mtl_alias_drop_pool(const void *pool) { void vk_tex_drop_pool(const void *); vk_tex_drop_pool(pool); }
// The NVN flags of the pool at mem (hle_nvn.c, after mtl_pool_add): a copied pool the CPU maps uncached (CPU_UNCACHED, 0x2) is one the game writes and the GPU reads;
// the game reads GPU results from cached memory, so such a pool is not copied back after each submission
void vk_pool_set_flags(const void *mem, int flags) {
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pool[i].base == (uint64_t)(uintptr_t)mem) g_pool[i].flags = flags;   // (every chunk of it)
    pthread_rwlock_unlock(&g_pool_mu);
}
static uint64_t g_sync_bytes, g_sync_full;   // copied pools: bytes copied by the incremental sync, full copies
// Copied pools: the guest's bytes into the GPU copy. The first sync copies the whole pool; later ones copy only the 64 KB blocks with pages the CPU wrote since the last
// sync (nvn_dirty's write-protect tracking: a block is protected again before it is copied, so a write during the copy is caught by the next sync). An 860 MB
// pool copied whole at every submission (Unicorn Overlord on a Legion Y700 2023) ran at a few frames a second
static void vk_pools_sync_(VkCommandBuffer cb);
void vk_pools_sync(VkCommandBuffer cb) { extern int g_prof_on; extern uint64_t g_prof_ns[8]; struct timespec a, b; if (g_prof_on) clock_gettime(CLOCK_MONOTONIC, &a);
    int diag = mem_diag_on(); uint64_t t = diag ? mem_diag_ns() : 0, bytes = diag ? g_sync_bytes : 0;
    vk_pools_sync_(cb); if (diag) mem_diag_note(1, t, g_sync_bytes - bytes);
    if (g_prof_on) { clock_gettime(CLOCK_MONOTONIC, &b); g_prof_ns[2] += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)); } }
static void vk_pools_sync_(VkCommandBuffer cb) {
    (void)cb;
    int diag = mem_diag_on(); uint64_t ns[4] = {0}, amount[4] = {0};
    uint64_t reason_count[4] = {0}, reason_bytes[4] = {0};
#define SYNC_MEASURE(k, n, op) do { if (diag) { uint64_t t_ = mem_diag_ns(); op; ns[k] += mem_diag_ns() - t_; amount[k] += (n); } else { op; } } while (0)
    enum { BLK = 64 << 10 };
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) {
        vkpool_t *p = &g_pool[i]; if (!p->shadow) continue;
        uint8_t *lo = (uint8_t *)(uintptr_t)p->lo; uint64_t size = p->hi - p->lo, st = nvn_dirty_now();
        if (!p->baseline) { p->baseline = malloc(size); if (!p->baseline) die("[RED] [vk] copied-pool baseline allocation failed (%llu bytes)", (unsigned long long)size);
            p->synced = 0; if (mem_diag_on()) VKLOG("[memdiag] copy-back baseline allocated: %.2f MiB", size / 1048576.0); }
        if (!p->span && (p->span = nvn_dirty_span(lo, size))) p->synced = 0;   // tracked only now (nvnMemoryPoolInitialize registers the range after the pool): copied whole once more, protected
        if (!p->synced || !p->span) {   // untracked: every sync copies all of it, as before
            if (diag) { int reason = !p->synced ? 0 : 1; reason_count[reason]++; reason_bytes[reason] += size; }
            if (p->span) SYNC_MEASURE(1, size, nvn_dirty_protect(lo, size));
            SYNC_MEASURE(2, size, memcpy(p->shadow, lo, size)); SYNC_MEASURE(3, size, memcpy(p->baseline, p->shadow, size)); pool_cache_sync(p, 0, 0, size); g_sync_bytes += size; g_sync_full++; p->synced = 1; p->baseline_complete = 0; p->stamp0 = p->stamp = st; continue; }
        // judged against the sync before the last: a write whose fault took its generation before the last sync's stamp but stored it after that sync looked
        // at its block would otherwise never count (its page is writable from then on, and clean by every later stamp). A dirty block is copied twice
        uint64_t first = size, last = 0;
        uint64_t scan_start = diag ? mem_diag_ns() : 0, work_start = ns[1] + ns[2] + ns[3];
        for (uint64_t off = 0; off < size; off += BLK) {
            uint64_t n = size - off < BLK ? size - off : BLK;
            if (!nvn_dirty_since_span(p->span, lo + off, n, p->stamp0)) continue;
            SYNC_MEASURE(1, n, nvn_dirty_protect(lo + off, n));
            SYNC_MEASURE(2, n, memcpy(p->shadow + off, lo + off, n)); g_sync_bytes += n;
            if (off < first) first = off; last = off + n;
            if (p->baseline_complete) SYNC_MEASURE(3, n, memcpy(p->baseline + off, p->shadow + off, n));
        }
        // A full merge already refreshed the baseline; only uploaded blocks changed. A pool synced again with no merge since (no completion) takes a full snapshot.
        if (diag) { ns[0] += mem_diag_ns() - scan_start - (ns[1] + ns[2] + ns[3] - work_start); amount[0] += size; }
        if (!p->baseline_complete) {
            if (diag) { int reason = p->flags & 0x2 ? 2 : 3; reason_count[reason]++; reason_bytes[reason] += size; }
            SYNC_MEASURE(3, size, memcpy(p->baseline, p->shadow, size));
        }
        if (last) pool_cache_sync(p, 0, first, last);
        p->baseline_complete = 0;
        p->stamp0 = p->stamp; p->stamp = st;
    }
    pthread_rwlock_unlock(&g_pool_mu);
    if (diag) for (unsigned k = 0; k < 4; k++) mem_diag_note(6 + k, mem_diag_ns() - ns[k], amount[k]);
    if (diag) mem_diag_baselines(reason_count, reason_bytes);
#undef SYNC_MEASURE
    static uint64_t n, last; if (++n % 600 == 0) { VKLOG("copied pools: %.1f MB synced in the last 600 submissions (%.2f MB each), %llu full copies so far", (g_sync_bytes - last) / 1048576.0, (g_sync_bytes - last) / 600.0 / 1048576.0, (unsigned long long)g_sync_full); last = g_sync_bytes; }
}
// Copied pools after a submission: merge GPU changes against their baseline, preserving independent CPU writes made after the sync. Never-synced pools are left alone.
// A CPU-uncached pool (vk_pool_set_flags) takes back only the ranges the submission's GPU writes covered (sub_gpu_wrote):
// copying 860 MB back after every submission held Unicorn Overlord to a few frames a second
struct vksub; static void pools_copy_back(const struct vksub *s);

// ---------------- submission slots ----------------
#define NSLOT 6
#define MAIN_CBS 32   // (allocated as used: DM_VK_PASS_SPLIT cuts inside passes as well)
// GPU time: a timestamp at the start and end of each submission's main command buffer (one queue: their sum is the time the GPU was busy)
static VkQueryPool g_tsq; uint64_t g_gpu_ns;
// Timestamp counters (ReportCounter TIMESTAMP): each writes a GPU timestamp where it is in the command stream (CQ per slot), and at completion it gets the guest
// clock now less how long before the submission's end the GPU passed it. They used to all get the completion time of their submission: a game timing its GPU
// work between two counters read 0 when both were in one submission and the gap between completions (our CPU replay included) when not. Xenoblade Chronicles 3
// sizes its frame by that time (dynamic resolution): its main pass went between 960x544 and 776x440 every few frames while the camera turned; with GPU times
// the size holds steady
enum { CQ = 64 };
static VkQueryPool g_cq;
#define SCRATCH_BLOCK (8u << 20)
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize size, used; } scratch_blk;
typedef struct { int kind; void *a; void *b; } dfree_t;   // 0 buffer+memory, 1 image+memory, 2 view
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; int q; } post_op;   // q: a timestamp counter's query in g_cq, or -1
struct vksub {
    VkCommandPool pool; VkCommandBuffer up, main; int up_used;
    VkCommandBuffer mains[MAIN_CBS]; int nmain_alloc, nmain_used, cb_err;
    VkCommandBuffer ups[MAIN_CBS]; int nup_alloc, nup_used; int nmain_sent;   // upload command buffers in order (one per part sent early), main buffers already submitted   // the submission's main command buffers in order (vk_cmd_split); main = the one being recorded
    uint64_t fence;   // the fence this slot last committed with (0 = never)
    int ncq;          // timestamp counter queries used in this submission (g_cq)
    scratch_blk *blk; int nblk, capblk;
    VkDescriptorPool *dp; int ndp, capdp, dp_cur;
    uint32_t dp_used[9], dp_sets; int dp_counted; int no_ts;   // what pool dp_cur has given out (vk_alloc_set)
    dfree_t *df; int ndf, capdf;
    post_op *post; int npost, cappost;
    struct { uint64_t lo, hi; } gw[64]; int ngw, readback_ranges;   // GPU writes into pool memory in this submission (sub_gpu_wrote), for pools_copy_back: nearby ones merged, and once full each joins the nearest
};
enum { GW_SUB = 64 };
static vksub_t g_slot[NSLOT]; static int g_cur = -1; uint64_t g_cmd_gen;   // g_cmd_gen: main command buffers begun (gfx_vk_exec.c bind cache)
static pthread_mutex_t g_df_mu = PTHREAD_MUTEX_INITIALIZER;   // the slots' deferred-free lists (df, ndf, capdf): df_add says why
static uint64_t pool_merge_back(vkpool_t *p, uint64_t lo, uint64_t hi) {
    int diag = mem_diag_on(); uint64_t t = diag ? mem_diag_ns() : 0;
    uint64_t bytes = 0;
    uint8_t after[4096];
    // A whole-pool copy overwrote new CPU data even outside GPU writes (reproduced with independent bytes in one cache line).
    for (uint64_t off = lo - p->lo, end = hi - p->lo; off < end; ) {
        size_t n = end - off < sizeof after ? (size_t)(end - off) : sizeof after;
        // Direct scans of mapped memory reached 1 s on Mali; stage once so byte comparisons use CPU memory.
        memcpy(after, p->shadow + off, n);
        const uint8_t *src = after; uint8_t *base = p->baseline + off, *dst = (uint8_t *)(uintptr_t)(p->lo + off);
        if (memcmp(base, src, n)) {
            dm_guest_write_prepare(dst, n);
            for (size_t i = 0; i < n; ) {
                if (base[i] == src[i]) { i++; continue; }
                size_t a = i++; while (i < n && base[i] != src[i]) i++;
                memcpy(dst + a, src + a, i - a); memcpy(base + a, src + a, i - a); bytes += i - a;
            }
        }
        off += n;
    }
    if (diag) mem_diag_note(5, t, hi - lo);
    return bytes;
}
static void pools_copy_back(const struct vksub *s) {
    int diag = mem_diag_on(); uint64_t t = diag ? mem_diag_ns() : 0, bytes = 0;
    uint64_t invalidate_ns = 0, invalidate_calls = 0, invalidate_bytes = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) { vkpool_t *p = &g_pool[i]; if (!p->shadow || !p->synced) continue;
        uint64_t first = 0, last = p->hi - p->lo;
        // Mali: full invalidation took 13 ms per submission; eligible ranges cover every GPU write.
        // Other shadow bytes stay unchanged; the next sync overwrites whole dirty blocks before snapshotting them.
        if (s->readback_ranges) {
            first = last; last = 0;
            for (int k = 0; k < s->ngw; k++) { uint64_t lo = s->gw[k].lo > p->lo ? s->gw[k].lo : p->lo, hi = s->gw[k].hi < p->hi ? s->gw[k].hi : p->hi;
                if (lo < hi) { if (lo - p->lo < first) first = lo - p->lo; if (hi - p->lo > last) last = hi - p->lo; } }
        }
        if (first < last) {
            uint64_t start = diag && p->noncoherent ? mem_diag_ns() : 0;
            pool_cache_sync(p, 1, first, last);
            if (diag && p->noncoherent) { invalidate_ns += mem_diag_ns() - start; invalidate_calls++; invalidate_bytes += last - first; }
        }
        if (!(p->flags & 0x2) && !s->readback_ranges) { bytes += pool_merge_back(p, p->lo, p->hi); p->baseline_complete = 1; continue; }
        // The baseline stays complete after a merge of the tracked ranges, for NVN CPU_UNCACHED pools too after a submission with an unknown writer: those pools
        // trust the tracked ranges, as before the baseline (an untracked GPU write stays in the GPU copy only and is merged with a later tracked range over it).
        // Taking a full snapshot at the next sync instead copied the whole pool before every submission: Unicorn Overlord's 860 MB CPU_UNCACHED pool, as before
        p->baseline_complete = 1;
        for (int k = 0; k < s->ngw; k++) { uint64_t lo = s->gw[k].lo > p->lo ? s->gw[k].lo : p->lo, hi = s->gw[k].hi < p->hi ? s->gw[k].hi : p->hi;
            if (lo < hi) bytes += pool_merge_back(p, lo, hi); } }
    pthread_rwlock_unlock(&g_pool_mu);
    if (diag) {
        uint64_t total = mem_diag_ns() - t;
        mem_diag_note(2, t, bytes);
        mem_diag_copyback(total, invalidate_ns, invalidate_calls, invalidate_bytes);
        static uint64_t seen, narrowed; if (s->readback_ranges) narrowed++;
        if (++seen <= 3 || seen % 600 == 0) VKLOG("[memdiag] readback coverage: %llu range submissions / %llu total (others retain full cached-pool merges)", (unsigned long long)narrowed, (unsigned long long)seen);
    }
}
int vk_readback_ranges(void) { return g_cur >= 0 && g_slot[g_cur].readback_ranges; }
void vk_readback_unknown(void) { if (g_cur >= 0) g_slot[g_cur].readback_ranges = 0; }
static VkCommandBuffer g_slot_main_cb(void) { return g_slot[g_cur].main; }
// DM_VK_PASS_TIMES=1: a timestamp before and after every render pass (the replay's per-frame line then lists the passes that cost the most GPU time)
enum { PASSQ = 512 };   // passes (and dispatches) timed per submission
static VkQueryPool g_pq; static int g_pq_on = -1;
typedef struct { uint32_t w, h, fmt, draws, fidx; } pass_meta;   // fidx: the pass's place in its frame (submissions are cut every DM_VK_CB_DRAWS draws)
enum { FPASSQ = 512 }; static int g_fpass;   // passes of the frame so far
void vk_pass_frame(void) { g_fpass = 0; }   // a present: the next pass is the frame's first
static pass_meta g_pmeta[NSLOT][PASSQ]; static int g_npass[NSLOT];
static double g_pacc[FPASSQ]; static uint32_t g_pcnt[FPASSQ]; static pass_meta g_plast[FPASSQ]; static int g_pmax;   // by the pass's place in its frame (the completion thread adds)
static int pass_times_on(void) { if (g_pq_on < 0) { const char *e = getenv("DM_VK_PASS_TIMES"); g_pq_on = e && atoi(e); } return g_pq_on && g_pq; }
void vk_pass_ts(int end, uint32_t w, uint32_t h, uint32_t fmt, uint32_t draws) {
    if (!pass_times_on() || g_cur < 0) return; int *n = &g_npass[g_cur]; if (*n >= PASSQ) return;
    vkCmdWriteTimestamp(g_slot_main_cb(), end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_pq, (uint32_t)(g_cur * PASSQ * 2 + *n * 2 + end));
    if (end) { g_pmeta[g_cur][*n] = (pass_meta){w, h, fmt, draws, (uint32_t)(g_fpass < FPASSQ - 1 ? g_fpass : FPASSQ - 1)}; (*n)++; g_fpass++; }
}
void vk_pass_report(void) {   // the most expensive passes since the last report, then the counters start again
    if (!pass_times_on()) return;
    static int idx[FPASSQ]; int m = g_pmax; for (int i = 0; i < m; i++) idx[i] = i;
    for (int i = 0; i < m; i++) for (int j = i + 1; j < m; j++) if (g_pcnt[idx[j]] && (!g_pcnt[idx[i]] || g_pacc[idx[j]] / g_pcnt[idx[j]] > g_pacc[idx[i]] / g_pcnt[idx[i]])) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    double tot = 0; for (int i = 0; i < m; i++) if (g_pcnt[i]) tot += g_pacc[i] / g_pcnt[i];
    char b[3000]; int o = snprintf(b, sizeof b, "passes by GPU time (%d passes, %.2f ms in passes per frame):", m, tot);
    for (int i = 0; i < m && i < 20 && o < (int)sizeof b - 80; i++) { int k = idx[i]; if (!g_pcnt[k]) continue;
        if (g_plast[k].fmt == PT_DISPATCH) o += snprintf(b + o, sizeof b - (size_t)o, " #%d %.2f ms (dispatch %ux%ux%u);", k, g_pacc[k] / g_pcnt[k], g_plast[k].w, g_plast[k].h, g_plast[k].draws);
        else o += snprintf(b + o, sizeof b - (size_t)o, " #%d %.2f ms (%ux%u fmt %u, %u draws over %u px wide);", k, g_pacc[k] / g_pcnt[k], g_plast[k].w, g_plast[k].h, g_plast[k].fmt, g_plast[k].draws & 0xffff, g_plast[k].draws >> 16); }
    { extern uint64_t g_nvn_discards[2]; static uint64_t pd0, pd1; o += snprintf(b + o, sizeof b - (size_t)o, " discards since: colour %llu, depth %llu", (unsigned long long)(g_nvn_discards[0] - pd0), (unsigned long long)(g_nvn_discards[1] - pd1)); pd0 = g_nvn_discards[0]; pd1 = g_nvn_discards[1]; }
    VKLOG("%s", b);
    memset(g_pacc, 0, sizeof g_pacc); memset(g_pcnt, 0, sizeof g_pcnt); g_pmax = 0;
}
// one submission at a time: the game's (hle_nvn.c holds its own lock around them) and the backend's own (texel readback, present)
static pthread_mutex_t g_sub_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fence;   // fence of the last commit
static VkSemaphore g_timeline;   // submission f signals the value f (V.has_timeline), else g_fring[f % NFRING]
#define NFRING 16   // more than the submissions in flight (NSLOT: mtl_sub_begin's slot_recycle waits for the slot's last one)
static VkFence g_fring[NFRING];
static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_done;
static void slots_init(void) {
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    if (V.has_timeline) VKCHK(vkCreateSemaphore(V.dev, &si, NULL, &g_timeline));
    else for (int i = 0; i < NFRING; i++) { VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VKCHK(vkCreateFence(V.dev, &fi, NULL, &g_fring[i])); }
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * NSLOT};
    if (V.props.limits.timestampComputeAndGraphics && vkCreateQueryPool(V.dev, &qi, NULL, &g_tsq)) g_tsq = VK_NULL_HANDLE;
    VkQueryPoolCreateInfo cqi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, NSLOT * CQ};
    if (!g_tsq || vkCreateQueryPool(V.dev, &cqi, NULL, &g_cq)) g_cq = VK_NULL_HANDLE;
    { const char *e = getenv("DM_VK_PASS_TIMES"); if (e && atoi(e)) { VkQueryPoolCreateInfo pq = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, NSLOT * PASSQ * 2}; if (vkCreateQueryPool(V.dev, &pq, NULL, &g_pq)) g_pq = VK_NULL_HANDLE; } }
    for (int i = 0; i < NSLOT; i++) {
        // not TRANSIENT, and reset by command buffer (see slot_recycle)
        VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.qf};
        VKCHK(vkCreateCommandPool(V.dev, &ci, NULL, &g_slot[i].pool));
        VkCommandBuffer cb[2]; VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_slot[i].pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
        VKCHK(vkAllocateCommandBuffers(V.dev, &ai, cb)); g_slot[i].up = g_slot[i].ups[0] = cb[0]; g_slot[i].main = g_slot[i].mains[0] = cb[1]; g_slot[i].nmain_alloc = 1; g_slot[i].nup_alloc = 1;
    }
}
uint64_t vk_fence_done(void) { return __atomic_load_n(&g_done, __ATOMIC_ACQUIRE); }   // written under g_done_mu (for the waiters) with a release store
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
// Without timeline semaphores only the completion thread waits on the ring's fences, in submission order (a fence tells nothing of the submissions before
// it); everyone else waits for the completion thread to pass the fence (g_done), so a fence is never waited on while the commit resets it for reuse
static void wait_fence(uint64_t f) {
    if (!f) return;
    if (!V.has_timeline) { mtl_wait(f, ~0ull); return; }
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &g_timeline, &f};
    VkResult r = vkWaitSemaphores(V.dev, &wi, UINT64_MAX); if (r != VK_SUCCESS) die("[vk] GPU timeline wait did not complete: %d", r);
}
// The slot's command buffers are reset on the completion thread once its submission is done (completion_main), not when the replay begins them again: an
// implicit reset at vkBeginCommandBuffer took 7% of Xenoblade Chronicles 3's replay thread at a submission's start and more at every part (its battles
// replay ~3,000 draws a frame on a thread that is the frame's limit). The slot is then taken only after the completion thread has passed it (mtl_wait, g_done)
// rather than as soon as the GPU has. DM_VK_X_NOPRERESET=1 (start-up): reset as they are begun
static int g_prereset = -1;
static void slot_recycle(vksub_t *s) {   // the GPU has passed this slot's last submission: free what it held, rewind its allocators
    if (g_prereset < 0) { const char *e = getenv("DM_VK_X_NOPRERESET"); g_prereset = !(e && atoi(e)); }
    if (g_prereset) mtl_wait(s->fence, ~0ull); else wait_fence(s->fence);
    pthread_mutex_lock(&g_df_mu); dfree_t *df = s->df; int ndf = s->ndf; s->df = NULL; s->ndf = s->capdf = 0; pthread_mutex_unlock(&g_df_mu);   // the list taken whole: a game thread may add to it meanwhile
    for (int i = 0; i < ndf; i++) {
        dfree_t *d = &df[i];
        if (d->kind == 0) { vkDestroyBuffer(V.dev, (VkBuffer)d->a, NULL); if (d->b) vkFreeMemory(V.dev, (VkDeviceMemory)d->b, NULL); }
        else if (d->kind == 1) { vkDestroyImage(V.dev, (VkImage)d->a, NULL); if (d->b) vkFreeMemory(V.dev, (VkDeviceMemory)d->b, NULL); }
        else if (d->kind == 4) { void img_sub_free_(void *); vkDestroyImage(V.dev, (VkImage)d->a, NULL); img_sub_free_(d->b); }
        else if (d->kind == 2) vkDestroyImageView(V.dev, (VkImageView)d->a, NULL);
        else if (d->kind == 3) vkDestroyBufferView(V.dev, (VkBufferView)d->a, NULL);
    }
    free(df);
    for (int i = 0; i < s->nblk; i++) s->blk[i].used = 0;
    for (int i = 0; i < s->ndp; i++) vkResetDescriptorPool(V.dev, s->dp[i], 0);
    s->dp_cur = 0; s->dp_counted = -1;
    // The pool is not reset: each command buffer is reset as it is begun again (the pool allows it). A reset of the transient pool made Adreno's driver free the
    // command memory and map it again as the next submission grew (ioctl, mmap and munmap): in Xenoblade Chronicles 3's battles the submission's start took 21%
    // of the replay thread, now 9%, and frames went from about 42 to 38 ms
}
static vksub_t *cur(void) { if (g_cur < 0) die("[vk] no open submission"); return &g_slot[g_cur]; }
int vk_sub_open(void) { return g_cur >= 0; }
// A long submission is recorded into several command buffers, submitted together in order: Adreno's driver refuses a command buffer past some size at
// vkEndCommandBuffer (VK_ERROR_OUT_OF_HOST_MEMORY), which NEO's handheld frames reached at about the 712th draw. The caller has ended the render pass.
static void vk_cmd_split_(void);
static void pools_readback_barrier(VkCommandBuffer cb);
void vk_cmd_split(void) { extern int g_prof_on; extern uint64_t g_prof_ns[8]; struct timespec a, b; if (g_prof_on) clock_gettime(CLOCK_MONOTONIC, &a);
    vk_cmd_split_(); if (g_prof_on) { clock_gettime(CLOCK_MONOTONIC, &b); g_prof_ns[6] += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)); } }
static void vk_cmd_split_(void) {
    vksub_t *s = cur(); if (s->nmain_used == MAIN_CBS || s->nup_used == MAIN_CBS) return;
    pools_readback_barrier(s->main);
    if (vkEndCommandBuffer(s->main) < 0) { s->cb_err = 1; static int n; if (n++ < 3) VKLOG("[RED] a part of a long submission was refused at its end (part %d)", s->nmain_used); }
    // what is recorded so far goes to the GPU now, as a submission of its own (with its uploads first): Adreno refuses a single submission past about
    // 700 draws (NEO's handheld frames: VK_ERROR_OUT_OF_HOST_MEMORY at vkEndCommandBuffer whatever the draws were, and in one command buffer or several)
    const char *se = getenv("DM_VK_SUBMIT_SPLIT"); int send = !(se && !atoi(se));   // on unless DM_VK_SUBMIT_SPLIT=0
    if (send && !s->cb_err) {
        VkCommandBuffer cbs[2]; uint32_t n = 0;
        if (s->up_used) { vk_img_barrier_all(s->up); if (vkEndCommandBuffer(s->up) < 0) s->cb_err = 1; cbs[n++] = s->up; }
        cbs[n++] = s->main;
        if (!s->cb_err) {
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, n, cbs, 0, NULL};
            pthread_mutex_lock(&g_vk_queue_mu); VkResult r = vkQueueSubmit(V.q, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&g_vk_queue_mu);
            if (r) { void vk_crumbs_report(void); vk_crumbs_report(); die("[vk] vkQueueSubmit (part): %d (device lost?)", r); }
            { static int n; if (n++ < 3) VKLOG("a long submission: part %d sent early", s->nmain_used); }
            s->nmain_sent = s->nmain_used;
            if (s->up_used) {   // the next part records its uploads into a fresh upload buffer
                if (s->nup_used == s->nup_alloc) { VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
                    VKCHK(vkAllocateCommandBuffers(V.dev, &ai, &s->ups[s->nup_alloc])); s->nup_alloc++; }
                s->up = s->ups[s->nup_used++]; s->up_used = 0;
            }
        }
    }
    if (s->nmain_used == s->nmain_alloc) { VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VKCHK(vkAllocateCommandBuffers(V.dev, &ai, &s->mains[s->nmain_alloc])); s->nmain_alloc++; }
    s->main = s->mains[s->nmain_used++];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; VKCHK(vkBeginCommandBuffer(s->main, &bi)); g_cmd_gen++;
}   // outside a submission nothing may be recorded (present, a pool finalized between frames)
VkCommandBuffer vk_cmd(void) { return cur()->main; }
// CPU BC decoding needs the buffer's bytes at this exact replay point, including
// prior GPU writes. Flush and wait, then reuse the current command buffers rather
// than consuming a split slot for every texture upload.
int vk_buffer_snapshot(VkBuffer src, VkDeviceSize off, size_t len, void *dst) {
    vksub_t *s = cur();
    if (s->cb_err) { VKRED_ONCE(90, "BC readback skipped: submission already refused"); return 0; }
    VkBuffer b; VkDeviceSize o; void *p = vk_scratch(len, 16, &b, &o);
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    VkBufferCopy r = {off, o, len}; vkCmdCopyBuffer(cb, src, b, 1, &r);
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    pools_readback_barrier(s->main);
    if (vkEndCommandBuffer(s->main) < 0) goto refused;
    VkCommandBuffer cbs[MAIN_CBS + 1]; uint32_t n = 0;
    if (s->up_used) { vk_img_barrier_all(s->up); if (vkEndCommandBuffer(s->up) < 0) goto refused; cbs[n++] = s->up; }
    for (int i = s->nmain_sent; i < s->nmain_used; i++) cbs[n++] = s->mains[i];
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, n, cbs, 0, NULL};
    pthread_mutex_lock(&g_vk_queue_mu);
    VkResult result = vkQueueSubmit(V.q, 1, &si, VK_NULL_HANDLE);
    if (result == VK_SUCCESS) result = vkQueueWaitIdle(V.q);
    pthread_mutex_unlock(&g_vk_queue_mu);
    VKCHK(result);
    memcpy(dst, p, len); // scratch buffers use HOST_COHERENT memory
    // All pending work completed, so the last main/up buffers can be recorded
    // again. Earlier buffers stay marked as sent; final commit submits this tail.
    s->nmain_sent = s->nmain_used - 1;
    if (vkResetCommandBuffer(s->main, 0) < 0) goto refused;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkBeginCommandBuffer(s->main, &bi) < 0) goto refused;
    g_cmd_gen++;
    if (s->up_used) { if (vkResetCommandBuffer(s->up, 0) < 0) goto refused; s->up_used = 0; }
    { static int said; if (!said++) VKLOG("BC GPU snapshot: ordered readback completed (%zu bytes)", len); }
    return 1;
refused:
    s->cb_err = 1; // final commit uses the existing dropped-submission path
    VKRED_ONCE(91, "BC readback skipped: command buffer refused, submission will be dropped");
    // These buffers were never submitted, or the successful wait above finished
    // them. Rearm recording so later replay ops need not touch an ended buffer.
    if (vkResetCommandBuffer(s->main, 0) >= 0) {
        VkCommandBufferBeginInfo retry = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (vkBeginCommandBuffer(s->main, &retry) >= 0) g_cmd_gen++;
    }
    if (s->up_used) { vkResetCommandBuffer(s->up, 0); s->up_used = 0; }
    return 0;
}

int vk_cmd_parts_left(void) { vksub_t *s = cur(); return MAIN_CBS - (s->nmain_used > s->nup_used ? s->nmain_used : s->nup_used); }   // parts vk_cmd_split can still begin (gfx_vk_exec.c pass splits)
uint64_t g_up_gen;   // upload command buffers begun (gfx_vk_tex.c lin_copy: once per upload buffer)
VkCommandBuffer vk_upload_cmd(void) {
    vksub_t *s = cur();
    if (!s->up_used) { VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; VKCHK(vkBeginCommandBuffer(s->up, &bi)); s->up_used = 1; g_up_gen++; }
    return s->up;
}
void *vk_scratch(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    vksub_t *s = cur(); if (!align) align = 16;
    for (int i = 0; i < s->nblk; i++) {
        scratch_blk *b = &s->blk[i]; VkDeviceSize o = (b->used + align - 1) & ~(align - 1);
        if (o + n <= b->size) { b->used = o + n; *buf = b->b; *off = o; return b->p + o; }
    }
    if (s->nblk == s->capblk) { s->capblk = s->capblk ? 2 * s->capblk : 8; s->blk = realloc(s->blk, s->capblk * sizeof *s->blk); }
    scratch_blk *b = &s->blk[s->nblk++]; memset(b, 0, sizeof *b);
    static VkDeviceSize blk = 0; if (!blk) { const char *e = getenv("DM_VK_SCRATCH_MB"); blk = e && atoi(e) > 0 ? (VkDeviceSize)atoi(e) << 20 : SCRATCH_BLOCK; }
    b->size = n > blk ? (n + 0xffff) & ~0xffffull : blk;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, b->size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &b->b));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b->b, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
    VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &b->m)); VKCHK(vkBindBufferMemory(V.dev, b->b, b->m, 0));
    void *m; VKCHK(vkMapMemory(V.dev, b->m, 0, VK_WHOLE_SIZE, 0, &m)); b->p = m;
    b->used = n; *buf = b->b; *off = 0; return b->p;
}
// The deferred frees have a lock of their own: the replay thread adds to its slot's list while it records (holding g_sub_lock for the whole submission), and a game
// thread that finalizes a pool (nvnMemoryPoolFinalize → mtl_pool_del) adds to the same list without g_sub_lock. Unlocked, two first adds to a recycled slot crashed
// Animal Crossing: New Horizons' release build (SIGSEGV in df_add: one thread had set capdf, the other wrote through the df still NULL). g_df_mu is above slot_recycle
static void df_add(int kind, void *a, void *b) {
    pthread_mutex_lock(&g_df_mu);
    if (g_cur >= 0) {
        vksub_t *s = &g_slot[g_cur]; if (s->ndf == s->capdf) { s->capdf = s->capdf ? 2 * s->capdf : 64; s->df = realloc(s->df, s->capdf * sizeof *s->df); }
        s->df[s->ndf++] = (dfree_t){kind, a, b};
        pthread_mutex_unlock(&g_df_mu); return;
    }
    pthread_mutex_unlock(&g_df_mu);
    {   // outside a submission (a pool finalized between frames): after everything submitted so far
        wait_fence(g_fence);
        if (kind == 0) { vkDestroyBuffer(V.dev, (VkBuffer)a, NULL); if (b) vkFreeMemory(V.dev, (VkDeviceMemory)b, NULL); }
        else if (kind == 1) { vkDestroyImage(V.dev, (VkImage)a, NULL); if (b) vkFreeMemory(V.dev, (VkDeviceMemory)b, NULL); }
        else if (kind == 4) { void img_sub_free_(void *); vkDestroyImage(V.dev, (VkImage)a, NULL); img_sub_free_(b); }
        else if (kind == 2) vkDestroyImageView(V.dev, (VkImageView)a, NULL);
        else vkDestroyBufferView(V.dev, (VkBufferView)a, NULL);
    }
}
// Image memory. Small images share blocks: each allocation the driver makes is a mapping of the process (Adreno maps them all), and a game with thousands of textures
// ran towards the kernel's per-process mapping limit (Xenoblade Chronicles 3: about 9,400 driver mappings in its first area, 2,216 of them 172 KB). Images up to
// IMG_SLOT_MAX take a slot of a block (64 slots, 2 MB to IMG_BLOCK bytes), in size classes 2^(k/2) x 4 KB apart (a slot is at most 41% larger than the image); larger ones, or
// ones that need more than 4 KB alignment, get memory of their own
enum { IMG_CLASSES = 18, IMG_BLOCK = 32 << 20 };
#define IMG_SLOT_MAX ((VkDeviceSize)2 << 20)
typedef struct imgblk { VkDeviceMemory m; uint32_t type, cls, nslot, nfree; uint64_t bits[128]; struct imgblk *next; } imgblk;   // bits: 1 = slot free
typedef struct { imgblk *b; uint32_t slot; } imgsub;
static imgblk *g_imgblk[IMG_CLASSES]; static pthread_mutex_t g_imgblk_mu = PTHREAD_MUTEX_INITIALIZER;
static VkDeviceSize img_class_size(int c) { return (c & 1 ? 6144ull : 4096ull) << (c >> 1); }   // 4K, 6K, 8K, 12K, 16K, 24K, ... 2M
VkDeviceMemory vk_image_memory(const VkMemoryRequirements *req, uint32_t type, VkDeviceSize *off, void **sub) {
    *off = 0; *sub = NULL;
    int c = 0; while (c < IMG_CLASSES && img_class_size(c) < req->size) c++;
    if (c == IMG_CLASSES || req->size > IMG_SLOT_MAX || req->alignment > 4096) {   // memory of its own
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req->size, type}; VkDeviceMemory m;
        return vkAllocateMemory(V.dev, &ai, NULL, &m) ? VK_NULL_HANDLE : m;
    }
    pthread_mutex_lock(&g_imgblk_mu);
    imgblk *b = g_imgblk[c]; while (b && (b->type != type || !b->nfree)) b = b->next;
    if (!b) {
        b = calloc(1, sizeof *b); b->type = type; b->cls = (uint32_t)c; VkDeviceSize bs = img_class_size(c) * 64; if (bs < (2 << 20)) bs = 2 << 20; if (bs > IMG_BLOCK) bs = IMG_BLOCK; b->nslot = (uint32_t)(bs / img_class_size(c));   // 64 slots, 2 MB to 32 MB
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, (VkDeviceSize)b->nslot * img_class_size(c), type};
        if (vkAllocateMemory(V.dev, &ai, NULL, &b->m)) { pthread_mutex_unlock(&g_imgblk_mu); free(b); return VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < b->nslot; i++) b->bits[i >> 6] |= 1ull << (i & 63);
        b->nfree = b->nslot; b->next = g_imgblk[c]; g_imgblk[c] = b;
    }
    uint32_t i = 0; while (!b->bits[i >> 6]) i += 64; i += (uint32_t)__builtin_ctzll(b->bits[i >> 6]);
    b->bits[i >> 6] &= ~(1ull << (i & 63)); b->nfree--;
    pthread_mutex_unlock(&g_imgblk_mu);
    imgsub *s = malloc(sizeof *s); s->b = b; s->slot = i; *sub = s; *off = (VkDeviceSize)i * img_class_size(c);
    return b->m;
}
static void img_sub_free(imgsub *s) {   // the slot is free again; blocks are kept (the next images of the class take them)
    pthread_mutex_lock(&g_imgblk_mu); s->b->bits[s->slot >> 6] |= 1ull << (s->slot & 63); s->b->nfree++; pthread_mutex_unlock(&g_imgblk_mu); free(s);
}
void vk_defer_free_buffer(VkBuffer b, VkDeviceMemory m) { df_add(0, (void *)b, (void *)m); }
void vk_defer_free_image_sub(VkImage i, void *sub) { df_add(4, (void *)i, sub); }
void img_sub_free_(void *s) { img_sub_free((imgsub *)s); }
void vk_defer_free_image(VkImage i, VkDeviceMemory m) { df_add(1, (void *)i, (void *)m); }
void vk_defer_free_view(VkImageView v) { df_add(2, (void *)v, NULL); }
void vk_defer_free_bview(VkBufferView v) { df_add(3, (void *)v, NULL); }
// Descriptor pools: the allocator counts what each set takes and moves to a new pool before any count runs out. Adreno's driver does not refuse an
// allocation past a pool's sizes: it succeeds and the submission then fails as a whole at vkEndCommandBuffer (VK_ERROR_OUT_OF_HOST_MEMORY; NEO's
// handheld frames with 32-slot texture arrays crossed the old 8192 combined-sampler limit at about the 712th draw)
enum { DP_SETS = 4096 };
static const uint32_t DP_CAP[9] = {[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC] = 16384, [VK_DESCRIPTOR_TYPE_SAMPLER] = 1024, [VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] = 65536, [VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE] = 1024,
    [VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] = 2048, [VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER] = 4096, [VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER] = 2048,
    [VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER] = 32768, [VK_DESCRIPTOR_TYPE_STORAGE_BUFFER] = 4096};
static nvn_hmap g_dsl_need = {.klen = sizeof(VkDescriptorSetLayout)}; static pthread_mutex_t g_dsl_mu = PTHREAD_MUTEX_INITIALIZER;
void vk_dsl_register(VkDescriptorSetLayout l, const VkDescriptorSetLayoutBinding *b, uint32_t n) {   // what one set of this layout takes, by type
    uint32_t *need = calloc(9, sizeof *need);
    for (uint32_t i = 0; i < n; i++) if ((uint32_t)b[i].descriptorType < 9) need[b[i].descriptorType] += b[i].descriptorCount;
    pthread_mutex_lock(&g_dsl_mu); uint32_t *old = nvn_hm_put(&g_dsl_need, &l, need); pthread_mutex_unlock(&g_dsl_mu);
    free(old);   // (a non-dispatchable handle may be the same value for two identical layouts: the counts are the same, the older array goes)
}
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout l) {
    vksub_t *s = cur();
    pthread_mutex_lock(&g_dsl_mu); const uint32_t *need = nvn_hm_get(&g_dsl_need, &l); pthread_mutex_unlock(&g_dsl_mu);
    for (;;) {
        if (s->dp_cur == s->ndp) {
            VkDescriptorPoolSize sz[9]; uint32_t nz = 0;
            for (int t = 0; t < 9; t++) if (DP_CAP[t]) sz[nz++] = (VkDescriptorPoolSize){(VkDescriptorType)t, DP_CAP[t]};
            VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, DP_SETS, nz, sz};
            if (s->ndp == s->capdp) { s->capdp = s->capdp ? 2 * s->capdp : 4; s->dp = realloc(s->dp, s->capdp * sizeof *s->dp); }
            VKCHK(vkCreateDescriptorPool(V.dev, &pi, NULL, &s->dp[s->ndp++]));
        }
        if (s->dp_cur != s->dp_counted) { memset(s->dp_used, 0, sizeof s->dp_used); s->dp_sets = 0; s->dp_counted = s->dp_cur; }
        int fits = s->dp_sets < DP_SETS;
        if (need) for (int t = 0; t < 9 && fits; t++) if (need[t] && s->dp_used[t] + need[t] > DP_CAP[t]) fits = 0;
        if (!fits) { s->dp_cur++; continue; }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, s->dp[s->dp_cur], 1, &l};
        VkDescriptorSet set; VkResult r = vkAllocateDescriptorSets(V.dev, &ai, &set);
        if (r == VK_SUCCESS) { s->dp_sets++; if (need) for (int t = 0; t < 9; t++) s->dp_used[t] += need[t]; return set; }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) die("[vk] vkAllocateDescriptorSets: %d", r);
        s->dp_cur++;
    }
}

// ---------------- submissions: begin, CPU-visible effects, commit ----------------
static void post(post_op o) { vksub_t *s = cur(); if (s->npost == s->cappost) { s->cappost = s->cappost ? 2 * s->cappost : 64; s->post = realloc(s->post, s->cappost * sizeof *s->post); } s->post[s->npost++] = o; }
void mtl_init(void) { vk_init(); }
static int g_shadow_pools;
static void pools_readback_barrier(VkCommandBuffer cb) {
    if (!g_shadow_pools) return;
    // The last part can be dropped after earlier ones were sent; each part must publish its writes for the CPU merge.
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}
void mtl_sub_begin(void) {
    vk_init();
    pthread_mutex_lock(&g_sub_lock);
    // the next slot of the ring (g_cur is -1 between submissions, so it cannot say where the ring is: every submission took slot 0 and waited in slot_recycle
    // for the one before to finish on the GPU, which kept the replay of a frame from overlapping the GPU work of the last)
    static int next; g_cur = next; next = (next + 1) % NSLOT; vksub_t *s = &g_slot[g_cur];
    slot_recycle(s);
    s->npost = 0; s->up_used = 0; s->ngw = 0;
    { const char *e = getenv("DM_VK_POOL_READBACK_RANGES"); s->readback_ranges = e && atoi(e); }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    s->main = s->mains[0]; s->nmain_used = 1; s->cb_err = 0; s->up = s->ups[0]; s->nup_used = 1; s->nmain_sent = 0;
    VKCHK(vkBeginCommandBuffer(s->main, &bi)); g_cmd_gen++;
    // copied pools: what the game wrote before submitting goes into the GPU copy before any part of the submission can reach the GPU (DM_VK_CB_DRAWS sends
    // parts early; synced at the commit, those parts read the copy as the last submission left it), and only once no earlier submission still reads it
    pthread_rwlock_rdlock(&g_pool_mu); int shadow = 0; for (int i = 0; i < g_npool; i++) shadow |= g_pool[i].shadow != NULL; pthread_rwlock_unlock(&g_pool_mu);
    g_shadow_pools = shadow;
    if (shadow) {
        int diag = mem_diag_on(); uint64_t t = diag ? mem_diag_ns() : 0;
        mtl_wait(g_fence, ~0ull); if (diag) mem_diag_note(0, t, 0);
        vk_pools_sync(NULL);
    }   // (after the completion thread's copy-back of the last submission, not only its GPU work)
    if (g_tsq) { vkCmdResetQueryPool(s->main, g_tsq, 2 * (uint32_t)g_cur, 2); vkCmdWriteTimestamp(s->main, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_tsq, 2 * (uint32_t)g_cur); }
    if (g_cq) vkCmdResetQueryPool(s->main, g_cq, (uint32_t)g_cur * CQ, CQ);
    s->ncq = 0;
    g_npass[g_cur] = 0; if (pass_times_on()) vkCmdResetQueryPool(s->main, g_pq, (uint32_t)(g_cur * PASSQ * 2), PASSQ * 2);
    vk_exec_reset();
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0, -1}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    vksub_t *s = cur(); int q = -1;
    if (g_cq && (type == 0 || type == 0x10) && s->ncq < CQ) { q = g_cur * CQ + s->ncq++; vkCmdWriteTimestamp(vk_cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_cq, (uint32_t)q); }
    post((post_op){2, NULL, 0, 0, addr, type, q});
}
static VkMemoryBarrier2 full_barrier(void) {
    return (VkMemoryBarrier2){VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
}
void vk_img_barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}
void vk_gpu_wrote(uint64_t a, uint64_t n);
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    VkBuffer sb, db; VkDeviceSize so, dof;
    if (!n) return;
    if (!vk_buf_at(src, n, &sb, &so) || !vk_buf_at(dst, n, &db, &dof)) { VKRED_ONCE(1, "buffer copy outside the memory pools (%#llx → %#llx, %llu bytes)", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n); return; }
    vk_end_pass_land();
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    VkBufferCopy r = {so, dof, n}; vkCmdCopyBuffer(cb, sb, db, 1, &r);
    vk_img_barrier_all(cb); vk_gpu_wrote(dst, n);
}
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    VkBuffer db; VkDeviceSize dof;
    if (!n) return;
    if (!vk_buf_at(dst, n, &db, &dof)) { VKRED_ONCE(2, "buffer fill outside the memory pools"); return; }
    vk_end_pass_land();
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    vkCmdFillBuffer(cb, db, dof, n & ~3ull, value);
    vk_img_barrier_all(cb); vk_gpu_wrote(dst, n);
}
// an inline uniform update (UpdateUniformBuffer): the bytes are taken at record time, copied in GPU order from scratch memory
// Inline uniform updates (UpdateUniformBuffer) are deferred instead of ending the render pass (as nvn_mtl.m's ubd_*): a copy cannot run inside a pass, and
// ending it stores every attachment and loads it again in the next pass, which on a tiler is a round trip of the whole tile memory per update (Unity updates
// before nearly every draw: NEO had about 145 passes for 148 draws a frame). NVN applies the update in command order, so a draw after it whose uniform
// binding overlaps it reads a shadow: memory with the deferred updates applied in order (vk_ubd_shadow). The real copies are made in order when a pass
// ends anyway, and at the end of the submission (vk_ubd_land), so later passes, copies, dispatches and the CPU see them as before.
typedef struct { uint64_t lo, hi; VkBuffer sb; VkDeviceSize so; const uint8_t *p; } ubd_t;
enum { UBD_MAX = 512 };
static ubd_t g_ubd[UBD_MAX]; static int g_nubd; uint64_t g_ubd_gen = 1, g_ubd_landed, g_ubd_shadows, g_ubd_bytes, g_ns_submit, g_ubd_superseded;
void vk_end_pass_land(void);
void vk_sub_update(uint64_t dst, const void *data, uint64_t n) {
    VkBuffer db, sb; VkDeviceSize dof, so;
    if (!n) return;
    if (!vk_buf_at(dst, n, &db, &dof)) { VKRED_ONCE(3, "uniform update outside the memory pools"); return; }
    // an update of exactly the range of a deferred one supersedes it: the older one goes (Unity rewrites the same few constant buffers before every draw,
    // so the list keeps about one entry per buffer region, which keeps the landing to a few copies and the shadows cheap to build)
    for (int i = g_nubd - 1; i >= 0; i--) if (g_ubd[i].lo == dst && g_ubd[i].hi == dst + n) { memmove(&g_ubd[i], &g_ubd[i + 1], sizeof *g_ubd * (size_t)(g_nubd - i - 1)); g_nubd--; g_ubd_superseded++; break; }
    if (g_nubd == UBD_MAX) vk_end_pass_land();   // too many distinct ranges: they land now
    uint8_t *p = vk_scratch(n, 4, &sb, &so); memcpy(p, data, n);
    g_ubd[g_nubd++] = (ubd_t){dst, dst + n, sb, so, p}; g_ubd_gen++;
}
// the draw bisection (gfx_vk_exec.c draw_allowed): the outcome of this submission
extern int g_bis_on, g_bis_lo, g_bis_hi, g_bis_trace, g_bis_var; void vk_bisect_reset_count(void);
static void vk_bisect_result(int ok) {
    static int done, draws_seen; extern uint32_t g_sub_ops[64]; int nd = (int)(g_sub_ops[25] + g_sub_ops[26]);
    if (done) {   // the found draw: one change per submission, then the draw is no longer traced
        if (g_bis_trace >= 0) {
            static const char *what[] = {"as recorded", "depth bias values 0", "no depth bias", "dummy textures", "vertex offset 0", "zero vertex buffer", "in a new pass", "skipped alone (the rest drawn)", "without their fragment shader", "with zero uniform buffers", "with their quad draws skipped", "with all but their quad draws skipped"};
            VKLOG("bisection: draws from %d on %s: %s", g_bis_trace, what[g_bis_var], ok ? "ACCEPTED" : "refused");
            if (++g_bis_var > 11) { g_bis_trace = -1; g_bis_var = 0; }
        }
        vk_bisect_reset_count(); return; }
    if (!g_bis_on) { if (!ok) { g_bis_on = 1; g_bis_lo = 0; g_bis_hi = nd + 1; draws_seen = nd; VKLOG("bisecting the refused submission's %d draws", nd); } vk_bisect_reset_count(); return; }
    int mid = (g_bis_lo + g_bis_hi) / 2;
    if (ok) g_bis_lo = mid; else g_bis_hi = mid;
    if (g_bis_hi - g_bis_lo <= 1) { done = 1; g_bis_on = 0; g_bis_trace = g_bis_lo; VKLOG("bisection: draw %d of %d is the first the driver refuses (traced in the next submission)", g_bis_lo, draws_seen); }
    vk_bisect_reset_count();
}
void vk_ubd_land_drop(void) { g_nubd = 0; g_ubd_gen++; }
// Landed updates still in flight: their copies are queued but the GPU may not have run them, so memory may not hold them yet; a shadow built on the
// CPU (vk_ubd_read) applies them over memory until their submission completes. Without this a shadow mixed the newest update with the frame before's
// memory (shadows detached from moving characters, far pedestrians flickering).
typedef struct { uint64_t lo, hi, fence; const uint8_t *data; } ubd_infl_t;   // data: the update's bytes in its submission's scratch memory, which is not reused before that submission completes
static ubd_infl_t *g_infl; static int g_ninfl, g_capinfl;
static void infl_prune(void) {
    uint64_t done = vk_fence_done(); int o = 0;
    for (int i = 0; i < g_ninfl; i++) if (g_infl[i].fence > done) g_infl[o++] = g_infl[i];
    g_ninfl = o;
}
static void sub_gpu_wrote(uint64_t a, uint64_t n);
void vk_ubd_land(VkCommandBuffer cb) {   // outside a render pass
    if (!g_nubd) return;
    infl_prune();
    for (int i = 0; i < g_nubd; i++) {
        if (g_ninfl == g_capinfl) { g_capinfl = g_capinfl ? 2 * g_capinfl : 256; g_infl = realloc(g_infl, sizeof *g_infl * (size_t)g_capinfl); }
        // (the bytes used to be copied into the heap here: a malloc, memcpy and free of up to 64 KB per update, for bytes the scratch memory keeps anyway)
        g_infl[g_ninfl++] = (ubd_infl_t){g_ubd[i].lo, g_ubd[i].hi, g_fence + 1, g_ubd[i].p};   // the fence of the submission being recorded
    }
    vk_img_barrier_all(cb);
    for (int i = 0; i < g_nubd; i++) { VkBuffer db; VkDeviceSize dof; if (vk_buf_at(g_ubd[i].lo, g_ubd[i].hi - g_ubd[i].lo, &db, &dof)) {
        VkBufferCopy r = {g_ubd[i].so, dof, g_ubd[i].hi - g_ubd[i].lo}; vkCmdCopyBuffer(cb, g_ubd[i].sb, db, 1, &r); sub_gpu_wrote(g_ubd[i].lo, g_ubd[i].hi - g_ubd[i].lo); } }
    vk_img_barrier_all(cb);
    g_ubd_landed += (uint64_t)g_nubd; g_nubd = 0; g_ubd_gen++;
}
int vk_ubd_overlaps(uint64_t a, uint64_t n) { for (int i = 0; i < g_nubd; i++) if (g_ubd[i].lo < a + n && a < g_ubd[i].hi) return 1; return 0; }
// GPU writes to pool memory (buffer copies and fills, texture → buffer copies, storage buffers of dispatches) whose submission may not have completed:
// until it has, a CPU read of that memory (vk_ubd_read: a shadow, a handle in a uniform buffer) may see the bytes from before them or after them
typedef struct { uint64_t lo, hi, fence; } gpuw_t; enum { GW_MAX = 1024 };
static gpuw_t g_gw[GW_MAX]; static int g_ngw; uint64_t g_ubd_racy;
static void sub_gpu_wrote(uint64_t a, uint64_t n) {   // into the open submission's list (pools_copy_back)
    if (g_cur < 0 || !n) return;
    vksub_t *s = &g_slot[g_cur]; uint64_t lo = a, hi = a + n, bd = ~0ull; int best = -1;
    for (int k = 0; k < s->ngw; k++) { uint64_t d = lo > s->gw[k].hi ? lo - s->gw[k].hi : s->gw[k].lo > hi ? s->gw[k].lo - hi : 0; if (d < bd) { bd = d; best = k; } }
    if (best >= 0 && (bd <= 65536 || s->ngw == GW_SUB)) { if (lo < s->gw[best].lo) s->gw[best].lo = lo; if (hi > s->gw[best].hi) s->gw[best].hi = hi; }
    else { s->gw[s->ngw].lo = lo; s->gw[s->ngw].hi = hi; s->ngw++; }
}
void vk_gpu_wrote(uint64_t a, uint64_t n) {
    uint64_t f = g_fence + 1;
    for (int i = g_ngw - 1; i >= 0 && g_gw[i].fence == f; i--) if (g_gw[i].lo <= a && a + n <= g_gw[i].hi) return;
    if (g_ngw == GW_MAX) { uint64_t done = vk_fence_done(); int o = 0; for (int i = 0; i < g_ngw; i++) if (g_gw[i].fence > done) g_gw[o++] = g_gw[i]; g_ngw = o; }
    if (g_ngw == GW_MAX) { memmove(g_gw, g_gw + 1, sizeof *g_gw * (GW_MAX - 1)); g_ngw--; }
    g_gw[g_ngw++] = (gpuw_t){a, a + n, f};
    sub_gpu_wrote(a, n);
}
int vk_gpu_pending(uint64_t a, uint64_t n) {
    if (!g_ngw) return 0;
    uint64_t done = vk_fence_done();
    for (int i = g_ngw - 1; i >= 0; i--) if (g_gw[i].fence > done && g_gw[i].lo < a + n && a < g_gw[i].hi) return 1;
    return 0;
}
// what a GPU read of [a, a + n) sees at this point of the command stream, into dst (memory, then the deferred updates in order)
void vk_ubd_read(uint64_t a, uint64_t n, uint8_t *dst) {
    if (vk_gpu_pending(a, n)) { static int k; g_ubd_racy++; if (k < 20) { k++; int cov = 0; for (int i = 0; i < g_nubd; i++) if (g_ubd[i].lo <= a && a + n <= g_ubd[i].hi) cov = 1;
        VKLOG("[RED] a CPU read of %#llx +%llu (%s) overlaps a GPU write not known to be complete", (unsigned long long)a, (unsigned long long)n, cov ? "covered by a deferred update" : "partly from memory"); } }
    memcpy(dst, (const void *)(uintptr_t)a, n);
    if (g_ninfl) { uint64_t done = vk_fence_done();
        for (int i = 0; i < g_ninfl; i++) { if (g_infl[i].fence <= done) continue; uint64_t lo = g_infl[i].lo > a ? g_infl[i].lo : a, hi = g_infl[i].hi < a + n ? g_infl[i].hi : a + n;
            if (lo < hi) memcpy(dst + (lo - a), g_infl[i].data + (lo - g_infl[i].lo), hi - lo); } }
    for (int i = 0; i < g_nubd; i++) { uint64_t lo = g_ubd[i].lo > a ? g_ubd[i].lo : a, hi = g_ubd[i].hi < a + n ? g_ubd[i].hi : a + n;
        if (lo < hi) memcpy(dst + (lo - a), g_ubd[i].p + (lo - g_ubd[i].lo), hi - lo); }
}
// a uniform binding [a, a + n) that deferred updates overlap: its shadow (0 when none overlaps). Shadows are kept per (address, size) until the next update.
int vk_ubd_shadow(uint64_t a, uint64_t n, VkBuffer *buf, VkDeviceSize *off) {
    if (!g_nubd || !vk_ubd_overlaps(a, n)) return 0;   // (a binding no deferred update touches reads memory on the GPU, after the in-flight copies: no shadow needed)
    enum { NC = 64 }; static struct { uint64_t a, n, gen; VkBuffer b; VkDeviceSize o; } c[NC];
    unsigned k = (unsigned)((a ^ n * 0x9E3779B97F4A7C15ull) >> 6) % NC;
    extern int g_x_noshc; if (c[k].a == a && c[k].n == n && c[k].gen == g_ubd_gen && !g_x_noshc) { *buf = c[k].b; *off = c[k].o; return 1; }
    uint8_t *d = vk_scratch(n, V.props.limits.minUniformBufferOffsetAlignment, buf, off); vk_ubd_read(a, n, d); g_ubd_bytes += n;
    c[k].a = a; c[k].n = n; c[k].gen = g_ubd_gen; c[k].b = *buf; c[k].o = *off; g_ubd_shadows++;
    return 1;
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }

// completion: a queue of committed submissions, each with its CPU-visible effects
typedef struct cpend { uint64_t fence, frame; post_op *ops; int n; int slot, rslot; int npass; pass_meta pmeta[PASSQ]; struct cpend *next; } cpend;
static cpend *g_ch, *g_ct; static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_ccv = PTHREAD_COND_INITIALIZER;
uint64_t guest_ns(void);
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); g = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; } return g * 12 / 625; }
// GPU time per NVN frame (the sum of its submissions' timestamp spans), for the timing line's GPU column: the frame a submission belongs to is the one being replayed
static uint64_t g_cur_frame, g_frame_gpu[64], g_frame_gpu_of[64];
void mtl_set_frame(uint64_t f) { __atomic_store_n(&g_cur_frame, f, __ATOMIC_RELAXED); }
double mtl_gpu_ms(uint64_t f) { return g_frame_gpu_of[f % 64] == f + 1 ? g_frame_gpu[f % 64] / 1e6 : -1; }
double vk_gpu_ms_recent(void) {   // the latest finished frame's GPU time among the last 8 (-1: none known)
    uint64_t f = __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED);
    for (uint64_t k = 2; k < 8 && k <= f; k++) { double g = mtl_gpu_ms(f - k); if (g >= 0) return g; }
    return -1;
}
// DM_VK_PASS_SPIKE=<ms> (read at start-up, with DM_VK_PASS_TIMES; default 25): a frame whose timed passes and dispatches, or whose submissions, take more than that lists its costliest
// ones, to find what a one-frame GPU spike drew (Tears of the Kingdom: 25-180 ms frames among 10-13 ms ones while fighting). Completion thread only
static uint64_t g_sf_frame = ~0ull; static struct { double ms; pass_meta m; } g_sf[PASSQ * 4]; static int g_nsf; static double g_sf_tot;
static void pass_spike_add(double ms, const pass_meta *m) { g_sf_tot += ms; if (g_nsf < PASSQ * 4) { g_sf[g_nsf].ms = ms; g_sf[g_nsf].m = *m; g_nsf++; } }
static void pass_spike_flush(void) {
    static double lim = -1; if (lim < 0) { const char *e = getenv("DM_VK_PASS_SPIKE"); lim = e && atof(e) > 0 ? atof(e) : 25; }
    double fg = g_sf_frame != ~0ull ? mtl_gpu_ms(g_sf_frame) : -1;   // the frame's submissions end to end: a spike outside the timed passes shows as the difference
    if (g_sf_frame != ~0ull && (g_sf_tot > lim || fg > lim)) {
        for (int i = 0; i < g_nsf && i < 6; i++) for (int j = i + 1; j < g_nsf; j++) if (g_sf[j].ms > g_sf[i].ms) { __typeof__(g_sf[0]) x = g_sf[i]; g_sf[i] = g_sf[j]; g_sf[j] = x; }
        char b[1200]; int o = snprintf(b, sizeof b, "GPU spike: frame %llu, %.2f ms in %d passes and dispatches (frame GPU %.2f ms, %.2f outside them):", (unsigned long long)g_sf_frame, g_sf_tot, g_nsf, fg, fg - g_sf_tot);
        for (int i = 0; i < g_nsf && i < 6 && o < (int)sizeof b - 100; i++) { const pass_meta *m = &g_sf[i].m;
            if (m->fmt == PT_DISPATCH) o += snprintf(b + o, sizeof b - (size_t)o, " #%u %.2f ms (dispatch %ux%ux%u);", m->fidx, g_sf[i].ms, m->w, m->h, m->draws);
            else o += snprintf(b + o, sizeof b - (size_t)o, " #%u %.2f ms (%ux%u fmt %u, %u draws);", m->fidx, g_sf[i].ms, m->w, m->h, m->fmt, m->draws & 0xffff); }
        VKLOG("%s", b);
    }
    g_nsf = 0; g_sf_tot = 0;
}
static void *completion_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "dm-vk-complete");
    for (;;) {
        pthread_mutex_lock(&g_cmu);
        while (!g_ch) pthread_cond_wait(&g_ccv, &g_cmu);
        cpend *c = g_ch; pthread_mutex_unlock(&g_cmu);
        if (V.has_timeline) wait_fence(c->fence); else { VkResult r = vkWaitForFences(V.dev, 1, &g_fring[c->fence % NFRING], VK_TRUE, UINT64_MAX); if (r != VK_SUCCESS) die("[vk] GPU fence wait did not complete: %d", r); }
        if (g_shadow_pools && c->rslot >= 0) pools_copy_back(&g_slot[c->rslot]);
        if (g_tsq && c->slot >= 0) { uint64_t t[2]; if (vkGetQueryPoolResults(V.dev, g_tsq, 2 * (uint32_t)c->slot, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
            { uint64_t ns = (uint64_t)((double)(t[1] - t[0]) * V.props.limits.timestampPeriod); g_gpu_ns += ns; int k = (int)(c->frame % 64);
              if (g_frame_gpu_of[k] != c->frame + 1) { g_frame_gpu[k] = 0; g_frame_gpu_of[k] = c->frame + 1; } g_frame_gpu[k] += ns; } }
        if (g_pq && pass_times_on() && c->slot >= 0) {   // this submission's passes
            int np = c->npass; uint64_t t[PASSQ * 2];
            if (c->frame != g_sf_frame) { pass_spike_flush(); g_sf_frame = c->frame; }
            if (np && vkGetQueryPoolResults(V.dev, g_pq, (uint32_t)(c->slot * PASSQ * 2), (uint32_t)np * 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                for (int i = 0; i < np; i++) if (t[2 * i + 1] > t[2 * i]) { pass_spike_add((double)(t[2 * i + 1] - t[2 * i]) * V.props.limits.timestampPeriod / 1e6, &c->pmeta[i]); int f = (int)c->pmeta[i].fidx; g_pacc[f] += (double)(t[2 * i + 1] - t[2 * i]) * V.props.limits.timestampPeriod / 1e6; g_pcnt[f]++; g_plast[f] = c->pmeta[i]; if (f + 1 > g_pmax) g_pmax = f + 1; }
        }
        uint64_t now = gpu_ticks(), tend = 0;
        if (g_tsq && c->slot >= 0) { uint64_t t[2]; if (vkGetQueryPoolResults(V.dev, g_tsq, 2 * (uint32_t)c->slot, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) tend = t[1]; }
        for (int i = 0; i < c->n; i++) {
            post_op *o = &c->ops[i];
            if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; continue; }
            uint64_t ts = now, tq;
            if (o->q >= 0 && tend && vkGetQueryPoolResults(V.dev, g_cq, (uint32_t)o->q, 1, 8, &tq, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && tq <= tend) {
                uint64_t back = (uint64_t)((double)(tend - tq) * V.props.limits.timestampPeriod * 12 / 625); if (back < now) ts = now - back; }
            // render_cond: a timestamp report's counter field is 0, as on the GPU (gfx_vk_exec.c render_cond evaluates conditions on it)
            uint64_t rec[2] = {o->type == 0 || o->type == 0x10 ? (DM_GAME_RENDER_COND ? 0 : ts) : o->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? (1u << 20) : 0, ts}; memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
        }
        pthread_mutex_lock(&g_cmu); g_ch = c->next; if (!g_ch) g_ct = NULL; pthread_mutex_unlock(&g_cmu);
        if (g_prereset > 0 && c->rslot >= 0) { vksub_t *s = &g_slot[c->rslot];   // its command buffers reset here (slot_recycle); the slot is not taken before g_done passes this fence
            if (s->fence == c->fence) { for (int i = 0; i < s->nmain_alloc; i++) vkResetCommandBuffer(s->mains[i], 0); for (int i = 0; i < s->nup_alloc; i++) vkResetCommandBuffer(s->ups[i], 0); } }
        pthread_mutex_lock(&g_done_mu); if (c->fence > g_done) __atomic_store_n(&g_done, c->fence, __ATOMIC_RELEASE); pthread_cond_broadcast(&g_done_cv); pthread_mutex_unlock(&g_done_mu);
        free(c->ops); free(c);
    }
    return NULL;
}
static void completion_start(void) { pthread_t t; if (pthread_create(&t, NULL, completion_main, NULL)) die("[vk] completion thread"); pthread_detach(t); }
uint64_t mtl_sub_commit(void) {
    { void vk_prof_end(void); vk_prof_end(); }
    vksub_t *s = cur();
    vk_end_pass_land();
    pools_readback_barrier(s->main);
    if (g_tsq) vkCmdWriteTimestamp(s->main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_tsq, 2 * (uint32_t)g_cur + 1);
    VkResult er = vkEndCommandBuffer(s->main); if (s->cb_err && er >= 0) er = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (er < 0) {   // the driver refused something recorded in it: logged with what the submission held, then it is dropped and the game goes on
        extern uint32_t g_sub_ops[64]; char ops[600]; int o = 0;
        for (int i = 0; i < 64 && o < (int)sizeof ops - 16; i++) if (g_sub_ops[i]) o += snprintf(ops + o, sizeof ops - (size_t)o, " %d:%u", i, g_sub_ops[i]);
        static int n; if (n++ < 3) VKLOG("[RED] vkEndCommandBuffer failed (%d): submission dropped; record ops (op:count)%s", er, ops);
        vk_bisect_result(0);
        if (s->nmain_sent) {   // parts already sent may still run: nothing more is submitted (the submission below only signals its fence)
            s->nmain_sent = s->nmain_used; s->up_used = 0; s->cb_err = 0; s->no_ts = 1; vk_ubd_land_drop();
        } else {
            vkResetCommandPool(V.dev, s->pool, 0); VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            s->main = s->mains[0]; s->nmain_used = 1; s->cb_err = 0; s->no_ts = 1;   // no timestamps: the slot's queries still hold an older submission's
            if (s->nup_used > 1) { s->up = s->ups[0]; s->nup_used = 1; }
            VKCHK(vkBeginCommandBuffer(s->main, &bi)); g_cmd_gen++; s->up_used = 0; vk_ubd_land_drop();
            pools_readback_barrier(s->main);
            VKCHK(vkEndCommandBuffer(s->main));
        }
    }
    else vk_bisect_result(1);
    { extern uint32_t g_sub_ops[64]; memset(g_sub_ops, 0, sizeof g_sub_ops); }
    if (s->up_used) { vk_img_barrier_all(s->up); VKCHK(vkEndCommandBuffer(s->up)); }
    uint64_t f = ++g_fence;
    VkCommandBuffer cbs[1 + MAIN_CBS]; uint32_t ncb = 0; if (s->up_used) cbs[ncb++] = s->up; for (int i = s->nmain_sent; i < s->nmain_used; i++) cbs[ncb++] = s->mains[i];
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &f};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 0, NULL, NULL, ncb, cbs, 1, &g_timeline};
    VkFence fe = VK_NULL_HANDLE;
    if (!V.has_timeline) {   // the ring's fence for f: free once the completion thread has passed f - NFRING (already so: slot_recycle waited for f - NSLOT)
        if (f > NFRING) mtl_wait(f - NFRING, ~0ull);
        fe = g_fring[f % NFRING]; VKCHK(vkResetFences(V.dev, 1, &fe)); si.pNext = NULL; si.signalSemaphoreCount = 0;
    }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); pthread_mutex_lock(&g_vk_queue_mu); VkResult r = vkQueueSubmit(V.q, 1, &si, fe); pthread_mutex_unlock(&g_vk_queue_mu); clock_gettime(CLOCK_MONOTONIC, &t1); g_ns_submit += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
    if (r) { void vk_crumbs_report(void); vk_crumbs_report(); die("[vk] vkQueueSubmit: %d (device lost?)", r); }
    s->fence = f;
    cpend *c = calloc(1, sizeof *c); c->fence = f; c->frame = __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED); c->n = s->npost; c->rslot = g_cur; c->slot = s->no_ts ? -1 : g_cur; s->no_ts = 0;
    c->npass = g_npass[g_cur]; memcpy(c->pmeta, g_pmeta[g_cur], sizeof(pass_meta) * (size_t)c->npass);
    if (s->npost) { c->ops = malloc(s->npost * sizeof *c->ops); memcpy(c->ops, s->post, s->npost * sizeof *c->ops); }
    pthread_mutex_lock(&g_cmu); if (g_ct) g_ct->next = c; else g_ch = c; g_ct = c; pthread_cond_signal(&g_ccv); pthread_mutex_unlock(&g_cmu);
    s->npost = 0; g_cur = -1;
    pthread_mutex_unlock(&g_sub_lock);
    return f;
}
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600ull * 1000000000ull ? 3600ull * 1000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000ull); dl.tv_nsec = (long)(ns % 1000000000ull);
    pthread_mutex_lock(&g_done_mu);
    while (g_done < fence) if (pthread_cond_timedwait(&g_done_cv, &g_done_mu, &dl)) break;
    int ok = g_done >= fence; pthread_mutex_unlock(&g_done_mu);
    return ok;
}
void mtl_finish(void) { mtl_wait(g_fence, ~0ull); }
void mtl_drain(void) { mtl_finish(); vk_pipeline_cache_save(); }
