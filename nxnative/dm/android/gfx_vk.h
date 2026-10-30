// Android port: the Vulkan backend behind the replay interface of nvn_mtl.h (what nvn_mtl.m implements on Metal).
// Files: gfx_vk_core.c (device, memory pools, submissions, completion), gfx_vk_tex.c (formats, textures, samplers, registration tables),
// gfx_vk_shader.c (shader bundle, modules, layouts), gfx_vk_exec.c (command replay: passes, pipelines, descriptors, draws, copies), gfx_vk_present.c (window, swapchain).
// Version 1 is correct-first: whole-pipeline barriers between passes, full pipelines with a small dynamic state set, descriptor sets per draw.
#pragma once
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include "dm.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"

#define VKLOG(...) LOG("[vk] " __VA_ARGS__)
#define VKRED_ONCE(id, ...) do { static int once_##id; if (!once_##id++) LOG("[RED] [vk] " __VA_ARGS__); } while (0)
#define VKCHK(x) do { VkResult r_ = (x); if (r_ < 0) die("[vk] %s failed: %d", #x, r_); } while (0)

// ---- Vulkan entry points (loaded from libvulkan.so) ----
#define VK_FNS(X) \
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateAndroidSurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR)
#define VK_DEV_FNS(X) \
    X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkBindImageMemory) X(vkGetBufferMemoryRequirements) \
    X(vkGetImageMemoryRequirements) X(vkCreateImage) X(vkDestroyImage) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) X(vkMapMemory) \
    X(vkCreateCommandPool) X(vkResetCommandPool) X(vkResetCommandBuffer) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) \
    X(vkCreateSemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkDestroyFence) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdResolveImage) \
    X(vkCreateRenderPass) X(vkCreateRenderPass2) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetDepthBias) X(vkCmdSetLineWidth) X(vkCmdDraw) X(vkCmdPushConstants) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateComputePipelines) X(vkCreatePipelineCache) \
    X(vkGetPipelineCacheData) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateBufferView) X(vkDestroyBufferView) X(vkGetAndroidHardwareBufferPropertiesANDROID) \
    X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkCmdBindTransformFeedbackBuffersEXT) X(vkCmdBeginTransformFeedbackEXT) X(vkCmdEndTransformFeedbackEXT) \
    X(vkGetRefreshCycleDurationGOOGLE) X(vkGetPastPresentationTimingGOOGLE)
#define VK_DECL(f) extern PFN_##f f;
VK_FNS(VK_DECL) VK_DEV_FNS(VK_DECL)

// ---- device ----
typedef struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties mem;
    VkPipelineCache pcache;
    int has_bc, has_astc, has_gs, has_depth_clip_ctl, has_custom_border, has_robust2, has_xfb, has_imageless, has_dtiming, has_dynrender;
    int has_timeline, has_rp2;   // the device gives vkWaitSemaphores, vkCreateRenderPass2 (by either name; vk_init asks the device itself)
    float max_aniso;
} vkdev_t;
extern vkdev_t V;
uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid);
void vk_init(void);   // once, from mtl_init or the first use
VkResult vk_create_render_pass(const VkRenderPassCreateInfo2 *ci, VkRenderPass *rp);   // vkCreateRenderPass2, or the same pass with vkCreateRenderPass
void vk_fb_view_gone(VkImageView v);   // gfx_vk_exec.c: a view is being destroyed, and the framebuffers made over it (without imageless framebuffers) with it

// ---- memory pools (gfx_vk_core.c): the guest's pool memory is GPU memory (AHardwareBuffer mapped over it) ----
typedef struct { uint64_t lo, hi; VkBuffer buf; VkDeviceMemory mem; void *ahb; int zero_copy; uint8_t *shadow; uint64_t serial, base, end; int flags, synced; uint64_t stamp, stamp0; const void *span; } vkpool_t;   // serial: unique per pool made (caches of views over a pool's buffer key on it); [base, end): the game's pool, of which [lo, hi) is one chunk (ZC_CHUNK); flags: the NVN pool's (vk_pool_set_flags); synced, stamp, stamp0, span: a copied pool's incremental sync
vkpool_t *vk_pool_of(uint64_t addr, uint64_t len);   // the pool containing [addr, addr+len), NULL if none
int vk_buf_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);   // GPU buffer + offset of a pool address range
// Pools that could not be made zero-copy keep a GPU copy refreshed before each submission (vk_pools_sync, called from mtl_sub_commit)
void vk_pools_sync(VkCommandBuffer cb);

// ---- submissions (gfx_vk_core.c) ----
typedef struct vksub vksub_t;
VkCommandBuffer vk_cmd(void);          // the current submission's main command buffer (recording)
int vk_sub_open(void);                 // a submission is being recorded
VkCommandBuffer vk_upload_cmd(void);   // the current submission's upload command buffer (runs before the main one)
void *vk_scratch(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off);   // host-visible memory that lives until this submission completes
void vk_defer_free_buffer(VkBuffer b, VkDeviceMemory m);   // destroyed once the GPU has passed the current submission
void vk_defer_free_image(VkImage i, VkDeviceMemory m);
VkDeviceMemory vk_image_memory(const VkMemoryRequirements *req, uint32_t type, VkDeviceSize *off, void **sub);   // NULL on failure; *sub: a slot of a shared block, or NULL (memory of its own)
void vk_defer_free_image_sub(VkImage i, void *sub);
void vk_defer_free_view(VkImageView v);
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout l);    // from this submission's descriptor pools
uint64_t vk_fence_done(void);   // the highest submission fence the GPU has completed

// ---- formats and textures (gfx_vk_tex.c) ----
typedef struct { VkFormat vk; VkComponentMapping swz; uint8_t kind; uint8_t depth, stencil, compressed, is_int, is_sint, bw, bh, bpb; } vkfmt_t;
const vkfmt_t *vk_fmt(int nvn_format);   // NULL when the format has no Vulkan counterpart on this device
typedef struct vkimg vkimg_t;
struct vkimg {
    VkImageUsageFlags usage; VkImageCreateFlags cflags;   // as created (an imageless framebuffer names them)
    VkImage img; VkDeviceMemory mem; void *sub; VkFormat fmt; VkImageViewType vtype; VkImageAspectFlags aspect;
    uint32_t w, h, d, layers, levels, samples; int target, nvn_format;
    VkImageLayout layout;           // version 1 keeps every image in GENERAL after its first use
    int packaged;                   // filled from guest memory (deswizzled upload), re-uploaded when its pages change
    uint64_t pkg_stamp; const void *pkg_span; uint64_t pkg_seen;
    const uint8_t *pkg_mem; size_t pkg_len; uint64_t pkg_used; int pkg_released;   // its storage, the present it was last drawn at, protection given up (vk_tex_sweep)
    uint64_t used;                  // the present it was last looked up at (vk_tex: evict_overlaps)
    uint64_t fdz_frame;             // depth images: 1 + the present at which a fragment shader that replaces depth last wrote it (gfx_vk_exec.c fdz)
    int gpu_w;                      // the GPU wrote it: an attachment, a storage image, a copy or clear destination (gfx_vk_exec.c); lin_copy keeps that until the CPU writes its storage
    uint64_t lin_stamp; const void *lin_span; const uint8_t *lin_mem; size_t lin_len;   // lin_copy: the storage watched since the GPU wrote it
    uint64_t lin_gen;               // linear textures: the upload command buffer (g_up_gen) that last copied it from its pool (vk_tex: lin_copy)
    uint64_t depth_sub;   // the submission (gfx_vk_exec.c g_set_epoch) whose passes last used it as their depth attachment
    uint64_t depth_cut;   // and the command buffer within it (gfx_vk_exec.c g_cuts)
    int fixed_fmt;   // created with a format list (DM_VK_UBWC): views only in its format or its sRGB twin
    VkFormat fmt2;   // with fixed_fmt: the other format of its list (its sRGB twin), VK_FORMAT_UNDEFINED when the list is its format alone
    VkImageView full;               // a view of the whole image as created
    VkBufferView tbuf;              // buffer textures: a texel buffer view of the pool
    uint64_t last_use;              // submission fence of the last use
    uint64_t pool; int64_t off, size; int flags;   // its storage (pool, offset, layout size) and NVN texture flags: images over the same memory find each other
    struct vkimg *ovl[8]; int novl;   // the other images over some of the same memory (a texture of another description over it: on the Switch it sees these texels)
    uint64_t wseq, rseen;             // when it was last rendered to (gfx_vk_tex.c g_wseq); the newest overlapping write already reported
};
vkimg_t *vk_tex(const tex_t *t);                                 // the image of a texture object (created, uploaded or refreshed as needed)
void vk_lrz_forget(vkimg_t *im);                                 // an image is being dropped: the pass code forgets it (gfx_vk_exec.c)
int vk_window_bottom_up(void);                                   // the game draws in lower-left origin mode: its window image is stored bottom-up (gfx_vk_exec.c)
VkImageView vk_tex_view(const tex_t *t, const view_t *v, int dim, int arrayed, int as_attachment, vkimg_t **img_out);   // a view for sampling / storage / rendering
void vk_watch(const vkimg_t *im, const char *what);   // DM_VK_WATCH (gfx_vk_exec.c)
vkimg_t *vk_tex_by_id(int tid, const view_t **view_out, const tex_t **tex_out);   // registered texture ID → image (NULL if none)
VkSampler vk_samp_by_id(int sid);
extern __thread VkFormat vk_view_fmt; extern __thread uint32_t vk_view_layers;   // the format and layer count of the view vk_tex_view last returned on this thread
VkImageView vk_dummy_view(int dim, int arrayed, int ms, int storage, int is_int);
VkSampler vk_dummy_sampler(void);
VkBufferView vk_dummy_tbuf(int storage);
void vk_end_pass_land(void);   // vk_end_pass, then the deferred uniform updates land (before copies, dispatches, the submission's end)
void vk_img_barrier_all(VkCommandBuffer cb);   // version 1: one global memory barrier (all images stay in GENERAL)
void vk_tex_prepare(VkCommandBuffer cb, vkimg_t *im);   // first use: transition from UNDEFINED to GENERAL (and upload a packaged texture)

// ---- shaders (gfx_vk_shader.c) ----
enum { RK_SUPPORT, RK_CBUF, RK_C1, RK_DRV, RK_SBUF, RK_UBO_PTR, RK_SBUF_UBO, RK_TEX, RK_TBUF, RK_IMG, RK_IMG_TBUF, RK_TEX_C2A, RK_TEX_UBO, RK_TBUF_UBO, RK_IMG_UBO, RK_UNKNOWN = 255 };
enum { RD_UBO, RD_SSBO, RD_SAMPLED, RD_STORAGE, RD_TBUF, RD_STORAGE_TBUF, RD_IMAGE, RD_SAMPLER };
enum { RS_NONE, RS_TEX, RS_SLOT, RS_ID, RS_UBO };
typedef struct __attribute__((packed)) { uint8_t kind, desc, dim, flags; uint16_t binding, count; int16_t reg; uint8_t samp, fmt; int32_t off, samp_arg; } dmsv_bind;
_Static_assert(sizeof(dmsv_bind) == 20, "dmsv_bind");
typedef struct {
    uint64_t key; int stage;          // NVN stage: 0 vertex 1 fragment 2 geometry 3 tess control 4 tess eval 5 compute
    VkShaderModule mod;
    VkDescriptorSetLayout dsl; int set;   // the set this stage's bindings were moved to (vertex 0, fragment 1, geometry 2, compute 0)
    int nbind; const dmsv_bind *b;
    const uint8_t *c1; uint32_t c1_len; VkBuffer c1_buf; VkDeviceSize c1_off;
    uint32_t local[3];
    uint8_t dyn[96]; int ndyn;          // bindings of uniform buffers bound with a dynamic offset (gfx_vk_exec.c stage_set), and their count
    int xfb; uint32_t xfb_stride[4];    // a vertex shader that captures transform feedback (its SPIR-V has the Xfb execution mode), and each buffer's stride
    uint32_t *spv, nspv;                // its SPIR-V as the module was made from it, for the variants below
    struct { uint64_t um, im; uint32_t xo; int fd; VkShaderModule mod; } var[8]; int nvar;   // variants whose sampled bindings (bit = index in b) of an integer view read uint (um) / int (im) texels,
                                                                                    // that declare the vec4 outputs xo (bit = location) the next stage reads, and (fd) that write gl_FragDepth = gl_FragCoord.z
    int depth_repl;                     // a fragment shader that writes gl_FragDepth (DepthReplacing / DepthGreater / DepthLess)
    uint32_t in_locs, out_locs;         // the vec4 (or per-vertex vec4) inputs and outputs it declares, by location
    int setmemo;                        // gfx_vk_exec.c stage_set: 1 its set may be memoized per stage, 2 not (it reads handles or addresses from uniform buffer memory), 0 not known yet
} vkshader_t;
vkshader_t *vk_shader(uint64_t key, int stage);   // NULL when the bundle has no such shader
VkShaderModule vk_shader_module(vkshader_t *s, uint64_t um, uint64_t im, uint32_t xo, int fd);   // the module, or its variant (integer views; extra outputs; fragment depth written)
void vk_shaders_open(const char *path);
VkBuffer vk_support_buffer(void);
VkBuffer vk_zero_buffer(void);

// ---- replay (gfx_vk_exec.c) ----
void vk_exec_reset(void);   // per submission
void vk_end_pass(void);     // close an open render pass

// ---- presentation (gfx_vk_present.c) ----
void vk_present_init(void);
uint64_t vk_present(const tex_t *t, double period, const int crop[4]);
