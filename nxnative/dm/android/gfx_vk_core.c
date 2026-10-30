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
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define VK_DEF(f) PFN_##f f;
VK_FNS(VK_DEF) VK_DEV_FNS(VK_DEF)
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
vkdev_t V;
pthread_mutex_t g_vk_queue_mu = PTHREAD_MUTEX_INITIALIZER;   // VkQueue access is externally synchronized: submissions and presents

uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < V.mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (V.mem.memoryTypes[i].propertyFlags & want) == want && !(V.mem.memoryTypes[i].propertyFlags & avoid)) return i;
    for (uint32_t i = 0; i < V.mem.memoryTypeCount; i++) if ((bits >> i & 1) && (V.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
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

// Whether the device reads ASTC back as it was given: a solid red block (void extent) copied into a 4x4 ASTC image, blitted to RGBA8 and read back. The Android
// emulator on a Mac (37.2.12; gfxstream's "ASTC LDR emulation mode: Gpu" over MoltenVK) reports ASTC and read (0, 0, 0, 0) from it, in a blit and in a compute
// shader's texelFetch alike, while BC1 read red: Link's Awakening's ASTC hearts drew nothing. 1 also when the format cannot be blitted (nothing to look at)
static int astc_reads_back(void) {
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) return 1;
    int ok = 1; VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory bm = VK_NULL_HANDLE, im_m[2] = {0}; VkImage im[2] = {0}; VkCommandPool cp = VK_NULL_HANDLE; uint8_t *p = NULL;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkMemoryRequirements rq; VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (vkCreateBuffer(V.dev, &bci, NULL, &b)) goto done;
    vkGetBufferMemoryRequirements(V.dev, b, &rq); ai.allocationSize = rq.size; ai.memoryTypeIndex = vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(V.dev, &ai, NULL, &bm) || vkBindBufferMemory(V.dev, b, bm, 0) || vkMapMemory(V.dev, bm, 0, 256, 0, (void **)&p)) goto done;
    static const uint8_t red[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF};   // void extent: RGBA16 (1, 0, 0, 1)
    memset(p, 0, 256); memcpy(p, red, 16);
    VkFormat fm[2] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM};
    for (int k = 0; k < 2; k++) {
        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fm[k], {4, 4, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        if (vkCreateImage(V.dev, &ic, NULL, &im[k])) goto done;
        vkGetImageMemoryRequirements(V.dev, im[k], &rq); ai.allocationSize = rq.size; ai.memoryTypeIndex = vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(V.dev, &ai, NULL, &im_m[k]) || vkBindImageMemory(V.dev, im[k], im_m[k], 0)) goto done;
    }
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, V.qf}; VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vkCreateCommandPool(V.dev, &pci, NULL, &cp)) goto done;
    cai.commandPool = cp; VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (vkAllocateCommandBuffers(V.dev, &cai, &cb) || vkBeginCommandBuffer(cb, &cbi)) goto done;
    VkImageMemoryBarrier ib[2]; VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    for (int k = 0; k < 2; k++) ib[k] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im[k], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, ib);
    VkBufferImageCopy up = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}}, down = {64, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
    vkCmdCopyBufferToImage(cb, b, im[0], VK_IMAGE_LAYOUT_GENERAL, 1, &up);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {4, 4, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {4, 4, 1}}};
    vkCmdBlitImage(cb, im[0], VK_IMAGE_LAYOUT_GENERAL, im[1], VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdCopyImageToBuffer(cb, im[1], VK_IMAGE_LAYOUT_GENERAL, b, 1, &down);
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT; vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
    if (vkEndCommandBuffer(cb) || vkQueueSubmit(V.q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(V.q)) goto done;
    for (int t = 0; t < 16; t++) if (p[64 + 4 * t] != 255 || p[65 + 4 * t] || p[66 + 4 * t] || p[67 + 4 * t] != 255) ok = 0;
    if (!ok) VKLOG("ASTC: a solid red block reads back as (%u, %u, %u, %u)", p[64], p[65], p[66], p[67]);
done:
    if (cp) vkDestroyCommandPool(V.dev, cp, NULL);
    for (int k = 0; k < 2; k++) { if (im[k]) vkDestroyImage(V.dev, im[k], NULL); if (im_m[k]) vkFreeMemory(V.dev, im_m[k], NULL); }
    if (b) vkDestroyBuffer(V.dev, b, NULL); if (bm) vkFreeMemory(V.dev, bm, NULL);
    return ok;
}

void vk_init(void) {
    static int done; if (done) return; done = 1;
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) die("[vk] libvulkan.so: %s", dlerror());
    p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
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
    if (!(v12 ? f12.timelineSemaphore : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)[0]))
        die("[vk] the device has no timeline semaphores (Vulkan 1.2 or VK_KHR_timeline_semaphore): every submission signals one");
    // dynamic rendering is 1.3 or its extension with the feature on; a loaded pointer does not tell, since the loader hands out a trampoline for core functions the driver lacks
    V.has_dynrender = v13 ? f13.dynamicRendering : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)[0];
    // imageless framebuffers need render pass 2 (gfx_vk_exec.c rp_get): core in 1.2, else its extension
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
    V.max_aniso = V.props.limits.maxSamplerAnisotropy;
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
    vkGetDeviceQueue(V.dev, V.qf, 0, &V.q);
    VkPipelineCacheCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    char path[1024]; const char *root = getenv("DM_ROOT"); void *data = NULL; size_t dn = 0;
    if (root && !getenv("DM_VK_NOPCACHE")) { snprintf(path, sizeof path, "%s/vk_pipeline_cache.bin", root); FILE *f = fopen(path, "rb");   // DM_VK_NOPCACHE=1: start empty
        if (f) { fseek(f, 0, SEEK_END); dn = (size_t)ftell(f); fseek(f, 0, SEEK_SET); data = malloc(dn); if (fread(data, 1, dn, f) != dn) dn = 0; fclose(f); } }
    pci.initialDataSize = dn; pci.pInitialData = data;
    if (vkCreatePipelineCache(V.dev, &pci, NULL, &V.pcache)) { pci.initialDataSize = 0; pci.pInitialData = NULL; VKCHK(vkCreatePipelineCache(V.dev, &pci, NULL, &V.pcache)); }
    free(data); free(ex);
    VKLOG("%s, Vulkan %u.%u, driver %#x; BC %s, ASTC %s, geometry shaders %s, nullDescriptor %s, custom border colours %s, depth clip control %s, transform feedback %s; pipeline cache %zu bytes",
          V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), V.props.driverVersion,
          V.has_bc ? "yes" : "no", V.has_astc ? "yes" : "no", V.has_gs ? "yes" : "no", V.has_robust2 ? "yes" : "no", V.has_custom_border ? "yes" : "no", V.has_depth_clip_ctl ? "yes" : "no", V.has_xfb ? "yes" : "no", dn);
    { const char *e = getenv("DM_VK_ASTC_CPU"); V.astc_cpu = e && *e ? atoi(e) != 0 : !V.has_astc || !astc_reads_back();   // DM_VK_ASTC_CPU=1 / =0: either way
      if (V.astc_cpu) VKLOG("ASTC textures are decoded on the CPU into RGBA8 (%s)", e && *e ? "DM_VK_ASTC_CPU" : !V.has_astc ? "the device has no ASTC" : "the device reads ASTC back wrong"); }
    slots_init();
    completion_start();
}
void vk_pipeline_cache_save(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static size_t last;
    const char *root = getenv("DM_ROOT"); if (!root || !V.pcache) return;
    pthread_mutex_lock(&mu);
    size_t n = 0; if (vkGetPipelineCacheData(V.dev, V.pcache, &n, NULL) || !n || n == last) { pthread_mutex_unlock(&mu); return; }   // unchanged since the last save
    last = n; pthread_mutex_unlock(&mu);
    void *d = malloc(n); if (vkGetPipelineCacheData(V.dev, V.pcache, &n, d)) { free(d); return; }
    char path[1024], tmp[1100]; snprintf(path, sizeof path, "%s/vk_pipeline_cache.bin", root); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (f) { fwrite(d, 1, n, f); fclose(f); rename(tmp, path); }
    free(d);
}

// ---------------- memory pools ----------------
#define MAXPOOL 4096
static vkpool_t g_pool[MAXPOOL]; static int g_npool;
static pthread_rwlock_t g_pool_mu = PTHREAD_RWLOCK_INITIALIZER;
static uint32_t g_pool_gen;   // bumped under the write lock when a pool is added or removed: vk_pool_of's per-thread cache holds while it is unchanged
// g_pool's indices by lo, remade under the write lock whenever a pool is added or removed: vk_pool_of searches it. A linear scan of g_pool behind the per-thread
// cache was 18% of the profiler's samples of Super Mario Bros. Wonder's replay in its first level (a draw looks up more pools than the cache holds); with the
// search its world map's replay took 4.2 ms a frame instead of 4.6-4.9
static uint16_t g_pool_ord[MAXPOOL];
static int pool_ord_cmp(const void *a, const void *b) { uint64_t x = g_pool[*(const uint16_t *)a].lo, y = g_pool[*(const uint16_t *)b].lo; return x < y ? -1 : x > y; }
static void pool_ord_make(void) { for (int i = 0; i < g_npool; i++) g_pool_ord[i] = (uint16_t)i; qsort(g_pool_ord, (size_t)g_npool, sizeof *g_pool_ord, pool_ord_cmp); }
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
// Zero copy: the pages of [mem, mem+size) become an AHardwareBuffer's, imported as a VkBuffer. 0 on success
static int pool_zero_copy(vkpool_t *p, uint8_t *mem, uint64_t size) {
    AHardwareBuffer_Desc d = {(uint32_t)size, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, 0, 0, 0};
    if (size > 0xffffffffull) return -1;
    AHardwareBuffer *ahb; if (AHardwareBuffer_allocate(&d, &ahb)) return -1;
    int sv[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { AHardwareBuffer_release(ahb); return -1; }
    int fds[16], nfd = -1;
    if (!AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0])) nfd = recv_fds(sv[1], fds, 16);
    close(sv[0]); close(sv[1]);
    if (nfd < 1) { AHardwareBuffer_release(ahb); return -1; }
    // copy the pool's current contents into the buffer through a temporary mapping, then put the buffer's pages at the pool's address
    uint8_t *tmp = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    int ok = tmp != MAP_FAILED;
    if (ok) { memcpy(tmp, mem, size); munmap(tmp, size); }
    ok = ok && mmap(mem, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fds[0], 0) == mem;
    for (int i = 0; i < nfd; i++) close(fds[i]);
    if (!ok) { AHardwareBuffer_release(ahb); return -1; }
    VkAndroidHardwareBufferPropertiesANDROID ap = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    if (vkGetAndroidHardwareBufferPropertiesANDROID(V.dev, ahb, &ap) || vkCreateBuffer(V.dev, &bi, NULL, &p->buf)) { AHardwareBuffer_release(ahb); return -2; }
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, p->buf};   // the driver requires a dedicated allocation for an imported AHardwareBuffer
    VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded, ahb};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, ap.allocationSize, vk_mem_type(ap.memoryTypeBits, 0, 0)};
    if (vkAllocateMemory(V.dev, &ai, NULL, &p->mem) || vkBindBufferMemory(V.dev, p->buf, p->mem, 0)) { vkDestroyBuffer(V.dev, p->buf, NULL); AHardwareBuffer_release(ahb); return -2; }
    p->ahb = ahb; p->zero_copy = 1;
    return 0;
}
// Zero copy through the driver's own memory, where the allocator makes no AHardwareBuffer for GPU data (the Android emulator: its ranchu gralloc refuses every BLOB
// with GPU usage, so every pool was copied in full around every submission): host-visible coherent memory is mapped with vkMapMemory, and the device file behind that
// mapping (its /proc/self/maps line: the emulator's /dev/goldfish_address_space, at an offset) is mapped again, MAP_SHARED | MAP_FIXED, over [mem, mem+size) after the
// pool's contents are copied in. The guest pointer is then GPU memory: CPU writes reach the GPU and back with no flush (probes/vkzc.c "remap" moved the same pages:
// 0 wrong words in 2 x 64 MB, read and write as fast as anonymous memory). Moving the driver's mapping onto the pool (mremap) worked too, but left its old range free
// until a placeholder covered it, and another thread's mapping landed there in between: the placeholder wiped it and malloc crashed at start-up. The second mapping
// leaves the driver's where it is. DM_VK_NO_ZC_MAP=1 turns it off. 0 on success
static int dev_mapping(const void *at, int *fd, uint64_t *off) {   // the device file and offset behind a mapped address: an open fd on that file
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return -1;
    char ln[512], path[256] = ""; unsigned long lo, hi, fo; int found = 0;
    while (fgets(ln, sizeof ln, f)) {
        int pos = 0; if (sscanf(ln, "%lx-%lx %*s %lx %*s %*s %n", &lo, &hi, &fo, &pos) < 3 || (uintptr_t)at < lo || (uintptr_t)at >= hi) continue;
        if (pos > 0) { snprintf(path, sizeof path, "%s", ln + pos); path[strcspn(path, "\n")] = 0; }
        *off = fo + ((uintptr_t)at - lo); found = path[0] == '/'; break; }
    fclose(f); if (!found) return -1;
    char lp[64], tg[256];
    for (int i = 0; i < 1024; i++) { snprintf(lp, sizeof lp, "/proc/self/fd/%d", i); ssize_t k = readlink(lp, tg, sizeof tg - 1);
        if (k > 0) { tg[k] = 0; if (!strcmp(tg, path)) { *fd = i; return 0; } } }
    return -1;
}
static int pool_zero_copy_map(vkpool_t *p, uint8_t *mem, uint64_t size) {
    static int off = -1; if (off < 0) off = getenv("DM_VK_NO_ZC_MAP") != NULL;
    if (off) return -6;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b; if (vkCreateBuffer(V.dev, &bi, NULL, &b)) return -6;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
    uint32_t t = vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);   // cached: the game reads its own memory
    if (t == UINT32_MAX) t = vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, b};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, req.size, t};
    VkDeviceMemory m; void *at = NULL; int fd = -1; uint64_t fo = 0;
    if (t == UINT32_MAX || vkAllocateMemory(V.dev, &ai, NULL, &m)) { vkDestroyBuffer(V.dev, b, NULL); return -6; }
    if (vkBindBufferMemory(V.dev, b, m, 0) || vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &at) || ((uintptr_t)at & 0xfff) || dev_mapping(at, &fd, &fo) || (fo & 0xfff)) {
        vkDestroyBuffer(V.dev, b, NULL); vkFreeMemory(V.dev, m, NULL); return -7; }
    memcpy(at, mem, size);
    if (mmap(mem, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)fo) != mem) { vkDestroyBuffer(V.dev, b, NULL); vkFreeMemory(V.dev, m, NULL); return -8; }
    p->buf = b; p->mem = m; p->zero_copy = 1; p->dfd = fd; p->doff = fo;
    return 0;
}
// A zero-copy pool's dma-buf mapped again over [mem, mem+len) in one piece per chunk, read-only (nvn_dirty_defrag: the mappings its protection split never merge). 0 = done
int vk_pool_remap_ro(void *mem, size_t len) {
    int rc = 0; uint64_t done = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool && !rc; i++) {
        vkpool_t *p = &g_pool[i]; if (p->base != (uint64_t)(uintptr_t)mem || p->end - p->base != len) continue;
        if (!p->zero_copy) { if (!p->shadow) { rc = -1; break; } done += p->hi - p->lo; continue; }   // a copied pool: nothing to map again (its protection stays as it is), and done, so defrag stops taking its pages busy
        if (!p->ahb) {   // zero copy through the driver's device file (pool_zero_copy_map)
            void *at = (void *)(uintptr_t)p->lo; rc = p->dfd >= 0 && mmap(at, p->hi - p->lo, PROT_READ, MAP_SHARED | MAP_FIXED, p->dfd, (off_t)p->doff) == at ? 0 : -1;
            if (!rc) done += p->hi - p->lo;
            continue;
        }
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
static void pool_add_one(const void *mem, uint64_t size, uint64_t base, uint64_t end) {
    pthread_rwlock_wrlock(&g_pool_mu);
    if (g_npool == MAXPOOL) { pthread_rwlock_unlock(&g_pool_mu); die("[vk] too many memory pools"); }
    vkpool_t *p = &g_pool[g_npool]; memset(p, 0, sizeof *p); p->dfd = -1;
    p->lo = (uint64_t)(uintptr_t)mem; p->hi = p->lo + size; p->base = base; p->end = end; { static uint64_t ser; p->serial = ++ser; }
    int zc = ((uintptr_t)mem & 0xfff) || (size & 0xfff) ? -3 : getenv("DM_VK_NO_ZEROCOPY") ? -4 : zc_over(end - base) ? -5 : pool_zero_copy(p, (uint8_t *)mem, size);
    if (zc == -1 && !(zc = pool_zero_copy_map(p, (uint8_t *)mem, size))) {   // no AHardwareBuffer for GPU data: the driver's device file mapped again instead
        static int said; if (!said++) VKLOG("memory pools: no AHardwareBuffer for GPU data, zero copy through the driver's device file mapped again at the pool"); }
    if (zc) {
        static int said; if (!said++) VKLOG("[RED] memory pool %p +%#llx could not be zero-copy (%d): a GPU copy is refreshed before each submission (slow)", mem, (unsigned long long)size, zc);
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &p->buf));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, p->buf, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
            vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &p->mem)); VKCHK(vkBindBufferMemory(V.dev, p->buf, p->mem, 0));
        void *m; VKCHK(vkMapMemory(V.dev, p->mem, 0, VK_WHOLE_SIZE, 0, &m)); p->shadow = m;
    }
    g_npool++; pool_ord_make(); __atomic_fetch_add(&g_pool_gen, 1, __ATOMIC_RELEASE);
    pthread_rwlock_unlock(&g_pool_mu);
    static uint64_t zc_bytes, cp_bytes; if (zc) cp_bytes += size; else zc_bytes += size;
    static int nlog; if (nlog++ < 24) VKLOG("memory pool %p +%#llx: %s (zero-copy %.1f MB, copied %.1f MB)", mem, (unsigned long long)size, zc ? "copied" : "zero-copy", zc_bytes / 1048576.0, cp_bytes / 1048576.0);
}
void mtl_pool_del(const void *mem) {
    for (;;) {   // every chunk of the pool
        pthread_rwlock_wrlock(&g_pool_mu);
        int i = 0; while (i < g_npool && g_pool[i].base != (uint64_t)(uintptr_t)mem) i++;
        if (i == g_npool) { pthread_rwlock_unlock(&g_pool_mu); return; }
        vkpool_t p = g_pool[i]; g_pool[i] = g_pool[--g_npool]; pool_ord_make(); __atomic_fetch_add(&g_pool_gen, 1, __ATOMIC_RELEASE);
        pthread_rwlock_unlock(&g_pool_mu);
        if (p.zero_copy) pool_copy_back(&p);
        vk_defer_free_buffer(p.buf, p.mem);   // the AHardwareBuffer goes with the imported memory's last reference
        if (p.ahb) AHardwareBuffer_release(p.ahb);
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
    *buf = p->buf; *off = addr - p->lo; return 1;
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
void vk_pools_sync(VkCommandBuffer cb) {
    (void)cb;
    enum { BLK = 64 << 10 };
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) {
        vkpool_t *p = &g_pool[i]; if (!p->shadow) continue;
        uint8_t *lo = (uint8_t *)(uintptr_t)p->lo; uint64_t size = p->hi - p->lo, st = nvn_dirty_now();
        if (!p->span && (p->span = nvn_dirty_span(lo, size))) p->synced = 0;   // tracked only now (nvnMemoryPoolInitialize registers the range after the pool): copied whole once more, protected
        if (!p->synced || !p->span) {   // untracked: every sync copies all of it, as before
            if (p->span) nvn_dirty_protect(lo, size);
            memcpy(p->shadow, lo, size); g_sync_bytes += size; g_sync_full++; p->synced = 1; p->stamp0 = p->stamp = st; continue; }
        // judged against the sync before the last: a write whose fault took its generation before the last sync's stamp but stored it after that sync looked
        // at its block would otherwise never count (its page is writable from then on, and clean by every later stamp). A dirty block is copied twice
        for (uint64_t off = 0; off < size; off += BLK) {
            uint64_t n = size - off < BLK ? size - off : BLK;
            if (!nvn_dirty_since_span(p->span, lo + off, n, p->stamp0)) continue;
            nvn_dirty_protect(lo + off, n); memcpy(p->shadow + off, lo + off, n); g_sync_bytes += n;
        }
        p->stamp0 = p->stamp; p->stamp = st;
    }
    pthread_rwlock_unlock(&g_pool_mu);
    static uint64_t n, last; if (++n % 600 == 0) { VKLOG("copied pools: %.1f MB synced in the last 600 submissions (%.2f MB each), %llu full copies so far", (g_sync_bytes - last) / 1048576.0, (g_sync_bytes - last) / 600.0 / 1048576.0, (unsigned long long)g_sync_full); last = g_sync_bytes; }
}
// Copied pools after a submission: what the GPU wrote, back into the guest's pages. A pool never synced yet is left alone (its copy holds nothing of the game's).
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
    struct { uint64_t lo, hi; } gw[64]; int ngw;   // GPU writes into pool memory in this submission (sub_gpu_wrote), for pools_copy_back: nearby ones merged, and once full each joins the nearest
};
enum { GW_SUB = 64 };
static vksub_t g_slot[NSLOT]; static int g_cur = -1; uint64_t g_cmd_gen;   // g_cmd_gen: main command buffers begun (gfx_vk_exec.c bind cache)
static void pools_copy_back(const struct vksub *s) {
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) { vkpool_t *p = &g_pool[i]; if (!p->shadow || !p->synced) continue;
        if (!(p->flags & 0x2)) { memcpy((void *)(uintptr_t)p->lo, p->shadow, p->hi - p->lo); continue; }
        for (int k = 0; k < s->ngw; k++) { uint64_t lo = s->gw[k].lo > p->lo ? s->gw[k].lo : p->lo, hi = s->gw[k].hi < p->hi ? s->gw[k].hi : p->hi;
            if (lo < hi) memcpy((void *)(uintptr_t)lo, p->shadow + (lo - p->lo), hi - lo); } }
    pthread_rwlock_unlock(&g_pool_mu);
}
static VkCommandBuffer g_slot_main_cb(void) { return g_slot[g_cur].main; }
// DM_VK_PASS_TIMES=1: a timestamp before and after every render pass (the replay's per-frame line then lists the passes that cost the most GPU time)
enum { PASSQ = 128 };   // passes timed per submission
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
    char b[1200]; int o = snprintf(b, sizeof b, "passes by GPU time (%d passes, %.2f ms in passes per frame):", m, tot);
    for (int i = 0; i < m && i < 10 && o < (int)sizeof b - 80; i++) { int k = idx[i]; if (!g_pcnt[k]) continue;
        o += snprintf(b + o, sizeof b - (size_t)o, " #%d %.2f ms (%ux%u fmt %u, %u draws over %u px wide);", k, g_pacc[k] / g_pcnt[k], g_plast[k].w, g_plast[k].h, g_plast[k].fmt, g_plast[k].draws & 0xffff, g_plast[k].draws >> 16); }
    { extern uint64_t g_nvn_discards[2]; static uint64_t pd0, pd1; o += snprintf(b + o, sizeof b - (size_t)o, " discards since: colour %llu, depth %llu", (unsigned long long)(g_nvn_discards[0] - pd0), (unsigned long long)(g_nvn_discards[1] - pd1)); pd0 = g_nvn_discards[0]; pd1 = g_nvn_discards[1]; }
    VKLOG("%s", b);
    memset(g_pacc, 0, sizeof g_pacc); memset(g_pcnt, 0, sizeof g_pcnt); g_pmax = 0;
}
// one submission at a time: the game's (hle_nvn.c holds its own lock around them) and the backend's own (texel readback, present)
static pthread_mutex_t g_sub_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fence;   // fence of the last commit
static VkSemaphore g_timeline;
static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_done;
static void slots_init(void) {
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    VKCHK(vkCreateSemaphore(V.dev, &si, NULL, &g_timeline));
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
static void wait_fence(uint64_t f) {
    if (!f) return;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &g_timeline, &f};
    vkWaitSemaphores(V.dev, &wi, UINT64_MAX);
}
// The slot's command buffers are reset on the completion thread once its submission is done (completion_main), not when the replay begins them again: an
// implicit reset at vkBeginCommandBuffer took 7% of Xenoblade Chronicles 3's replay thread at a submission's start and more at every part (its battles
// replay ~3,000 draws a frame on a thread that is the frame's limit). The slot is then taken only after the completion thread has passed it (mtl_wait, g_done)
// rather than as soon as the GPU has. DM_VK_X_NOPRERESET=1 (start-up): reset as they are begun
static int g_prereset = -1;
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static void slot_recycle(vksub_t *s) {   // the GPU has passed this slot's last submission: free what it held, rewind its allocators
    if (g_prereset < 0) { const char *e = getenv("DM_VK_X_NOPRERESET"); g_prereset = !(e && atoi(e)); }
    if (g_prereset) mtl_wait(s->fence, ~0ull); else wait_fence(s->fence);
    for (int i = 0; i < s->ndf; i++) {
        dfree_t *d = &s->df[i];
        if (d->kind == 0) { vkDestroyBuffer(V.dev, (VkBuffer)d->a, NULL); if (d->b) vkFreeMemory(V.dev, (VkDeviceMemory)d->b, NULL); }
        else if (d->kind == 1) { vkDestroyImage(V.dev, (VkImage)d->a, NULL); if (d->b) vkFreeMemory(V.dev, (VkDeviceMemory)d->b, NULL); }
        else if (d->kind == 4) { void img_sub_free_(void *); vkDestroyImage(V.dev, (VkImage)d->a, NULL); img_sub_free_(d->b); }
        else if (d->kind == 2) vkDestroyImageView(V.dev, (VkImageView)d->a, NULL);
        else if (d->kind == 3) vkDestroyBufferView(V.dev, (VkBufferView)d->a, NULL);
    }
    s->ndf = 0;
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
void vk_cmd_split(void) {
    vksub_t *s = cur(); if (s->nmain_used == MAIN_CBS || s->nup_used == MAIN_CBS) return;
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
static void df_add(int kind, void *a, void *b) {
    if (g_cur < 0) {   // outside a submission (a pool finalized between frames): after everything submitted so far
        wait_fence(g_fence);
        if (kind == 0) { vkDestroyBuffer(V.dev, (VkBuffer)a, NULL); if (b) vkFreeMemory(V.dev, (VkDeviceMemory)b, NULL); }
        else if (kind == 1) { vkDestroyImage(V.dev, (VkImage)a, NULL); if (b) vkFreeMemory(V.dev, (VkDeviceMemory)b, NULL); }
        else if (kind == 4) { void img_sub_free_(void *); vkDestroyImage(V.dev, (VkImage)a, NULL); img_sub_free_(b); }
        else if (kind == 2) vkDestroyImageView(V.dev, (VkImageView)a, NULL);
        else vkDestroyBufferView(V.dev, (VkBufferView)a, NULL);
        return;
    }
    vksub_t *s = cur(); if (s->ndf == s->capdf) { s->capdf = s->capdf ? 2 * s->capdf : 64; s->df = realloc(s->df, s->capdf * sizeof *s->df); }
    s->df[s->ndf++] = (dfree_t){kind, a, b};
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
    pthread_mutex_lock(&g_dsl_mu); nvn_hm_put(&g_dsl_need, &l, need); pthread_mutex_unlock(&g_dsl_mu);
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
void mtl_sub_begin(void) {
    vk_init();
    pthread_mutex_lock(&g_sub_lock);
    // the next slot of the ring (g_cur is -1 between submissions, so it cannot say where the ring is: every submission took slot 0 and waited in slot_recycle
    // for the one before to finish on the GPU, which kept the replay of a frame from overlapping the GPU work of the last)
    static int next; g_cur = next; next = (next + 1) % NSLOT; vksub_t *s = &g_slot[g_cur];
    slot_recycle(s);
    s->npost = 0; s->up_used = 0; s->ngw = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    s->main = s->mains[0]; s->nmain_used = 1; s->cb_err = 0; s->up = s->ups[0]; s->nup_used = 1; s->nmain_sent = 0;
    VKCHK(vkBeginCommandBuffer(s->main, &bi)); g_cmd_gen++;
    // copied pools: what the game wrote before submitting goes into the GPU copy before any part of the submission can reach the GPU (DM_VK_CB_DRAWS sends
    // parts early; synced at the commit, those parts read the copy as the last submission left it), and only once no earlier submission still reads it
    pthread_rwlock_rdlock(&g_pool_mu); int shadow = 0; for (int i = 0; i < g_npool; i++) shadow |= g_pool[i].shadow != NULL; pthread_rwlock_unlock(&g_pool_mu);
    g_shadow_pools = shadow;
    if (shadow) { mtl_wait(g_fence, ~0ull); vk_pools_sync(NULL); }   // (after the completion thread's copy-back of the last submission, not only its GPU work)
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
static void *completion_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "dm-vk-complete");
    for (;;) {
        pthread_mutex_lock(&g_cmu);
        while (!g_ch) pthread_cond_wait(&g_ccv, &g_cmu);
        cpend *c = g_ch; pthread_mutex_unlock(&g_cmu);
        wait_fence(c->fence);
        if (g_shadow_pools && c->rslot >= 0) pools_copy_back(&g_slot[c->rslot]);
        if (g_tsq && c->slot >= 0) { uint64_t t[2]; if (vkGetQueryPoolResults(V.dev, g_tsq, 2 * (uint32_t)c->slot, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
            { uint64_t ns = (uint64_t)((double)(t[1] - t[0]) * V.props.limits.timestampPeriod); g_gpu_ns += ns; int k = (int)(c->frame % 64);
              if (g_frame_gpu_of[k] != c->frame + 1) { g_frame_gpu[k] = 0; g_frame_gpu_of[k] = c->frame + 1; } g_frame_gpu[k] += ns; } }
        if (g_pq && pass_times_on() && c->slot >= 0) {   // this submission's passes
            int np = c->npass; uint64_t t[PASSQ * 2];
            if (np && vkGetQueryPoolResults(V.dev, g_pq, (uint32_t)(c->slot * PASSQ * 2), (uint32_t)np * 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                for (int i = 0; i < np; i++) if (t[2 * i + 1] > t[2 * i]) { int f = (int)c->pmeta[i].fidx; g_pacc[f] += (double)(t[2 * i + 1] - t[2 * i]) * V.props.limits.timestampPeriod / 1e6; g_pcnt[f]++; g_plast[f] = c->pmeta[i]; if (f + 1 > g_pmax) g_pmax = f + 1; }
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
    vksub_t *s = cur();
    vk_end_pass_land();
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
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); pthread_mutex_lock(&g_vk_queue_mu); VkResult r = vkQueueSubmit(V.q, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&g_vk_queue_mu); clock_gettime(CLOCK_MONOTONIC, &t1); g_ns_submit += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
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
