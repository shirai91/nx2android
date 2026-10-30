// Android port, Vulkan backend: formats, textures, views, samplers and the texture / sampler ID tables (see gfx_vk.h).
//   A texture object's image is found by its storage: textures over the same pool, offset and description share one image (as nvn_mtl.m's storage-sharing
//   table), which is how a render target and the texture a later pass samples it through meet. A packaged texture (packaged layout, CPU-written) is filled
//   from the guest's block-linear bytes (nvn_deswizzle on the CPU into scratch memory, copied in by the upload command buffer) and filled again when the
//   dirty-page tracker saw a write to its pages; any other texture is the GPU's (render targets, images), filled only by GPU work.
//   Version 1 keeps every image in VK_IMAGE_LAYOUT_GENERAL after its first use, ordered by whole-pipeline barriers.
#include "gfx_vk.h"
#include "nvn_tex.h"
#include "../gfx/tex/nvn_deswizzle.h"
#include "nvn_hmap.h"
#include <string.h>

int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp);
const void *nvn_dirty_span(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len); void nvn_dirty_release(const void *addr, size_t len); uint64_t nvn_dirty_released(void); int nvn_dirty_runs(void);
uint64_t nvn_dirty_now(void);

// ---------------- formats ----------------
enum { K_NONE, K_FLOAT, K_INT, K_SINT };
typedef struct { uint16_t vk; uint8_t noalpha, rb, kind; } fent;   // vk: VkFormat (16 bits suffice for the core formats; ASTC / packed extras computed)
#define F(v) {v, 0, 0, K_FLOAT}
#define FX(v) {v, 1, 0, K_FLOAT}
#define U(v) {v, 0, 0, K_INT}
#define UX(v) {v, 1, 0, K_INT}
#define I(v) {v, 0, 0, K_SINT}
#define IX(v) {v, 1, 0, K_SINT}
static const fent FT[0x9D] = {
    [0x01] = F(VK_FORMAT_R8_UNORM), F(VK_FORMAT_R8_SNORM), U(VK_FORMAT_R8_UINT), I(VK_FORMAT_R8_SINT),
    [0x05] = F(VK_FORMAT_R16_SFLOAT), F(VK_FORMAT_R16_UNORM), F(VK_FORMAT_R16_SNORM), U(VK_FORMAT_R16_UINT), I(VK_FORMAT_R16_SINT),
    [0x0A] = F(VK_FORMAT_R32_SFLOAT), U(VK_FORMAT_R32_UINT), I(VK_FORMAT_R32_SINT),
    [0x0D] = F(VK_FORMAT_R8G8_UNORM), F(VK_FORMAT_R8G8_SNORM), U(VK_FORMAT_R8G8_UINT), I(VK_FORMAT_R8G8_SINT),
    [0x11] = F(VK_FORMAT_R16G16_SFLOAT), F(VK_FORMAT_R16G16_UNORM), F(VK_FORMAT_R16G16_SNORM), U(VK_FORMAT_R16G16_UINT), I(VK_FORMAT_R16G16_SINT),
    [0x16] = F(VK_FORMAT_R32G32_SFLOAT), U(VK_FORMAT_R32G32_UINT), I(VK_FORMAT_R32G32_SINT),
    // 0x19-0x24 three-component formats: not sampleable on mobile GPUs (vertex input only, see vk_vertex_format)
    [0x25] = F(VK_FORMAT_R8G8B8A8_UNORM), F(VK_FORMAT_R8G8B8A8_SNORM), U(VK_FORMAT_R8G8B8A8_UINT), I(VK_FORMAT_R8G8B8A8_SINT),
    [0x29] = F(VK_FORMAT_R16G16B16A16_SFLOAT), F(VK_FORMAT_R16G16B16A16_UNORM), F(VK_FORMAT_R16G16B16A16_SNORM), U(VK_FORMAT_R16G16B16A16_UINT), I(VK_FORMAT_R16G16B16A16_SINT),
    [0x2E] = F(VK_FORMAT_R32G32B32A32_SFLOAT), U(VK_FORMAT_R32G32B32A32_UINT), I(VK_FORMAT_R32G32B32A32_SINT),
    [0x31] = U(VK_FORMAT_S8_UINT), F(VK_FORMAT_D16_UNORM), F(VK_FORMAT_X8_D24_UNORM_PACK32), F(VK_FORMAT_D32_SFLOAT), F(VK_FORMAT_D24_UNORM_S8_UINT), F(VK_FORMAT_D32_SFLOAT_S8_UINT),
    [0x37] = FX(VK_FORMAT_R8G8B8A8_SRGB), F(VK_FORMAT_R8G8B8A8_SRGB),
    [0x39] = F(VK_FORMAT_A4B4G4R4_UNORM_PACK16), FX(VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR), F(VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR), F(VK_FORMAT_R5G6B5_UNORM_PACK16),
    [0x3D] = F(VK_FORMAT_A2B10G10R10_UNORM_PACK32), U(VK_FORMAT_A2B10G10R10_UINT_PACK32), F(VK_FORMAT_B10G11R11_UFLOAT_PACK32), F(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32),
    [0x41] = F(VK_FORMAT_BC1_RGB_UNORM_BLOCK), F(VK_FORMAT_BC1_RGBA_UNORM_BLOCK), F(VK_FORMAT_BC2_UNORM_BLOCK), F(VK_FORMAT_BC3_UNORM_BLOCK),
             F(VK_FORMAT_BC1_RGB_SRGB_BLOCK), F(VK_FORMAT_BC1_RGBA_SRGB_BLOCK), F(VK_FORMAT_BC2_SRGB_BLOCK), F(VK_FORMAT_BC3_SRGB_BLOCK),
    [0x49] = F(VK_FORMAT_BC4_UNORM_BLOCK), F(VK_FORMAT_BC4_SNORM_BLOCK), F(VK_FORMAT_BC5_UNORM_BLOCK), F(VK_FORMAT_BC5_SNORM_BLOCK),
             F(VK_FORMAT_BC7_UNORM_BLOCK), F(VK_FORMAT_BC7_SRGB_BLOCK), F(VK_FORMAT_BC6H_SFLOAT_BLOCK), F(VK_FORMAT_BC6H_UFLOAT_BLOCK),
    // *_UI2F / *_I2F: integer storage read as float without normalization = Vulkan's scaled formats
    [0x51] = F(VK_FORMAT_R8_USCALED), F(VK_FORMAT_R8_SSCALED), F(VK_FORMAT_R16_USCALED), F(VK_FORMAT_R16_SSCALED), U(VK_FORMAT_R32_UINT), I(VK_FORMAT_R32_SINT),
    [0x57] = F(VK_FORMAT_R8G8_USCALED), F(VK_FORMAT_R8G8_SSCALED), F(VK_FORMAT_R16G16_USCALED), F(VK_FORMAT_R16G16_SSCALED), U(VK_FORMAT_R32G32_UINT), I(VK_FORMAT_R32G32_SINT),
    [0x63] = F(VK_FORMAT_R8G8B8A8_USCALED), F(VK_FORMAT_R8G8B8A8_SSCALED), F(VK_FORMAT_R16G16B16A16_USCALED), F(VK_FORMAT_R16G16B16A16_SSCALED), U(VK_FORMAT_R32G32B32A32_UINT), I(VK_FORMAT_R32G32B32A32_SINT),
    [0x69] = F(VK_FORMAT_A2B10G10R10_SNORM_PACK32), I(VK_FORMAT_A2B10G10R10_SINT_PACK32), F(VK_FORMAT_A2B10G10R10_USCALED_PACK32), F(VK_FORMAT_A2B10G10R10_SSCALED_PACK32),
    [0x6D] = FX(VK_FORMAT_R8G8B8A8_UNORM), FX(VK_FORMAT_R8G8B8A8_SNORM), UX(VK_FORMAT_R8G8B8A8_UINT), IX(VK_FORMAT_R8G8B8A8_SINT),
    [0x71] = FX(VK_FORMAT_R16G16B16A16_SFLOAT), FX(VK_FORMAT_R16G16B16A16_UNORM), FX(VK_FORMAT_R16G16B16A16_SNORM), UX(VK_FORMAT_R16G16B16A16_UINT), IX(VK_FORMAT_R16G16B16A16_SINT),
    [0x76] = FX(VK_FORMAT_R32G32B32A32_SFLOAT), UX(VK_FORMAT_R32G32B32A32_UINT), IX(VK_FORMAT_R32G32B32A32_SINT),
    [0x95] = F(VK_FORMAT_B5G6R5_UNORM_PACK16), FX(VK_FORMAT_A1R5G5B5_UNORM_PACK16), F(VK_FORMAT_A1R5G5B5_UNORM_PACK16), F(VK_FORMAT_R5G5B5A1_UNORM_PACK16),
    [0x99] = FX(VK_FORMAT_B8G8R8A8_UNORM), F(VK_FORMAT_B8G8R8A8_UNORM), FX(VK_FORMAT_B8G8R8A8_SRGB), F(VK_FORMAT_B8G8R8A8_SRGB),
};
static vkfmt_t g_fmt[0x9D]; static uint8_t g_fmt_done[0x9D];
static pthread_mutex_t g_fmt_mu = PTHREAD_MUTEX_INITIALIZER;
const vkfmt_t *vk_fmt(int f) {
    if (f <= 0 || f >= 0x9D) return NULL;
    if (__atomic_load_n(&g_fmt_done[f], __ATOMIC_ACQUIRE)) return g_fmt[f].kind ? &g_fmt[f] : NULL;   // made already: no lock (the replay looks formats up for every texture it binds)
    pthread_mutex_lock(&g_fmt_mu);
    if (!g_fmt_done[f]) {
        vkfmt_t *o = &g_fmt[f]; memset(o, 0, sizeof *o);
        fent e = FT[f];
        if (f >= 0x79 && f <= 0x94) { int k = (f - 0x79) % 14; e = (fent){(uint16_t)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * k + (f >= 0x87)), 0, 0, K_FLOAT}; }
        if (e.vk) {
            int bw, bh; o->bpb = (uint8_t)nvn_fmt_bpb(f, &bw, &bh); o->bw = (uint8_t)bw; o->bh = (uint8_t)bh;
            o->vk = (VkFormat)(e.vk == (uint16_t)VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR ? VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR : e.vk);
            if (f == 0x33) {   // DEPTH24: X8_D24 where the device renders to it, else D32
                VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_X8_D24_UNORM_PACK32, &fp);
                if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) o->vk = VK_FORMAT_D32_SFLOAT;
            }
            // DEPTH24_STENCIL8: D32_SFLOAT_S8 where the device renders to no D24S8 (Apple GPUs: in the Android emulator on an M3 Max, MoltenVK gives D24S8 no format
            // features at all, probes/vkprobe.c). Metal makes the same substitute (nvn_fmt_mtl.c: a precision upgrade as a render target)
            VkFormatProperties d24; vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_D24_UNORM_S8_UINT, &d24);
            VkFormat ds = d24.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
            if (f == 0x35) o->vk = ds;
            if (f == 0x31) {   // STENCIL8 alone: a combined format where the device has no S8 attachments
                VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_S8_UINT, &fp);
                if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) o->vk = ds;
            }
            o->depth = f >= 0x32 && f <= 0x36; o->stencil = f == 0x31 || f == 0x35 || f == 0x36 || (f == 0x31);
            o->compressed = o->bw > 1 || o->bh > 1;
            o->is_int = e.kind == K_INT || e.kind == K_SINT; o->is_sint = e.kind == K_SINT; o->kind = 1;
            o->swz = (VkComponentMapping){VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, e.noalpha ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY};
            if (V.astc_cpu && o->vk >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && o->vk <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {   // decoded on the CPU (pkg_upload_) into an RGBA8 image
                o->abw = o->bw; o->abh = o->bh; o->bw = o->bh = 1; o->bpb = 4; o->compressed = 0;
                o->vk = (o->vk - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            }
            if ((o->compressed && o->vk >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && o->vk <= VK_FORMAT_BC7_SRGB_BLOCK && !V.has_bc) ||
                (o->vk >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && o->vk <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK && !V.has_astc)) {
                o->kind = 0;   // said once per format: these textures went missing with nothing in the log (Adreno 6xx drivers of 2021 have no BC)
                VKLOG("[RED] texture format %#x (VkFormat %d) needs %s, which the device lacks: textures in it are not drawn", f, o->vk,
                      o->vk <= VK_FORMAT_BC7_SRGB_BLOCK ? "BC (textureCompressionBC)" : "ASTC (textureCompressionASTC_LDR)");
            }
        }
        __atomic_store_n(&g_fmt_done[f], 1, __ATOMIC_RELEASE);   // published once complete
    }
    pthread_mutex_unlock(&g_fmt_mu);
    return g_fmt[f].kind ? &g_fmt[f] : NULL;
}
static VkImageAspectFlags aspect_of(const vkfmt_t *f) {
    if (f->vk == VK_FORMAT_S8_UINT) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f->depth || f->stencil) return VK_IMAGE_ASPECT_DEPTH_BIT | (f->vk == VK_FORMAT_D24_UNORM_S8_UINT || f->vk == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

// ---------------- images ----------------
static unsigned layers_of(const nvn_texdesc *d) { return d->target == 4 || d->target == 9 ? (d->depth ? d->depth : 1) : d->target == 8 ? 6 : d->target == 3 ? (d->height ? d->height : 1) : 1; }
static VkImageViewType vtype_of(int target) {
    switch (target) {
    case 0: return VK_IMAGE_VIEW_TYPE_1D; case 3: return VK_IMAGE_VIEW_TYPE_1D_ARRAY; case 2: return VK_IMAGE_VIEW_TYPE_3D;
    case 4: case 6: return VK_IMAGE_VIEW_TYPE_2D_ARRAY; case 8: return VK_IMAGE_VIEW_TYPE_CUBE; case 9: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}
// storage key: textures with the same pool, offset and description share one image
typedef struct { uint64_t pool; int64_t off; nvn_texdesc d; } akey;
static nvn_hmap g_by_key = {.klen = sizeof(akey)};     // akey → vkimg_t*
static nvn_hmap g_by_serial = {.klen = sizeof(uint64_t)};  // serial → vkimg_t* (images with no storage key: no pool memory)
static pthread_mutex_t g_img_mu = PTHREAD_MUTEX_INITIALIZER;
// The sRGB / UNORM twin of a format (the one reinterpretation games make of uncompressed colour targets), VK_FORMAT_UNDEFINED when it has none
static VkFormat srgb_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB; case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB; case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32; case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}
// DM_VK_UBWC (start-up, bits): 1, uncompressed colour images get a format list (their format and its sRGB twin) instead of a free MUTABLE_FORMAT, which Adreno
// needs to keep UBWC framebuffer compression; views in another format then use the image's own (vk_tex_view). 2, depth / stencil images are made without
// MUTABLE_FORMAT and EXTENDED_USAGE (their views always take the image's own format). Unicorn Overlord on an Adreno 730: 1 cut GPU time by 35-40%. Both are the
// default (3): Super Mario Bros. Wonder's world map on the Adreno 830 took 8.7-8.9 ms of GPU a frame instead of 11.8, the same picture (1 alone: 9.5-10.0 ms).
// Xenoblade Chronicles 3's field took 22.9-23.0 ms against 23.0-23.6 (its own shading dominates), with clouds in every frame of a sky turn and no idle flicker;
// Graveyard Keeper 2, Advance Wars and DKCR drew the same pictures. A colour view in another format than the image's and its twin is logged [RED] (vk_tex_view):
// such a game wants DM_VK_UBWC=2 in its profile
static int ubwc_mode(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_UBWC"); v = e ? atoi(e) : 3; } return v; }
static vkimg_t *img_new(const tex_t *t, const vkfmt_t *f) {
    vkimg_t *im = calloc(1, sizeof *im);
    const nvn_texdesc *d = &t->d;
    im->w = d->width ? d->width : 1; im->h = d->target == 0 || d->target == 3 ? 1 : (d->height ? d->height : 1);
    im->d = d->target == 2 ? (d->depth ? d->depth : 1) : 1; im->layers = layers_of(d);
    im->levels = d->levels ? d->levels : 1; im->samples = d->samples > 1 ? d->samples : 1; im->target = d->target; im->nvn_format = d->format;
    im->fmt = f->vk; im->vtype = vtype_of(d->target); im->aspect = aspect_of(f);
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(V.pd, f->vk, &fp);
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((d->flags & NVN_TEXTURE_FLAGS_IMAGE) && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL,
        // a square 2D array of 6n layers too: shaders read one as a cube (Pokémon Legends: Arceus's reflection probes; the view was refused by the validation layer without the flag)
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | (d->target == 8 || d->target == 9 || (d->target == 4 && im->layers >= 6 && im->layers % 6 == 0 && im->w == im->h && im->samples <= 1) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
        (d->target == 2 ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0) | (f->compressed ? VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT : 0),
        d->target == 2 ? VK_IMAGE_TYPE_3D : d->target == 0 || d->target == 3 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D, f->vk,
        {im->w, im->h, im->d}, im->levels, im->layers, (VkSampleCountFlagBits)im->samples, VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (im->samples > 1) ci.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    // BLOCK_TEXEL_VIEW_COMPATIBLE only where the device takes it for the format: the Android emulator's MoltenVK refuses it for every BC format (FORMAT_NOT_SUPPORTED, and
    // vkCreateImage FEATURE_NOT_PRESENT), so every BC texture of Link's Awakening's menus failed. No view needs it: a view changing the block size keeps the image's format (vk_tex_view)
    if (f->compressed && (unsigned)f->vk < 256) {
        static _Atomic signed char btv[256];   // per format: 0 unknown, 1 taken, -1 refused
        if (!btv[f->vk]) { VkImageFormatProperties ip; btv[f->vk] = vkGetPhysicalDeviceImageFormatProperties(V.pd, f->vk, ci.imageType, ci.tiling, u, ci.flags, &ip) ? -1 : 1; }
        if (btv[f->vk] < 0) ci.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    }
    VkFormat fl[2] = {f->vk, srgb_twin(f->vk)}; VkImageFormatListCreateInfo fli = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, fl[1] ? 2u : 1u, fl};
    if (!f->compressed && (ubwc_mode() & (im->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? 1 : 2))) {   // (a depth format has no twin: a list of itself, no MUTABLE_FORMAT)
        im->fixed_fmt = 1; im->fmt2 = fl[1]; ci.pNext = &fli;
        ci.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; if (!fl[1]) ci.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    im->usage = ci.usage; im->cflags = ci.flags;
    VkResult r = vkCreateImage(V.dev, &ci, NULL, &im->img);
    if (r) { VKRED_ONCE(10, "vkCreateImage failed (%d): %ux%ux%u format %#x (vk %d) target %d levels %u layers %u samples %u", r, im->w, im->h, im->d, d->format, f->vk, d->target, im->levels, im->layers, im->samples); free(im); return NULL; }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, im->img, &req);
    if ((ci.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) && im->w >= 256) { static int n; if (n++ < 16)   // UBWC check: a compressed target takes room for its metadata
        VKLOG("target %ux%u format %d%s: %llu bytes", im->w, im->h, f->vk, im->fixed_fmt ? " (format-listed)" : "", (unsigned long long)req.size); }
    VkDeviceSize moff;
    if (!(im->mem = vk_image_memory(&req, vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT), &moff, &im->sub))) {
        VKRED_ONCE(11, "image memory allocation failed (%llu bytes)", (unsigned long long)req.size); vkDestroyImage(V.dev, im->img, NULL); free(im); return NULL; }
    VKCHK(vkBindImageMemory(V.dev, im->img, im->mem, moff));
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return im;
}
void vk_tex_prepare(VkCommandBuffer cb, vkimg_t *im) {
    if (im->layout == VK_IMAGE_LAYOUT_GENERAL) return;
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im->img, {im->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    im->layout = VK_IMAGE_LAYOUT_GENERAL;
}
// Upload a packaged texture from guest memory: each level and layer deswizzled into scratch memory, copied in by the upload command buffer
uint64_t g_pkg_n, g_pkg_bytes, g_pkg_ns;   // packaged texture uploads: count, bytes, CPU time (the replay's per-frame line)
static void pkg_upload_(vkimg_t *im, const tex_t *t, const vkfmt_t *f);
static void pkg_upload(vkimg_t *im, const tex_t *t, const vkfmt_t *f) {
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    pkg_upload_(im, t, f);
    clock_gettime(CLOCK_MONOTONIC, &b); g_pkg_ns += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)); g_pkg_n++; g_pkg_bytes += t->L.size;
}
static void pkg_upload_(vkimg_t *im, const tex_t *t, const vkfmt_t *f) {
    { static int n; const char *e = getenv("DM_VK_LOG_UPLOADS"); if (e && atoi(e) && n < 60) { n++;   // which packaged textures are (re-)uploaded
        VKLOG("upload: serial %llu %ux%ux%u nvn format %#x (vk %d) target %d levels %u layers %u flags %#x stride %u size %llu mem %p, tileH*100+pkg_block_h %d%s",
              (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, f->vk, t->d.target, im->levels, im->layers, t->d.flags, t->d.stride, (unsigned long long)t->L.size,
              (const void *)t->mem, (int)t->L.tileH * 100 + t->d.pkg_block_h, im->packaged > 1 ? " (again)" : ""); } }
    if (im->packaged < 100) im->packaged++;
    VkCommandBuffer cb = vk_upload_cmd();
    if (im->layout != VK_IMAGE_LAYOUT_GENERAL) vk_tex_prepare(cb, im);
    nvn_dirty_protect(t->mem, t->L.size);
    im->pkg_stamp = nvn_dirty_now(); im->pkg_span = nvn_dirty_span(t->mem, t->L.size); im->pkg_seen = im->pkg_stamp;
    im->pkg_mem = t->mem; im->pkg_len = t->L.size; im->pkg_released = 0;
    int linear = (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0;
    unsigned bw = f->bw ? f->bw : 1, bh = f->bh ? f->bh : 1, bpb = f->bpb;
    if (!bpb) { VKRED_ONCE(12, "packaged texture format %#x has no block size", t->d.format); return; }
    for (unsigned lv = 0; lv < im->levels; lv++) {
        uint32_t w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1, z = im->d >> lv ? im->d >> lv : 1;
        uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh; size_t pitch = (size_t)bx * bpb, slab = pitch * by * z;
        for (unsigned ly = 0; ly < im->layers; ly++) {
            VkBuffer sb; VkDeviceSize so; uint8_t *dst = vk_scratch(slab, 16, &sb, &so);
            if (f->abw) {   // ASTC decoded here (V.astc_cpu): its blocks in rows as the device would take them, then each decoded into dst's RGBA8 rows
                unsigned abx = (w + f->abw - 1) / f->abw, aby = (h + f->abh - 1) / f->abh; size_t ap = (size_t)abx * 16;
                uint8_t *blk = malloc(ap * aby * z); if (!blk) return;
                if (linear) { size_t stride = t->d.stride ? t->d.stride : ap; for (uint32_t y = 0; y < aby * z; y++) memcpy(blk + y * ap, t->mem + (size_t)y * stride, ap); }
                else if (nvn_deswizzle(&t->d, lv, ly, t->mem, blk, ap)) { free(blk); VKRED_ONCE(13, "deswizzle refused texture format %#x target %d", t->d.format, t->d.target); return; }
                vk_astc_decode(blk, f->abw, f->abh, w, h, z, f->vk == VK_FORMAT_R8G8B8A8_SRGB, dst, pitch);
                free(blk);
            } else if (linear) {   // linear storage: rows of the texture's stride, one level, one layer
                size_t stride = t->d.stride ? t->d.stride : pitch;
                for (uint32_t y = 0; y < by * z; y++) memcpy(dst + y * pitch, t->mem + (size_t)y * stride, pitch);
            } else if (nvn_deswizzle(&t->d, lv, ly, t->mem, dst, pitch)) { VKRED_ONCE(13, "deswizzle refused texture format %#x target %d", t->d.format, t->d.target); return; }
            VkBufferImageCopy r = {so, 0, 0, {im->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, lv, ly, 1}, {0, 0, 0}, {w, h, z}};
            vkCmdCopyBufferToImage(cb, sb, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
    }
}
static int packaged_tex(const tex_t *t);
static int lin_copy(vkimg_t *im, const tex_t *t, const vkfmt_t *f);
static void vk_tex_view_drop_all(vkimg_t *im);
// Images over the same memory with different descriptions (the storage key differs) are separate Vulkan images: on the Switch a texture sees what another
// one over its memory rendered, here it does not. They are linked both ways at creation (up to 8 each), and a sampled image older than an overlapping
// image's last rendering is logged ([alias] stale read), to find what a game reinterprets
static uint64_t g_wseq;
static void alias_link(vkimg_t *im) {
    for (uint32_t i = 0; i < g_by_key.cap; i++) {
        vkimg_t *o = g_by_key.vals[i]; if (!o || o == im || o->pool != im->pool || o->off >= im->off + im->size || im->off >= o->off + o->size) continue;
        if (im->novl < 8 && o->novl < 8) { im->ovl[im->novl++] = o; o->ovl[o->novl++] = im; }
    }
}
static void alias_unlink(vkimg_t *im) {
    for (int i = 0; i < im->novl; i++) { vkimg_t *o = im->ovl[i]; for (int j = 0; j < o->novl; j++) if (o->ovl[j] == im) { o->ovl[j] = o->ovl[--o->novl]; break; } }
    im->novl = 0;
}
static void alias_check(vkimg_t *im) {   // im is about to be sampled
    for (int i = 0; i < im->novl; i++) {
        vkimg_t *o = im->ovl[i]; if (o->wseq <= im->wseq || o->wseq <= im->rseen) continue;
        im->rseen = o->wseq;
        static int n; if (n++ < 200) VKLOG("[alias] stale read: %ux%u fmt %#x levels %u at %+lld..%+lld sampled, but %ux%u fmt %#x levels %u at %+lld..%+lld rendered over its memory later",
            im->w, im->h, im->nvn_format, im->levels, (long long)im->off, (long long)(im->off + im->size), o->w, o->h, o->nvn_format, o->levels, (long long)o->off, (long long)(o->off + o->size));
    }
}
extern uint64_t g_tex_frame;   // presents so far, for the packaged textures' last use (vk_tex_sweep)
// Images are kept per storage key until their pool goes, so a texture the game streams into memory another texture had before gets an image of its own and the old one
// stayed: Xenoblade Chronicles 3 streams its field textures through pools that live for good, the device's GPU memory grew by about 470 MB a minute while walking (6.3 GB
// after 20 minutes) and Android's low-memory killer ended the game. A new image takes over: the images of the same pool over its memory that were not looked up in the
// last EVICT_KEEP presents go (their memory now holds another texture; one drawn again is made and filled again). Called with g_img_mu held
enum { EVICT_KEEP = 60 };
static uint64_t g_img_n, g_img_bytes, g_img_evicted;   // live images and their bytes, images evicted so far (vk_tex_sweep's line)
static void img_drop(vkimg_t *im) { vk_lrz_forget(im); if (im->lin_mem) nvn_dirty_release(im->lin_mem, im->lin_len); if (im->packaged && !im->pkg_released && im->pkg_mem) nvn_dirty_release(im->pkg_mem, im->pkg_len); g_img_n--; g_img_bytes -= im->size > 0 ? (uint64_t)im->size : 0; alias_unlink(im); vk_tex_view_drop_all(im); if (im->sub) vk_defer_free_image_sub(im->img, im->sub); else vk_defer_free_image(im->img, im->mem); free(im); }
static void evict_overlaps(uint64_t pool, int64_t off, int64_t size) {
    akey ks[64]; int n = 0;
    for (uint32_t i = 0; i < g_by_key.cap && n < 64; i++) {
        vkimg_t *o = g_by_key.vals[i]; if (!o || o->pool != pool || o->off >= off + size || off >= o->off + o->size || g_tex_frame - o->used < EVICT_KEEP) continue;
        memcpy(&ks[n++], g_by_key.keys + (size_t)i * g_by_key.klen, sizeof(akey));
    }
    for (int j = 0; j < n; j++) { vkimg_t *o = nvn_hm_get(&g_by_key, &ks[j]); if (!o) continue; nvn_hm_del(&g_by_key, &ks[j]); img_drop(o); g_img_evicted++; }
}
vkimg_t *vk_tex(const tex_t *t) {
    if (!t || !t->serial || t->magic != MAGIC('T', 'E', 'X', 0)) return NULL;
    const vkfmt_t *f = vk_fmt(t->d.format);
    if (!f) { VKRED_ONCE(14, "texture format %#x has no Vulkan counterpart here (first case %ux%u target %d)", t->d.format, t->d.width, t->d.height, t->d.target); return NULL; }
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) return NULL;   // buffer textures: texel buffer views of the pool (vk_tex_view)
    akey k; memset(&k, 0, sizeof k); k.pool = (uint64_t)(uintptr_t)t->pool; k.off = t->off; k.d = t->d;
    // flags that do not change the storage's layout are not part of the key: on the Switch two textures over the same memory that differ only in them see the
    // same texels. Xenoblade Chronicles 3 renders its shadow maps and depth-stencil buffer through textures with COMPRESSIBLE and samples them through others with
    // IMAGE added; as separate images those were never written, the shadows were total and the stencil's material tags 0, and its lit scene black. IMAGE stays
    // in the key for colour formats (it gives the image storage usage), not for depth and stencil, which cannot be storage images
    k.d.flags &= NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_SPARSE | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_MINIMAL_LAYOUT | (f->depth || f->stencil ? 0 : NVN_TEXTURE_FLAGS_IMAGE);
    int keyed = t->pool != NULL;
    pthread_mutex_lock(&g_img_mu);
    vkimg_t *im = keyed ? nvn_hm_get(&g_by_key, &k) : nvn_hm_get(&g_by_serial, &t->serial);
    int fresh = 0;
    if (!im) {
        if (keyed) evict_overlaps(k.pool, t->off, (int64_t)t->L.size);
        im = img_new(t, f); if (!im) { pthread_mutex_unlock(&g_img_mu); return NULL; }
        im->pool = k.pool; im->off = t->off; im->size = (int64_t)t->L.size; im->flags = t->d.flags; g_img_n++; g_img_bytes += t->L.size;
        if (keyed) { alias_link(im); nvn_hm_put(&g_by_key, &k, im); } else nvn_hm_put(&g_by_serial, &t->serial, im);
        fresh = 1;
    }
    im->used = g_tex_frame;
    pthread_mutex_unlock(&g_img_mu);
    if (!vk_sub_open()) return fresh ? NULL : im;   // looked up outside a submission (present): nothing can be recorded, a new image has no contents yet
    if (t->mem && lin_copy(im, t, f)) {}
    else if (t->mem && packaged_tex(t)) {
        im->pkg_used = g_tex_frame;
        if (!im->packaged) { im->packaged = 1; pkg_upload(im, t, f); }
        else {
            uint64_t now = nvn_dirty_now();
            if (now != im->pkg_seen && nvn_dirty_since_span(im->pkg_span, t->mem, t->L.size, im->pkg_stamp)) pkg_upload(im, t, f);   // pages written since the upload
            im->pkg_seen = now;
        }
    } else if (fresh) vk_tex_prepare(vk_upload_cmd(), im);
    return im;
}
// Packaged textures give up their write protection (nvn_dirty_release; drawn again, they are uploaded and protected again) when not drawn for PKG_IDLE presents, and the
// least recently drawn ones beyond that while the protected runs, each splitting its pool's mapping, are more than PKG_RUNS (textures stay protected until the game writes them).
// Textures drawn in the last PKG_KEEP presents keep theirs (released, they would be uploaded again at once). Swept every 300 presents, with the pools whose mapping the
// protection has split most mapped again in one piece (nvn_dirty_defrag: Xenoblade Chronicles 3 went from 43,000 mappings after 25 minutes of walking, toward the kernel's
// limit of 65,530, to about 17,500)
enum { PKG_IDLE = 1800, PKG_KEEP = 60, PKG_RUNS = 8000 };   // about 60 s and 2 s at 30 fps; at most about 16,000 mappings for the protection
uint64_t g_tex_frame;
typedef struct { uint64_t used; vkimg_t *im; } pkg_age;
static int pkg_age_cmp(const void *a, const void *b) { uint64_t x = ((const pkg_age *)a)->used, y = ((const pkg_age *)b)->used; return x < y ? -1 : x > y; }
void vk_tex_sweep(void) {
    if (++g_tex_frame % 300) return;
    uint64_t idle = 0, lru = 0, before = nvn_dirty_released();
    pthread_mutex_lock(&g_img_mu);
    pkg_age *v = NULL; size_t nv = 0, cap = 0;
    for (int m = 0; m < 2; m++) {
        nvn_hmap *h = m ? &g_by_serial : &g_by_key;
        for (uint32_t i = 0; i < h->cap; i++) {
            vkimg_t *im = h->vals[i];
            if (!im || !im->packaged || im->pkg_released || !im->pkg_mem) continue;
            if (g_tex_frame - im->pkg_used >= PKG_IDLE) { nvn_dirty_release(im->pkg_mem, im->pkg_len); im->pkg_released = 1; idle++; continue; }
            if (g_tex_frame - im->pkg_used < PKG_KEEP) continue;
            if (nv == cap) { cap = cap ? cap * 2 : 4096; pkg_age *nw = realloc(v, cap * sizeof *v); if (!nw) break; v = nw; }
            v[nv++] = (pkg_age){im->pkg_used, im};
        }
    }
    { int vk_pool_remap_ro(void *mem, size_t len); int nvn_dirty_defrag(int (*)(void *, size_t)); struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); int d = nvn_dirty_defrag(vk_pool_remap_ro); clock_gettime(CLOCK_MONOTONIC, &t1); static uint64_t nd;
        if (d && (nd += d) <= 8) VKLOG("dirty-page tracking: %d pool(s) mapped again in one piece in %.1f ms (%llu so far)", d, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, (unsigned long long)nd); }
    int runs = nvn_dirty_runs();
    if (runs > PKG_RUNS && nv) {   // the least recently drawn first, in batches, until the runs are well under the budget
        qsort(v, nv, sizeof *v, pkg_age_cmp);
        for (size_t i = 0; i < nv && runs > PKG_RUNS * 3 / 4; ) {
            for (size_t e = i + 256; i < nv && i < e; i++) { nvn_dirty_release(v[i].im->pkg_mem, v[i].im->pkg_len); v[i].im->pkg_released = 1; lru++; }
            runs = nvn_dirty_runs();
        }
    }
    pthread_mutex_unlock(&g_img_mu);
    free(v);
    static uint64_t total; total += idle + lru;
    if (g_tex_frame % 3000 == 0) VKLOG("images: %llu live, %.0f MB of textures, %llu evicted so far (a newer texture over their memory)", (unsigned long long)g_img_n, g_img_bytes / 1048576.0, (unsigned long long)g_img_evicted);
    if ((idle || lru) && (g_tex_frame % 3000 == 0 || lru)) VKLOG("packaged textures: %llu not drawn for %d presents and %llu least recently drawn released their write protection (%llu pages; %llu so far); %d protected runs",
                    (unsigned long long)idle, PKG_IDLE, (unsigned long long)lru, (unsigned long long)(nvn_dirty_released() - before), (unsigned long long)total, runs);
}
void vk_tex_drop_pool(const void *pool) {   // the pool is finalized: its textures' images go once the GPU is done with them
    pthread_mutex_lock(&g_img_mu);
    for (uint32_t i = 0; i < g_by_key.cap; i++) {
        if (!g_by_key.vals[i]) continue;
        akey k; memcpy(&k, g_by_key.keys + (size_t)i * g_by_key.klen, sizeof k);
        if (k.pool != (uint64_t)(uintptr_t)pool) continue;
        vkimg_t *im = g_by_key.vals[i]; nvn_hm_del(&g_by_key, &k); i = (uint32_t)-1;   // the table was rehashed by the delete: rescan
        img_drop(im);
    }
    pthread_mutex_unlock(&g_img_mu);
}
// CPU-written textures (WriteTexels / a dirty first use): packaged from now on
static nvn_hmap g_cpu_written = {.klen = sizeof(uint64_t)}; static pthread_mutex_t g_cw_mu = PTHREAD_MUTEX_INITIALIZER;
void mtl_tex_cpu_written(const tex_t *t) { if (!t->serial) return; pthread_mutex_lock(&g_cw_mu); nvn_hm_put(&g_cpu_written, &t->serial, (void *)1); pthread_mutex_unlock(&g_cw_mu); }
// Linear textures (LINEAR) in a pool are their storage on the Switch: the CPU writes the pool memory and the GPU reads it (Metal's tex_linear makes them on the pool buffer).
// Hades II rasterizes its glyphs on the CPU into a 1024x1216 R8 IMAGE|LINEAR atlas (pool CPU_UNCACHED|GPU_CACHED, no flush, no WriteTexels) and decodes its Bink movies
// into linear planes: as GPU-only images of zeros all text, the title art and the movies were blank. A Vulkan image cannot be bound to the pool's memory (a dedicated
// AHardwareBuffer import, bound to its buffer), so each upload command buffer that a submission's draws need it in copies it from the pool buffer on the GPU (lin_copy).
// The packaged path (CPU upload, re-upload on written pages) is the fallback; as the only path the page faults of the CPU's writes cost Hades II about 120 a frame and
// 2 million while loading. Not LINEAR_RENDER_TARGET: the GPU draws into those, and a copy from storage would replace what it drew
static int linear_tex(const tex_t *t) { return t->pool && (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) == NVN_TEXTURE_FLAGS_LINEAR; }
// lin_copy takes LINEAR_RENDER_TARGET textures too: Super Mario Bros. Wonder's CPU writes its sky's four 64x1 RGBA16F gradients straight into such textures (NVN flags 0x40)
// that nothing draws into, and as GPU-only images of zeros the sky drew diagonal bands of saturated colour over every level. What the GPU draws into one (gpu_w) stays, as for a
// cleared or copied linear texture, until the CPU writes its storage again. The packaged path (linear_tex) still leaves them out: a re-upload there would replace a drawing
static int linear_storage(const tex_t *t) { return t->pool && (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)); }
extern uint64_t g_up_gen;
static int lin_copy(vkimg_t *im, const tex_t *t, const vkfmt_t *f) {   // 1 = the image holds its storage as of this upload buffer
    if (!linear_storage(t) || im->levels != 1 || im->d != 1 || im->layers != 1 || f->bw > 1 || f->bh > 1 || !f->bpb || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return 0;
    size_t stride = t->d.stride ? t->d.stride : (size_t)im->w * f->bpb; VkBuffer b; VkDeviceSize o;
    if (stride % f->bpb || stride < (size_t)im->w * f->bpb || !vk_buf_at((uint64_t)(uintptr_t)t->mem, (uint64_t)stride * (im->h - 1) + (uint64_t)im->w * f->bpb, &b, &o) || o % f->bpb) return 0;
    // the GPU wrote it (a clear, a copy, a storage write): it holds that until the CPU writes its storage again, which its pages, set read-only, tell
    size_t len = stride * (im->h - 1) + (size_t)im->w * f->bpb;
    if (im->gpu_w && !im->lin_mem) { nvn_dirty_protect(t->mem, len); im->lin_stamp = nvn_dirty_now(); im->lin_span = nvn_dirty_span(t->mem, len); im->lin_mem = t->mem; im->lin_len = len; return 1; }
    if (im->gpu_w) { if (!nvn_dirty_since_span(im->lin_span, t->mem, len, im->lin_stamp)) return 1; nvn_dirty_release(im->lin_mem, im->lin_len); im->lin_mem = NULL; im->gpu_w = 0; im->lin_gen = 0; }
    VkCommandBuffer cb = vk_upload_cmd();
    if (im->lin_gen == g_up_gen) return 1;
    im->lin_gen = g_up_gen;
    vk_img_barrier_all(cb);   // after the reads of earlier submissions
    if (im->layout != VK_IMAGE_LAYOUT_GENERAL) vk_tex_prepare(cb, im);
    VkBufferImageCopy r = {o, (uint32_t)(stride / f->bpb), 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {im->w, im->h, 1}};
    vkCmdCopyBufferToImage(cb, b, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    return 1;
}
static int packaged_tex(const tex_t *t) {
    if (t->has_layout || linear_tex(t)) return 1;
    pthread_mutex_lock(&g_cw_mu); int r = nvn_hm_get(&g_cpu_written, &t->serial) != NULL; pthread_mutex_unlock(&g_cw_mu);
    return r;
}
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) { (void)t; }   // images belong to their storage key, dropped with the pool
int mtl_tex_gpu_owned(uint64_t serial) { (void)serial; return 0; }

// ---------------- views ----------------
typedef struct vview { VkImageView v; VkImageViewType type; VkFormat fmt; uint32_t lv0, nlv, ly0, nly; VkComponentMapping swz; VkImageAspectFlags aspect; int attach; struct vview *next; } vview;   // attach: an attachment view (its usage differs from a sampling view's)
static nvn_hmap g_views = {.klen = sizeof(vkimg_t *)};   // vkimg_t* → vview list
static pthread_mutex_t g_view_mu = PTHREAD_MUTEX_INITIALIZER;
extern uint64_t g_tex_gen;
static void vk_tex_view_drop_all(vkimg_t *im) {
    pthread_mutex_lock(&g_view_mu);
    vview *v = nvn_hm_get(&g_views, &im); g_tex_gen++;
    while (v) { vview *n = v->next; vk_defer_free_view(v->v); free(v); v = n; }
    nvn_hm_del(&g_views, &im);
    pthread_mutex_unlock(&g_view_mu);
}
static VkComponentSwizzle swz_comp(int nvn) {   // NVN swizzle enum: 0 zero 1 one 2 R 3 G 4 B 5 A
    switch (nvn) { case 0: return VK_COMPONENT_SWIZZLE_ZERO; case 1: return VK_COMPONENT_SWIZZLE_ONE; case 2: return VK_COMPONENT_SWIZZLE_R; case 3: return VK_COMPONENT_SWIZZLE_G; case 4: return VK_COMPONENT_SWIZZLE_B; default: return VK_COMPONENT_SWIZZLE_A; }
}
static VkComponentSwizzle apply(VkComponentSwizzle outer, const VkComponentMapping *inner, VkComponentSwizzle self) {   // outer picks a channel of what inner produces
    if (outer == VK_COMPONENT_SWIZZLE_IDENTITY) outer = self;
    switch (outer) {
    case VK_COMPONENT_SWIZZLE_R: return inner->r == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_R : inner->r;
    case VK_COMPONENT_SWIZZLE_G: return inner->g == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_G : inner->g;
    case VK_COMPONENT_SWIZZLE_B: return inner->b == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_B : inner->b;
    case VK_COMPONENT_SWIZZLE_A: return inner->a == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_A : inner->a;
    default: return outer;
    }
}
__thread VkFormat vk_view_fmt; __thread uint32_t vk_view_layers;
VkImageView vk_tex_view(const tex_t *t, const view_t *vw, int dim, int arrayed, int as_attachment, vkimg_t **img_out) {
    vkimg_t *im = vk_tex(t); if (img_out) *img_out = im;
    if (!im) return VK_NULL_HANDLE;
    if (as_attachment) { im->wseq = ++g_wseq; im->gpu_w = 1; } else if (im->novl) alias_check(im);
    vk_watch(im, as_attachment ? "attachment" : "sampled or image");
    if (!as_attachment) { void vk_texdump_note(vkimg_t *im); vk_texdump_note(im); }
    const vkfmt_t *f = vk_fmt(vw && vw->set_format ? vw->format : t->d.format); if (!f) f = vk_fmt(t->d.format);
    uint32_t lv0 = vw && vw->set_levels ? vw->base_level : 0, nlv = vw && vw->set_levels ? vw->nlevels : im->levels - lv0;
    if (lv0 >= im->levels) lv0 = im->levels - 1; if (!nlv || lv0 + nlv > im->levels) nlv = im->levels - lv0;
    // a 3D image drawn into: the view's layers are depth slices of its level (the image is 2D-array compatible). Xenoblade Chronicles 3 renders its 16x16x16 colour grading
    // LUT one slice per pass through 2D views at layers 0-15; clamped to the image's one layer, every slice went to slice 0, and the graded title sky came out flat blue
    uint32_t nlay = as_attachment && im->vtype == VK_IMAGE_VIEW_TYPE_3D ? (im->d >> lv0 ? im->d >> lv0 : 1) : im->layers;
    uint32_t ly0 = vw && vw->set_layers ? vw->min_layer : 0, nly = vw && vw->set_layers ? vw->nlayers : nlay - ly0;
    if (ly0 >= nlay) ly0 = nlay - 1; if (!nly || ly0 + nly > nlay) nly = nlay - ly0;
    int target = vw && vw->set_target ? vw->target : im->target;
    VkImageViewType type = vtype_of(target);
    // what the shader declares wins over the object's target (a 2D render target sampled as an array, a cube face read as 2D)
    if (dim != 0xff && !as_attachment) {
        VkImageViewType want = dim == 0 ? (arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) : dim == 1 ? (arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D) :
                               dim == 2 ? VK_IMAGE_VIEW_TYPE_3D : dim == 3 ? (arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) : type;
        if (want == VK_IMAGE_VIEW_TYPE_2D && type == VK_IMAGE_VIEW_TYPE_2D_ARRAY) nly = 1;
        if ((want == VK_IMAGE_VIEW_TYPE_CUBE || want == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && (im->layers < 6 || nly % 6)) want = type;   // not a cube: keep it as it is
        if (want == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
        if (want == VK_IMAGE_VIEW_TYPE_3D && im->d == 1) want = type;
        if ((want == VK_IMAGE_VIEW_TYPE_1D || want == VK_IMAGE_VIEW_TYPE_1D_ARRAY) != (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY)) want = type;
        type = want;
    }
    if (as_attachment) { type = im->layers > 1 && nly > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : (im->vtype == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D); nlv = 1; }
    if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_3D) nly = 1;
    VkImageAspectFlags aspect = im->aspect;
    if (aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) && !as_attachment)
        aspect = (!f->depth || t->stencil || (vw && vw->set_dsmode && vw->dsmode == 1)) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;   // (STENCIL8 alone may be stored combined)
    VkComponentMapping swz = f->swz;
    if (!as_attachment) {
        if (t->set_swz) { VkComponentMapping o = swz; swz.r = apply(swz_comp(t->swz & 7), &o, VK_COMPONENT_SWIZZLE_R); swz.g = apply(swz_comp(t->swz >> 3 & 7), &o, VK_COMPONENT_SWIZZLE_G);
                          swz.b = apply(swz_comp(t->swz >> 6 & 7), &o, VK_COMPONENT_SWIZZLE_B); swz.a = apply(swz_comp(t->swz >> 9 & 7), &o, VK_COMPONENT_SWIZZLE_A); }
        if (vw && vw->set_swizzle) { VkComponentMapping o = swz; swz.r = apply(swz_comp(vw->swz[0]), &o, VK_COMPONENT_SWIZZLE_R); swz.g = apply(swz_comp(vw->swz[1]), &o, VK_COMPONENT_SWIZZLE_G);
                                     swz.b = apply(swz_comp(vw->swz[2]), &o, VK_COMPONENT_SWIZZLE_B); swz.a = apply(swz_comp(vw->swz[3]), &o, VK_COMPONENT_SWIZZLE_A); }
    } else swz = (VkComponentMapping){0};
    VkFormat fmt = f->vk;
    if ((im->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || f->compressed != vk_fmt(im->nvn_format)->compressed) fmt = im->fmt;   // depth views and block-size changes keep the image's format
    if (im->fixed_fmt && fmt != im->fmt && fmt != srgb_twin(im->fmt)) { fmt = im->fmt; static int n; if (n++ < 8) VKLOG("[RED] view format %d of a format-listed image (%d, DM_VK_UBWC): the image's own used", f->vk, im->fmt); }
    pthread_mutex_lock(&g_view_mu);
    vview *head = nvn_hm_get(&g_views, &im);
    for (vview *v = head; v; v = v->next)
        if (v->type == type && v->fmt == fmt && v->lv0 == lv0 && v->nlv == nlv && v->ly0 == ly0 && v->nly == nly && v->aspect == aspect && v->attach == (as_attachment != 0) && !memcmp(&v->swz, &swz, sizeof swz)) { pthread_mutex_unlock(&g_view_mu); vk_view_fmt = fmt; vk_view_layers = nly; return v->v; }
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, 0};
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(V.pd, fmt, &fp);
    uci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (as_attachment) uci.usage = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) | (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0);
    else if ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) uci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &uci, 0, im->img, type, fmt, swz, {aspect, lv0, nlv, ly0, nly}};
    VkImageView view; VkResult r = vkCreateImageView(V.dev, &ci, NULL, &view);
    if (r) { pthread_mutex_unlock(&g_view_mu); VKRED_ONCE(15, "vkCreateImageView failed (%d): type %d format %d levels %u+%u layers %u+%u of image %ux%ux%u", r, type, fmt, lv0, nlv, ly0, nly, im->w, im->h, im->layers); return VK_NULL_HANDLE; }
    vview *nv = calloc(1, sizeof *nv); *nv = (vview){view, type, fmt, lv0, nlv, ly0, nly, swz, aspect, as_attachment != 0, head};
    nvn_hm_put(&g_views, &im, nv);
    pthread_mutex_unlock(&g_view_mu);
    vk_view_fmt = fmt; vk_view_layers = nly;
    return view;
}

// ---------------- texture / sampler ID tables ----------------
// A registration as the replay of each submission sees it. NVN's texture pool is memory the GPU reads as it runs, and a game rewrites an ID once the work
// that used it is done; with the replay on its own thread (DM_NVN_ASYNC) a submission is replayed after the game has gone on, and Xenoblade Chronicles 3
// rewrites about 90 IDs a second with other textures: a frame replayed late drew with the next frame's textures (whole-screen garbage while the camera
// turned). So each registration keeps the number of replay jobs queued when it was made, and a job sees the newest one made before it was queued
// (nvn_jobs_queued / nvn_replaying_job, gfx/nvn/hle_nvn.c); without a replay thread that is always the newest. TREG_OLD older ones per ID, kept for IDs rewritten
enum { TREG_OLD = 3 };
typedef struct { tex_t t; view_t v; uint8_t ok, has_v; uint64_t seq; } treg_ver;
typedef struct { treg_ver cur; treg_ver *old; } treg_t;   // old: TREG_OLD earlier versions, newest first (allocated at the first rewrite)
static treg_t *g_treg; static int g_ntreg;
uint64_t g_tex_gen = 1;   // changes whenever a texture ID is registered again or views go (gfx_vk_exec.c's per-submission view cache)
uint64_t g_samp_gen = 1;  // changes whenever a sampler ID is registered with other parameters (gfx_vk_exec.c samp_cached and the descriptor set memo)
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static VkSampler *g_samp; static int g_nsreg;
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
__attribute__((weak)) uint64_t nvn_jobs_queued(void) { return 0; }
__attribute__((weak)) uint64_t nvn_replaying_job(void) { return 0; }
void mtl_reg_tex(int tid, const tex_t *t, const view_t *v) {
    if (tid < 0) return;
    uint64_t seq = nvn_jobs_queued();
    pthread_mutex_lock(&g_rmu);
    if (tid >= g_ntreg) { int n = tid + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    g_tex_gen++; treg_t *r = &g_treg[tid];
    if (r->cur.ok && seq) {   // keep the one replaced for the jobs queued before this registration
        if (!r->old) r->old = calloc(TREG_OLD, sizeof *r->old);
        if (r->old) { memmove(r->old + 1, r->old, sizeof *r->old * (TREG_OLD - 1)); r->old[0] = r->cur; }
    }
    r->cur.ok = t != NULL; if (t) r->cur.t = *t; r->cur.has_v = v != NULL; if (v) r->cur.v = *v; r->cur.seq = seq;
    pthread_mutex_unlock(&g_rmu);
}
void mtl_set_tex_pool_n(int n) { (void)n; }
static __thread treg_ver t_treg;   // the version a lookup found, copied (the table may be rewritten or grow meanwhile)
vkimg_t *vk_tex_by_id(int tid, const view_t **view_out, const tex_t **tex_out) {
    uint64_t job = nvn_replaying_job();
    pthread_mutex_lock(&g_rmu);
    const treg_ver *r = tid > 0 && tid < g_ntreg ? &g_treg[tid].cur : NULL;
    if (r && job && r->seq >= job && g_treg[tid].old)   // registered after this job was queued: the newest earlier one
        for (int k = 0; k < TREG_OLD; k++) { const treg_ver *o = &g_treg[tid].old[k]; if (!o->ok) break; r = o; if (o->seq < job) break; }
    int ok = r && r->ok; if (ok) t_treg = *r;
    pthread_mutex_unlock(&g_rmu);
    if (!ok) return NULL;
    if (view_out) *view_out = t_treg.has_v ? &t_treg.v : NULL; if (tex_out) *tex_out = &t_treg.t;
    return (vkimg_t *)1;   // presence only: the caller makes the view with vk_tex_view
}
static VkSamplerAddressMode wrap(int w) {
    switch (w) { case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 2: case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
                 case 4: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; }
}
static VkCompareOp cmp_op(int f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }   // NVN NEVER..ALWAYS = 1..8, Vulkan 0..7 in the same order
static VkSampler make_sampler(const samp_par *p) {
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo rd = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, NULL, p->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : p->reduction == 2 ? VK_SAMPLER_REDUCTION_MODE_MAX : VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE};
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, p->reduction ? &rd : NULL};
    ci.magFilter = p->magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = p->minf == 1 || p->minf == 3 || p->minf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = p->minf == 4 || p->minf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap(p->ws); ci.addressModeV = wrap(p->wt); ci.addressModeW = wrap(p->wr);
    ci.mipLodBias = p->lod_bias;
    ci.anisotropyEnable = p->aniso > 1.0f; ci.maxAnisotropy = p->aniso > V.max_aniso ? V.max_aniso : p->aniso;
    ci.compareEnable = p->cmp_mode == 1; ci.compareOp = cmp_op(p->cmp_func);
    ci.minLod = p->lod_min; ci.maxLod = p->minf <= 1 ? 0.25f : p->lod_max;   // no mipmap filter: level 0 only (GL's rule)
    int border = ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (border) {
        const float *b = p->border;
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if ((b[0] || b[1] || b[2] || b[3]) && V.has_custom_border) {
            ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT; memcpy(cb.customBorderColor.float32, b, 16); cb.format = VK_FORMAT_UNDEFINED;
            cb.pNext = (void *)ci.pNext; ci.pNext = &cb;
        }
    }
    VkSampler s; if (vkCreateSampler(V.dev, &ci, NULL, &s)) return VK_NULL_HANDLE;
    return s;
}
void mtl_reg_samp(int sid, const samp_par *sp) {
    if (sid < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (sid >= g_nsreg) { int n = sid + 256; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); g_samp = realloc(g_samp, n * sizeof *g_samp);
        memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg); memset(g_samp + g_nsreg, 0, (n - g_nsreg) * sizeof *g_samp); g_nsreg = n; }
    if (!g_sreg_ok[sid] || memcmp(&g_sreg[sid], sp, sizeof *sp)) { g_sreg[sid] = *sp; g_sreg_ok[sid] = 1; g_samp[sid] = VK_NULL_HANDLE; __atomic_fetch_add(&g_samp_gen, 1, __ATOMIC_RELEASE); }   // samplers are cached by parameters below
    pthread_mutex_unlock(&g_rmu);
}
static nvn_hmap g_samp_cache = {.klen = sizeof(samp_par)};   // samp_par → VkSampler (VkSamplers are never destroyed: few distinct parameter sets)
void vk_samp_describe(int sid, char *out, size_t n) {   // the registered parameters (traces)
    pthread_mutex_lock(&g_rmu);
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) { const samp_par *p = &g_sreg[sid]; snprintf(out, n, "sampler %d: wrap %d %d %d min %d mag %d lod %g..%g bias %g aniso %g cmp %d/%d red %d border %g %g %g %g", sid, p->ws, p->wt, p->wr,
        p->minf, p->magf, p->lod_min, p->lod_max, p->lod_bias, p->aniso, p->cmp_mode, p->cmp_func, p->reduction, p->border[0], p->border[1], p->border[2], p->border[3]); }
    else snprintf(out, n, "sampler %d: not registered (dummy)", sid);
    pthread_mutex_unlock(&g_rmu);
}
VkSampler vk_samp_by_id(int sid) {
    pthread_mutex_lock(&g_rmu);
    VkSampler s = VK_NULL_HANDLE;
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) {
        s = g_samp[sid];
        if (!s) { s = nvn_hm_get(&g_samp_cache, &g_sreg[sid]); if (!s) { s = make_sampler(&g_sreg[sid]); if (s) nvn_hm_put(&g_samp_cache, &g_sreg[sid], s); } g_samp[sid] = s; }
    }
    pthread_mutex_unlock(&g_rmu);
    return s;
}
// The same sampler without filtering, for integer views: Maxwell reads integer texels unfiltered whatever the sampler says, while Vulkan forbids linear filtering of
// formats without SAMPLED_IMAGE_FILTER_LINEAR (Pokémon Legends: Arceus samples R8_UINT and R32_UINT views with trilinear samplers; the validation layer named them)
VkSampler vk_samp_by_id_nearest(int sid) {   // cached by its parameters, as the samplers themselves
    pthread_mutex_lock(&g_rmu);
    VkSampler s = VK_NULL_HANDLE;
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) {
        samp_par p = g_sreg[sid]; p.magf = 0; p.minf = p.minf == 1 ? 0 : p.minf == 3 || p.minf == 4 || p.minf == 5 ? 2 : p.minf; p.aniso = 1; p.reduction = 0;
        if (!(s = nvn_hm_get(&g_samp_cache, &p)) && (s = make_sampler(&p))) nvn_hm_put(&g_samp_cache, &p, s);
    }
    pthread_mutex_unlock(&g_rmu);
    return s;
}
VkSampler vk_dummy_sampler(void) {
    static VkSampler s; if (s) return s;
    samp_par p = {0}; p.magf = 1; p.minf = 1; p.lod_max = 1000; p.aniso = 1; s = make_sampler(&p); return s;
}
// ---------------- dummies: what an unbound slot reads (zeros) ----------------
VkImageView vk_dummy_view(int dim, int arrayed, int ms, int storage, int is_int) {
    static VkImageView cache[4][2][2][2][2]; static vkimg_t *imgs[4][2][2][2]; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    int d = dim == 0 ? 0 : dim == 2 ? 2 : dim == 3 ? 3 : 1; arrayed = !!arrayed; ms = !!ms && d == 1; storage = !!storage; is_int = !!is_int;
    pthread_mutex_lock(&mu);
    VkImageView v = cache[d][arrayed][ms][storage][is_int];
    if (!v) {
        vkimg_t **pi = &imgs[d][ms][storage][is_int];
        if (!*pi) {
            tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = 1;
            t.d.format = is_int ? 0x27 : 0x25; t.d.width = t.d.height = 1; t.d.depth = d == 3 ? 12 : 6; t.d.levels = 1; t.d.samples = ms ? 4 : 0;
            t.d.target = d == 0 ? 3 : d == 2 ? 2 : d == 3 ? 9 : ms ? 6 : 4; t.d.flags = storage ? NVN_TEXTURE_FLAGS_IMAGE : 0;
            if (d == 2) t.d.depth = 1;
            *pi = img_new(&t, vk_fmt(t.d.format));
            VkCommandBuffer cb = vk_upload_cmd(); vk_tex_prepare(cb, *pi);
            VkClearColorValue z = {{0}}; VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
            if (!ms) vkCmdClearColorImage(cb, (*pi)->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
        }
        vkimg_t *im = *pi;
        VkImageViewType type = d == 0 ? (arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) : d == 2 ? VK_IMAGE_VIEW_TYPE_3D : d == 3 ? (arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) : (arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        uint32_t nly = type == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? 12 : arrayed ? im->layers : 1;
        VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im->img, type, im->fmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, nly}};
        VKCHK(vkCreateImageView(V.dev, &ci, NULL, &v)); cache[d][arrayed][ms][storage][is_int] = v;
    }
    pthread_mutex_unlock(&mu);
    return v;
}
VkBufferView vk_dummy_tbuf(int storage) {
    static VkBufferView v[2]; static VkBuffer b; static VkDeviceMemory m; storage = !!storage;
    if (v[storage]) return v[storage];
    if (!b) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &b)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &m)); VKCHK(vkBindBufferMemory(V.dev, b, m, 0));
        void *p; VKCHK(vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &p)); memset(p, 0, 65536);
    }
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, b, VK_FORMAT_R32_UINT, 0, 65536};
    VKCHK(vkCreateBufferView(V.dev, &ci, NULL, &v[storage]));
    return v[storage];
}
// a buffer texture's texel buffer view over its pool (cached by texture contents: the registered copy)
VkBufferView vk_tbuf_view(const tex_t *t) {
    static nvn_hmap cache = {.klen = 24}; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    // keyed by the pool too: a pool finalized and another made at the same address has a new VkBuffer, and the old one's memory is freed once the GPU is past it. Keyed by address
    // alone, the view over the old buffer was handed out again and shaders read and wrote freed memory, which the kernel gives to newer buffers (Xenoblade Chronicles 3: the
    // allocator kept inside one of its memory pools had its tree bins overwritten during the opening movie, and malloc crashed at the next scene)
    vkpool_t *pl = t->mem ? vk_pool_of((uint64_t)(uintptr_t)t->mem, 1) : NULL;
    struct { uint64_t mem, pool; uint32_t fmt, n; } k = {(uint64_t)(uintptr_t)t->mem, pl ? pl->serial : 0, (uint32_t)t->d.format, t->d.width};
    pthread_mutex_lock(&mu);
    VkBufferView v = nvn_hm_get(&cache, &k);
    if (!v) {
        const vkfmt_t *f = vk_fmt(t->d.format); VkBuffer b; VkDeviceSize off;
        if (f && t->mem && vk_buf_at((uint64_t)(uintptr_t)t->mem, (uint64_t)t->d.width * f->bpb, &b, &off) && off % V.props.limits.minTexelBufferOffsetAlignment == 0) {
            VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, b, f->vk, off, (VkDeviceSize)t->d.width * f->bpb};
            if (vkCreateBufferView(V.dev, &ci, NULL, &v)) v = VK_NULL_HANDLE; else nvn_hm_put(&cache, &k, v);
        } else VKRED_ONCE(16, "buffer texture not in a pool or misaligned (format %#x, %u elements)", t->d.format, t->d.width);
    }
    pthread_mutex_unlock(&mu);
    return v;
}
// CPU readback of texels (ReadTexels): the image copied to scratch memory by a submission of its own, waited for
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    void mtl_sub_begin(void); uint64_t mtl_sub_commit(void); int mtl_wait(uint64_t, uint64_t);
    mtl_sub_begin();
    vkimg_t *im = vk_tex(t); const vkfmt_t *f = vk_fmt(t->d.format);
    size_t depth = r->depth ? r->depth : 1;
    if (!im || !f) { mtl_sub_commit(); for (size_t z = 0; z < depth; z++) for (int y = 0; y < r->height; y++) memset(dst + z * img + (size_t)y * row, 0, row); return; }
    unsigned bw = f->bw ? f->bw : 1, bh = f->bh ? f->bh : 1;
    size_t pitch = (size_t)((r->width + bw - 1) / bw) * f->bpb, rows = (r->height + bh - 1) / bh;
    VkBuffer sb; VkDeviceSize so; uint8_t *p = vk_scratch(pitch * rows * depth, 16, &sb, &so);
    VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, im); vk_img_barrier_all(cb);
    int is3d = im->d > 1;
    VkBufferImageCopy c = {so, 0, 0, {im->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, level, is3d ? 0 : layer0 + (uint32_t)r->zoffset, is3d ? 1 : (uint32_t)depth},
                           {r->xoffset, r->yoffset, is3d ? r->zoffset : 0}, {(uint32_t)r->width, (uint32_t)r->height, is3d ? (uint32_t)depth : 1}};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &c);
    mtl_wait(mtl_sub_commit(), ~0ull);
    for (size_t z = 0; z < depth; z++) for (size_t y = 0; y < rows; y++) memcpy(dst + z * img + y * row, p + (z * rows + y) * pitch, pitch < row ? pitch : row);
}
