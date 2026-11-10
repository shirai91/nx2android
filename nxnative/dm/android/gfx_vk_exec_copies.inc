// ---------------- copies and clears ----------------
static VkImageSubresourceLayers sub_of(vkimg_t *im, const view_t *v, int has_v, int z) {
    uint32_t lv = has_v && v->set_levels ? v->base_level : 0, ly = has_v && v->set_layers ? v->min_layer : 0;
    int layered = im->layers > 1;
    return (VkImageSubresourceLayers){im->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT), lv, ly + (layered ? (uint32_t)z : 0), 1};
}
static int copy_span_bytes(uint64_t row_bytes, uint64_t pitch, uint64_t slice, uint64_t rows, uint64_t slices, uint64_t *span) {
    if (!row_bytes || !pitch || !rows || !slices || pitch < row_bytes || rows - 1 > (UINT64_MAX - row_bytes) / pitch) return 0;
    uint64_t image = (rows - 1) * pitch + row_bytes, prior = slices - 1;
    if (prior && (!slice || slice < image || prior > (UINT64_MAX - image) / slice)) return 0;
    *span = prior * slice + image; return 1;
}
static int copy_buffer_span(const VkBufferImageCopy *r, const vkfmt_t *f, uint32_t slices, uint64_t *span) {
    uint64_t bw = f->bw ? f->bw : 1, bh = f->bh ? f->bh : 1, bpb = f->bpb;
    uint64_t w = r->imageExtent.width, h = r->imageExtent.height;
    uint64_t cols = (w + bw - 1) / bw, rows = (h + bh - 1) / bh;
    uint64_t row_blocks = (r->bufferRowLength ? r->bufferRowLength : w) + bw - 1;
    uint64_t image_rows = (r->bufferImageHeight ? r->bufferImageHeight : h) + bh - 1;
    row_blocks /= bw; image_rows /= bh;
    if (!bpb || !cols || !rows || row_blocks < cols || image_rows < rows || cols > UINT64_MAX / bpb || row_blocks > UINT64_MAX / bpb) return 0;
    uint64_t tight = cols * bpb, pitch = row_blocks * bpb;
    if (image_rows > UINT64_MAX / pitch) return 0;
    return copy_span_bytes(tight, pitch, pitch * image_rows, rows, slices, span);
}
static void copy_b2t(const p_copy_b2t *c, int to_texture) {
    vk_end_pass_land();
    vkimg_t *im = vk_tex(c->tex); const vkfmt_t *f = vk_fmt(c->tex->d.format);
    if (!im || !f) return;
    if (c->r.width <= 0 || c->r.height < 0 || c->r.depth < 0) { VKRED_ONCE(72, "buffer ↔ texture copy with an invalid extent: skipped"); return; }
    unsigned bw = f->bw ? f->bw : 1, bh = f->bh ? f->bh : 1;
    uint32_t w = (uint32_t)c->r.width, h = (uint32_t)(c->r.height ? c->r.height : 1), depth = (uint32_t)(c->r.depth ? c->r.depth : 1);
    uint64_t rowlen64 = 0, imgh64 = 0;
    if (S.row_stride > 0 && f->bpb) { uint64_t blocks = (uint64_t)S.row_stride / f->bpb; if (blocks > UINT64_MAX / bw) { VKRED_ONCE(73, "buffer ↔ texture copy stride exceeds Vulkan's range: skipped"); return; } rowlen64 = blocks * bw; }
    if (S.image_stride > 0 && S.row_stride > 0) { uint64_t images = (uint64_t)S.image_stride / (uint64_t)S.row_stride; if (images > UINT64_MAX / bh) { VKRED_ONCE(73, "buffer ↔ texture copy stride exceeds Vulkan's range: skipped"); return; } imgh64 = images * bh; }
    if (rowlen64 > UINT32_MAX || imgh64 > UINT32_MAX) { VKRED_ONCE(73, "buffer ↔ texture copy stride exceeds Vulkan's range: skipped"); return; }
    uint32_t rowlen = (uint32_t)rowlen64, imgh = (uint32_t)imgh64;
    int layered = im->layers > 1;
    VkBufferImageCopy r = {0, rowlen, imgh, sub_of(im, &c->view, c->has_view, c->r.zoffset), {c->r.xoffset, c->r.yoffset, layered ? 0 : c->r.zoffset},
                           {w, h, layered ? 1 : depth}};
    if (layered) r.imageSubresource.layerCount = depth;
    VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, im); vk_img_barrier_all(cb);
    uint64_t span;
    if (!copy_buffer_span(&r, f, depth, &span) || span > UINT64_MAX - c->src) { VKRED_ONCE(74, "buffer ↔ texture copy extent overflows: skipped"); return; }
    int decode = f->dec && to_texture;
    if (f->dec && !to_texture) { VKRED_ONCE(71, "copy of a BC texture decoded on the CPU into a buffer: skipped"); return; }
    VkBuffer b = VK_NULL_HANDLE; VkDeviceSize o = 0;
    if (decode) {   // BC formats the device lacks: stage through sparse-aware CPU reads before decoding
        uint64_t cols = ((uint64_t)w + bw - 1) / bw, rows = ((uint64_t)h + bh - 1) / bh;
        if (!f->bpb || cols > UINT64_MAX / f->bpb) { VKRED_ONCE(74, "BC buffer ↔ texture copy extent overflows: skipped"); return; }
        uint64_t tight = cols * f->bpb, pitch = S.row_stride > 0 ? (uint64_t)S.row_stride : tight, slice;
        if (S.image_stride > 0) slice = (uint64_t)S.image_stride;
        else { if (rows > UINT64_MAX / pitch) { VKRED_ONCE(74, "BC buffer ↔ texture copy extent overflows: skipped"); return; } slice = pitch * rows; }
        if (!copy_span_bytes(tight, pitch, slice, rows, depth, &span) || span > UINT64_MAX - c->src) { VKRED_ONCE(74, "BC buffer ↔ texture copy extent overflows: skipped"); return; }
    }
    // The lookup extent: a sparse source must map contiguously, so it is checked (and translated) over its whole span; a regular pool keeps main's
    // 1-byte lookup, so a copy whose span runs past the pool is logged by the block below and still made, as main decides
    uint64_t lookup = nvn_sparse_address_pool(c->src, 1) ? span : 1;
    if (!vk_buf_at(c->src, lookup, &b, &o)) { VKRED_ONCE(70, "buffer of a buffer ↔ texture copy outside the memory pools"); return; }
    {   // a transfer past the image's level or past the buffer is not covered by any robustness feature: Adreno loses the device (checked before the copy and logged; the copy is still made)
        uint32_t lv = r.imageSubresource.mipLevel, lw = im->w >> lv ? im->w >> lv : 1, lh = im->h >> lv ? im->h >> lv : 1, ld = im->d > 1 ? (im->d >> lv ? im->d >> lv : 1) : 1;
        lw = (lw + bw - 1) / bw * bw; lh = (lh + bh - 1) / bh * bh;
        int bad_img = lv >= im->levels || r.imageOffset.x < 0 || r.imageOffset.y < 0 || r.imageOffset.z < 0 || (uint32_t)r.imageOffset.x + r.imageExtent.width > lw ||
                      (uint32_t)r.imageOffset.y + r.imageExtent.height > lh || (uint32_t)r.imageOffset.z + r.imageExtent.depth > ld ||
                      r.imageSubresource.baseArrayLayer + r.imageSubresource.layerCount > (im->layers ? im->layers : 1);
        uint64_t need = 0; int bad_buf = 0;
        if (!f->dec && f->bpb) {
            uint64_t bx = (r.imageExtent.width + bw - 1) / bw, by = (r.imageExtent.height + bh - 1) / bh, row = (uint64_t)((rowlen ? rowlen : r.imageExtent.width) + bw - 1) / bw * f->bpb;
            uint64_t slice = (uint64_t)(((imgh ? imgh : r.imageExtent.height) + bh - 1) / bh) * row, nz = layered ? r.imageSubresource.layerCount : r.imageExtent.depth;
            need = (nz ? nz - 1 : 0) * slice + (by ? by - 1 : 0) * row + bx * f->bpb;
            VkBuffer b2; VkDeviceSize o2; bad_buf = need && !vk_buf_at(c->src, need, &b2, &o2);
        }
        if (bad_img || bad_buf) {
            static unsigned nbad; if (nbad++ < 16) VKLOG("[RED] buffer %s texture copy runs %s (logged, still made: no game seen doing it yet); region %d,%d,%d %ux%ux%u level %u layers %u+%u, image %ux%ux%u layers %u levels %u (nvn format %#x), buffer %#llx +%llu bytes",
                to_texture ? "→" : "←", bad_img ? "past the image" : "past the memory pool", r.imageOffset.x, r.imageOffset.y, r.imageOffset.z, r.imageExtent.width, r.imageExtent.height, r.imageExtent.depth, lv,
                r.imageSubresource.baseArrayLayer, r.imageSubresource.layerCount, im->w, im->h, im->d, im->layers, im->levels, (unsigned)c->tex->d.format, (unsigned long long)c->src, (unsigned long long)need);
        }
    }
    if (decode) {
        uint64_t pixels = (uint64_t)w * h;
        uint64_t out64 = pixels * f->dbpp;
        if (!f->dbpp || out64 > SIZE_MAX || depth > SIZE_MAX / out64 || span > SIZE_MAX) { VKRED_ONCE(74, "BC buffer ↔ texture copy is too large to decode: skipped"); return; }
        size_t out = (size_t)out64, pitch = S.row_stride > 0 ? (size_t)S.row_stride : (size_t)(((uint64_t)w + bw - 1) / bw) * f->bpb;
        size_t slice = S.image_stride > 0 ? (size_t)S.image_stride : pitch * (((size_t)h + bh - 1) / bh);
        uint8_t *raw = malloc((size_t)span); if (!raw) { VKRED_ONCE(75, "BC buffer ↔ texture copy could not allocate a CPU snapshot: skipped"); return; }
        vk_ubd_read(c->src, span, raw);
        VkBuffer sb; VkDeviceSize so; uint8_t *p = vk_scratch(out * depth, 16, &sb, &so);
        for (uint32_t z = 0; z < depth; z++) vk_bc_decode(f, raw + (size_t)z * slice, pitch, w, h, 1, p + (size_t)z * out);
        free(raw); b = sb; r.bufferOffset = so; r.bufferRowLength = 0; r.bufferImageHeight = 0;
    } else r.bufferOffset = o;
    if (g_watch_off >= 0) { char w[96]; snprintf(w, sizeof w, "buffer copy %s (buffer %#llx, %dx%d)", to_texture ? "destination" : "source", (unsigned long long)c->src, c->r.width, c->r.height); vk_watch(im, w); }
    if (to_texture) { vkCmdCopyBufferToImage(cb, b, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r); im->gpu_w = 1; }
    else { vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
        void vk_gpu_wrote(uint64_t a, uint64_t n); vk_gpu_wrote(c->src, span); vk_readback_unknown(); }
    vk_img_barrier_all(cb);
}
// Scaled copies of depth and stencil: Adreno blits no depth-stencil format (Pokémon Legends: Arceus copies its 1920x1080 D24S8 depth to a 480x270 one, which stayed unwritten), so each
// aspect goes image → buffer, through dm/android/shaders/depthscale.comp (the nearest texel), buffer → image. Two device buffers kept from copy to copy, grown when a region needs more
#include "depthscale_comp.spv.h"
static struct { VkPipeline p; VkPipelineLayout pl; VkDescriptorSetLayout dl; VkDescriptorPool dp; VkBuffer b[2]; VkDeviceMemory m[2]; VkDeviceSize n[2]; VkDescriptorSet set; int bad; } g_dsc;
static int dsc_init(void) {
    if (g_dsc.p || g_dsc.bad) return !g_dsc.bad;
    VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, lb};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 64, 1, &ps};
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof depthscale_comp_spv, depthscale_comp_spv}; VkShaderModule sm = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_dsc.dl) || vkCreateDescriptorPool(V.dev, &pi, NULL, &g_dsc.dp) || vkCreateShaderModule(V.dev, &mi, NULL, &sm)) { g_dsc.bad = 1; return 0; }
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_dsc.dl, 1, &pr};
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL}};
    if (vkCreatePipelineLayout(V.dev, &pli, NULL, &g_dsc.pl) || (ci.layout = g_dsc.pl, vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_dsc.p))) g_dsc.bad = 1;
    vkDestroyShaderModule(V.dev, sm, NULL);
    if (g_dsc.bad) VKLOG("[RED] the scaled depth copy's compute pipeline could not be made");
    return !g_dsc.bad;
}
static int dsc_buf(int i, VkDeviceSize n) {   // buffer i holds at least n bytes
    if (g_dsc.n[i] >= n) return 1;
    if (g_dsc.b[i]) vk_defer_free_buffer(g_dsc.b[i], g_dsc.m[i]);
    g_dsc.b[i] = VK_NULL_HANDLE; g_dsc.n[i] = 0; g_dsc.set = VK_NULL_HANDLE; n = n < (1u << 20) ? 1u << 20 : n + n / 2;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(V.dev, &bi, NULL, &g_dsc.b[i])) return 0;
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, g_dsc.b[i], &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (vkAllocateMemory(V.dev, &ai, NULL, &g_dsc.m[i])) { vkDestroyBuffer(V.dev, g_dsc.b[i], NULL); g_dsc.b[i] = VK_NULL_HANDLE; return 0; }
    vkBindBufferMemory(V.dev, g_dsc.b[i], g_dsc.m[i], 0); g_dsc.n[i] = n;
    return 1;
}
static int depth_scale(VkCommandBuffer cb, vkimg_t *s, vkimg_t *d, VkImageSubresourceLayers ss, VkImageSubresourceLayers ds, const p_copy_t2t *k) {   // 1: copied
    if (s->fmt != d->fmt || !(s->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) || s->aspect != d->aspect || !dsc_init()) return 0;
    uint32_t sw = (uint32_t)k->sr.width, sh = (uint32_t)k->sr.height, dw = (uint32_t)k->dr.width, dh = (uint32_t)k->dr.height;
    for (int a = 0; a < 2; a++) {
        VkImageAspectFlags as = a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT; if (!(s->aspect & as)) continue;
        uint32_t bytes = a ? 1 : s->fmt == VK_FORMAT_D16_UNORM ? 0 : 4; if (!bytes) return 0;   // (16-bit depth: two texels a word, not handled)
        if (!dsc_buf(0, ((VkDeviceSize)sw * sh * bytes + 3) & ~3ull) || !dsc_buf(1, ((VkDeviceSize)dw * dh * bytes + 3) & ~3ull)) return 0;
        if (!g_dsc.set) {
            VkDescriptorSetAllocateInfo sa = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dsc.dp, 1, &g_dsc.dl};
            if (vkAllocateDescriptorSets(V.dev, &sa, &g_dsc.set)) { g_dsc.set = VK_NULL_HANDLE; VKRED_ONCE(76, "scaled depth copy: no descriptor set left"); return 0; }
            VkDescriptorBufferInfo bi[2] = {{g_dsc.b[0], 0, VK_WHOLE_SIZE}, {g_dsc.b[1], 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_dsc.set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[0], NULL},
                                         {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_dsc.set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[1], NULL}};
            vkUpdateDescriptorSets(V.dev, 2, w, 0, NULL);
        }
        VkImageSubresourceLayers sl = ss, dl = ds; sl.aspectMask = as; dl.aspectMask = as;
        VkBufferImageCopy r = {0, 0, 0, sl, {k->sr.xoffset, k->sr.yoffset, 0}, {sw, sh, 1}};
        vkCmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, g_dsc.b[0], 1, &r); vk_img_barrier_all(cb);
        uint32_t pc[5] = {sw, sh, dw, dh, bytes}, n = bytes == 4 ? dw * dh : (dw * dh + 3) / 4, groups = (n + 63) / 64, gx = groups < 65535 ? groups : 65535, gy = (groups + gx - 1) / gx;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_dsc.p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_dsc.pl, 0, 1, &g_dsc.set, 0, NULL);
        vkCmdPushConstants(cb, g_dsc.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cb, gx, gy, 1); vk_img_barrier_all(cb);
        VkBufferImageCopy w = {0, 0, 0, dl, {k->dr.xoffset, k->dr.yoffset, 0}, {dw, dh, 1}};
        vkCmdCopyBufferToImage(cb, g_dsc.b[1], d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &w); vk_img_barrier_all(cb);
    }
    S.bound_pipe = VK_NULL_HANDLE;   // the compute pipeline took the bind point the replay's dispatches use
    return 1;
}
enum { CF_MIRROR_X = 4, CF_MIRROR_Y = 8 };   // NVNcopyFlags (copy_t2t)
// Transfer commands interpret image formats, not views. Family images are UNORM,
// so an sRGB resolve/blit needs a temporary image in the texture's logical format.
// Copying bytes into/out of it preserves encoding; retire it with this submission.
static VkImage transfer_image(VkCommandBuffer cb, VkFormat fmt, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage im; VkResult r = vkCreateImage(V.dev, &ci, NULL, &im);
    if (r == VK_ERROR_DEVICE_LOST) VKCHK(r);
    if (r) { VKRED_ONCE(108, "transfer image creation failed (%d): %ux%u format %d, copy skipped", r, w, h, fmt); return VK_NULL_HANDLE; }
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(V.dev, im, &rq);
    VkDeviceSize off; void *sub;
    VkDeviceMemory mem = vk_image_memory(&rq, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT), &off, &sub);
    if (!mem) { VKRED_ONCE(109, "transfer image allocation failed (%llu bytes): copy skipped", (unsigned long long)rq.size); vkDestroyImage(V.dev, im, NULL); return VK_NULL_HANDLE; }
    r = vkBindImageMemory(V.dev, im, mem, off);
    if (sub) vk_defer_free_image_sub(im, sub); else vk_defer_free_image(im, mem);
    if (r == VK_ERROR_DEVICE_LOST) VKCHK(r);
    if (r) { VKRED_ONCE(110, "transfer image memory binding failed (%d): copy skipped", r); return VK_NULL_HANDLE; }
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    return im;
}
// A texture copy into a LINEAR texture in CPU-visible memory: on the Switch the copy writes the texture's storage, which the CPU then reads. Here it writes the Vulkan
// image only, so the copied region is also copied back to the storage at the texture's stride, marked as written by the GPU (the pools' copy-back carries it to the CPU).
// Pokémon Violet captures its scene for the enrollment paper's portrait this way (1920x1080 RGBA8_SRGB into a 1920x1080 RGBA8 LINEAR texture in a CPU_UNCACHED pool,
// stride 7680, a 1440x864 region) and encodes the picture to BC1 on the CPU from that storage; without the copy-back it read zeros and the portrait was black.
// bpb is the image's own texel size only for an uncompressed format the device has (f->dec == 0: a CPU-decoded BC format's image texel is dbpp bytes).
// DM_VK_LIN_COPY_BACK=1 (start-up, a profile env row): on. Off by default
static void t2t_lin_back(VkCommandBuffer cb, vkimg_t *d, const tex_t *t, const vkfmt_t *f, const p_copy_t2t *c) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_LIN_COPY_BACK"); on = e && atoi(e) != 0; }
    if (!on || !t->mem || !t->pool || !(t->d.flags & NVN_TEXTURE_FLAGS_LINEAR) || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) return;
    if (f->dec || !f->bpb || f->bw > 1 || f->bh > 1 || d->levels != 1 || d->layers != 1 || d->d != 1 || d->aspect != VK_IMAGE_ASPECT_COLOR_BIT || d->samples > 1) return;
    if (c->dr.xoffset < 0 || c->dr.yoffset < 0 || c->dr.width <= 0 || c->dr.height <= 0) return;
    size_t stride = t->d.stride ? t->d.stride : (size_t)d->w * f->bpb;
    if (stride % f->bpb || stride < (size_t)d->w * f->bpb) return;
    uint64_t start = (uint64_t)(uintptr_t)t->mem + (uint64_t)c->dr.yoffset * stride + (uint64_t)c->dr.xoffset * f->bpb;
    size_t len = stride * (size_t)(c->dr.height - 1) + (size_t)c->dr.width * f->bpb; VkBuffer b; VkDeviceSize o;
    if (!vk_buf_at(start, len, &b, &o) || o % f->bpb) return;
    vk_img_barrier_all(cb);
    VkBufferImageCopy r = {o, (uint32_t)(stride / f->bpb), 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {c->dr.xoffset, c->dr.yoffset, 0}, {(uint32_t)c->dr.width, (uint32_t)c->dr.height, 1}};
    vkCmdCopyImageToBuffer(cb, d->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    void vk_gpu_wrote(uint64_t a, uint64_t n); vk_gpu_wrote(start, len); vk_readback_unknown();
    static int n; if (n++ < 4) VKLOG("texture copy into a linear %ux%u texture (nvn format %#x, stride %zu): region %dx%d copied back to its storage %p (DM_VK_LIN_COPY_BACK)", d->w, d->h, t->d.format, stride, c->dr.width, c->dr.height, (void *)t->mem);
}
static void copy_t2t(const p_copy_t2t *c) {
    vkimg_t *s, *d;
    if (g_defer_clear && S.pass_open == 2 && P_cleared) {
        s = vk_tex(c->src); d = vk_tex(c->dst);
        int keep_pass = 0;
        if (s && d) {
            int s_att = -1, d_att = -1;
            for (int i = 0; i < 9; i++) {
                if (P_att[i].im == s) s_att = i;
                if (P_att[i].im == d) d_att = i;
            }
            if (s_att < 0 && d_att < 0) {
                // Neither source nor destination is in the deferred clear pass; run copy outside it without committing.
                keep_pass = 1;
            } else if (s_att == 8 && d_att < 0 && P_da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
                (!(d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) || (P_sa.imageView && P_sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)) &&
                !(c->flags & (CF_MIRROR_X | CF_MIRROR_Y)) && s->aspect == d->aspect &&
                (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
                S.pass_w == s->w && S.pass_h == s->h &&
                s->levels == 1 && d->levels == 1 && s->layers == 1 && d->layers == 1 &&
                s->d <= 1 && d->d <= 1 && c->sr.depth <= 1 && c->dr.depth <= 1 &&
                c->sr.xoffset == 0 && c->sr.yoffset == 0 && c->sr.width == (int32_t)s->w && c->sr.height == (int32_t)s->h &&
                c->dr.xoffset == 0 && c->dr.yoffset == 0 && c->dr.width == (int32_t)d->w && c->dr.height == (int32_t)d->h) {
                VkImageSubresourceLayers ss = sub_of(s, &c->sv, c->has_sv, c->sr.zoffset), ds = sub_of(d, &c->dv, c->has_dv, c->dr.zoffset);
                if (ss.mipLevel == 0 && ss.baseArrayLayer == 0 && ds.mipLevel == 0 && ds.baseArrayLayer == 0) {
                    VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, d); vk_img_barrier_all(cb); d->gpu_w = 1;
                    VkImageSubresourceRange rg = {d->aspect, 0, 1, 0, 1};
                    VkClearDepthStencilValue v = P_da.clearValue.depthStencil;
                    if (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) v.stencil = P_sa.clearValue.depthStencil.stencil;
                    vkCmdClearDepthStencilImage(cb, d->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rg);
                    vk_img_barrier_all(cb);
                    return;
                }
            }
        }
        if (!keep_pass) vk_end_pass();
    } else {
        vk_end_pass();
        s = vk_tex(c->src); d = vk_tex(c->dst);
    }
    if (!s || !d) return;
    const vkfmt_t *sf = vk_fmt(c->src->d.format), *df = vk_fmt(c->dst->d.format);
    if (!sf || !df) return;
    vk_watch(s, "copy source"); vk_watch(d, "copy destination");
    VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, s); vk_tex_prepare(cb, d); vk_img_barrier_all(cb);
    VkImageSubresourceLayers ss = sub_of(s, &c->sv, c->has_sv, c->sr.zoffset), ds = sub_of(d, &c->dv, c->has_dv, c->dr.zoffset);
    // Vulkan refuses (vkEndCommandBuffer fails for the whole submission) a region outside either image or a blit the format cannot do: regions are clipped
    // to both images' levels, a copy with nothing left is skipped
    int32_t sw = (int32_t)(s->w >> ss.mipLevel ? s->w >> ss.mipLevel : 1), sh = (int32_t)(s->h >> ss.mipLevel ? s->h >> ss.mipLevel : 1);
    int32_t dw = (int32_t)(d->w >> ds.mipLevel ? d->w >> ds.mipLevel : 1), dh = (int32_t)(d->h >> ds.mipLevel ? d->h >> ds.mipLevel : 1);
    p_copy_t2t k = *c; c = &k; d->gpu_w = 1;
    // NVNcopyFlags: LINEAR_FILTER 1, ENGINE_2D 2 (nothing to do here), MIRROR_X 4, MIRROR_Y 8, MIRROR_Z 0x10: a mirrored copy is a blit with the destination's offsets
    // swapped. Pokémon Sword 1.3.2 captures its picture for a door's fade with MIRROR_Y (its lower-left origin image turned over) and draws it back; copied
    // unmirrored, the fade showed the room upside down for 7 frames (Odin 2, 2026-11-13)
    int mir = (int)(k.flags & (CF_MIRROR_X | CF_MIRROR_Y));
    if (k.flags & ~(DM_COPY_RESOLVE | 1u | 2u | CF_MIRROR_X | CF_MIRROR_Y)) VKRED_ONCE(77, "texture copy flags %#x: mirror Z (or an unknown bit) not done", k.flags & ~DM_COPY_RESOLVE);
    if (k.sr.height <= 0) k.sr.height = 1; if (k.dr.height <= 0) k.dr.height = 1;
    if (ss.mipLevel >= s->levels || ds.mipLevel >= d->levels || k.sr.xoffset < 0 || k.sr.yoffset < 0 || k.dr.xoffset < 0 || k.dr.yoffset < 0 || k.sr.xoffset >= sw || k.sr.yoffset >= sh || k.dr.xoffset >= dw || k.dr.yoffset >= dh) {
        VKRED_ONCE(72, "texture copy outside its images (src %dx%d @%d,%d of %dx%d level %u, dst %dx%d @%d,%d of %dx%d level %u): skipped", c->sr.width, c->sr.height, c->sr.xoffset, c->sr.yoffset, sw, sh, ss.mipLevel,
                   c->dr.width, c->dr.height, c->dr.xoffset, c->dr.yoffset, dw, dh, ds.mipLevel); vk_img_barrier_all(cb); return; }
    int same = k.sr.width == k.dr.width && k.sr.height == k.dr.height;
    if (k.sr.xoffset + k.sr.width > sw) { VKRED_ONCE(73, "texture copy source region clipped to its image"); k.sr.width = sw - k.sr.xoffset; if (same) k.dr.width = k.sr.width; }
    if (k.sr.yoffset + k.sr.height > sh) { VKRED_ONCE(73, "texture copy source region clipped to its image"); k.sr.height = sh - k.sr.yoffset; if (same) k.dr.height = k.sr.height; }
    if (k.dr.xoffset + k.dr.width > dw) { VKRED_ONCE(74, "texture copy destination region clipped to its image"); k.dr.width = dw - k.dr.xoffset; if (same) k.sr.width = k.dr.width; }
    if (k.dr.yoffset + k.dr.height > dh) { VKRED_ONCE(74, "texture copy destination region clipped to its image"); k.dr.height = dh - k.dr.yoffset; if (same) k.sr.height = k.dr.height; }
    if (k.sr.width <= 0 || k.sr.height <= 0 || k.dr.width <= 0 || k.dr.height <= 0) { vk_img_barrier_all(cb); return; }
    if (!(c->flags & DM_COPY_RESOLVE) && (mir || !(k.sr.width == k.dr.width && k.sr.height == k.dr.height && sf->vk == df->vk))) {   // a blit: both formats must allow it (and linear filtering)
        VkFormatProperties sp, dp; vkGetPhysicalDeviceFormatProperties(V.pd, sf->vk, &sp); vkGetPhysicalDeviceFormatProperties(V.pd, df->vk, &dp);
        if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(dp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) || (s->aspect != d->aspect)) {
            if (depth_scale(cb, s, d, ss, ds, &k)) return;
            VKRED_ONCE(75, "texture blit between formats %d → %d that cannot be blitted (%dx%d → %dx%d, present %llu): skipped", s->fmt, d->fmt, k.sr.width, k.sr.height, k.dr.width, k.dr.height, (unsigned long long)g_tex_frame); vk_img_barrier_all(cb); return; }
        if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) || (s->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) k.flags &= ~1u;   // nearest where linear is not allowed
    }
    if (!mir && s == d && sf->vk == df->vk && !memcmp(&ss, &ds, sizeof ss) && k.sr.xoffset == k.dr.xoffset && k.sr.yoffset == k.dr.yoffset && k.sr.width == k.dr.width && k.sr.height == k.dr.height && k.sr.zoffset == k.dr.zoffset) {
        vk_img_barrier_all(cb); return; }   // a copy of a region onto itself (two textures over the same storage): nothing to do, and Vulkan forbids overlapping copies
    if (c->flags & DM_COPY_RESOLVE) {
        VkImageResolve r = {ss, {c->sr.xoffset, c->sr.yoffset, 0}, ds, {c->dr.xoffset, c->dr.yoffset, 0}, {(uint32_t)c->sr.width, (uint32_t)c->sr.height, 1}};
        if (s->fmt == d->fmt) vkCmdResolveImage(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        else {   // MSAA sRGB is not a family image; its single-sample target can be UNORM.
            if (s->aspect != VK_IMAGE_ASPECT_COLOR_BIT || d->aspect != VK_IMAGE_ASPECT_COLOR_BIT || s->samples <= 1 || d->samples != 1 ||
                sf->vk != s->fmt || sf->vk != df->vk ||
                !((s->fmt == VK_FORMAT_R8G8B8A8_SRGB && d->fmt == VK_FORMAT_R8G8B8A8_UNORM) ||
                  (s->fmt == VK_FORMAT_B8G8R8A8_SRGB && d->fmt == VK_FORMAT_B8G8R8A8_UNORM))) {
                VKRED_ONCE(107, "texture resolve between incompatible image formats %d -> %d (texture formats %d -> %d): skipped", s->fmt, d->fmt, sf->vk, df->vk);
                vk_img_barrier_all(cb); return;
            }
            VkImage tmp = transfer_image(cb, s->fmt, r.extent.width, r.extent.height); if (!tmp) return;
            VkImageCopy cp = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, ds, r.dstOffset, r.extent};
            r.dstSubresource = cp.srcSubresource; r.dstOffset = cp.srcOffset;
            vkCmdResolveImage(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &r); vk_img_barrier_all(cb);
            vkCmdCopyImage(cb, tmp, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        }
    } else if (!mir && c->sr.width == c->dr.width && c->sr.height == c->dr.height && sf->vk == df->vk) {
        VkImageCopy r = {ss, {c->sr.xoffset, c->sr.yoffset, s->layers > 1 ? 0 : c->sr.zoffset}, ds, {c->dr.xoffset, c->dr.yoffset, d->layers > 1 ? 0 : c->dr.zoffset},
                         {(uint32_t)c->sr.width, (uint32_t)(c->sr.height ? c->sr.height : 1), s->layers > 1 ? 1 : (uint32_t)(c->sr.depth ? c->sr.depth : 1)}};
        vkCmdCopyImage(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
        VkImageBlit r = {ss, {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + (c->sr.height ? c->sr.height : 1), 1}},
                         ds, {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + (c->dr.height ? c->dr.height : 1), 1}}};
        VkImage si = s->img, di = d->img;
        VkImageCopy back = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, ds, r.dstOffsets[0], {(uint32_t)c->dr.width, (uint32_t)c->dr.height, 1}};
        if (s->fmt != sf->vk) {
            si = transfer_image(cb, sf->vk, (uint32_t)c->sr.width, (uint32_t)c->sr.height); if (!si) return;
            VkImageCopy cp = {ss, r.srcOffsets[0], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)c->sr.width, (uint32_t)c->sr.height, 1}};
            vkCmdCopyImage(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, si, VK_IMAGE_LAYOUT_GENERAL, 1, &cp); vk_img_barrier_all(cb);
            r.srcSubresource = cp.dstSubresource; r.srcOffsets[0] = cp.dstOffset; r.srcOffsets[1] = (VkOffset3D){c->sr.width, c->sr.height, 1};
        }
        if (d->fmt != df->vk) {
            di = transfer_image(cb, df->vk, (uint32_t)c->dr.width, (uint32_t)c->dr.height); if (!di) return;
            r.dstSubresource = back.srcSubresource; r.dstOffsets[0] = back.srcOffset; r.dstOffsets[1] = (VkOffset3D){c->dr.width, c->dr.height, 1};
        }
        if (mir & CF_MIRROR_X) { int32_t x = r.dstOffsets[0].x; r.dstOffsets[0].x = r.dstOffsets[1].x; r.dstOffsets[1].x = x; }
        if (mir & CF_MIRROR_Y) { int32_t y = r.dstOffsets[0].y; r.dstOffsets[0].y = r.dstOffsets[1].y; r.dstOffsets[1].y = y; }
        vkCmdBlitImage(cb, si, VK_IMAGE_LAYOUT_GENERAL, di, VK_IMAGE_LAYOUT_GENERAL, 1, &r, (c->flags & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        if (di != d->img) { vk_img_barrier_all(cb); vkCmdCopyImage(cb, di, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &back); }
    }
    t2t_lin_back(cb, d, k.dst, df, &k);
    vk_img_barrier_all(cb);
}
// ClearTexture has its own coordinates, unrelated to the bound render targets or
// the draw scissor/origin. A whole-image Vulkan clear cannot express a rectangle
// or channel mask: the synthetic mip/layer test used to erase its guard pixels.
// Adapted from nxnative's Vulkan region-clear path (80bee884), using this backend's
// view cache, masked-clear pipelines and submission retirement.
static int clear_tex_slice(vkimg_t *im, VkImageView view, VkFormat format, VkExtent2D extent, VkRect2D rect,
                           unsigned mask, unsigned type, const VkClearColorValue *value) {
    mckey key = {.ncol=1, .cfmt={(uint32_t)format}, .samples=im->samples, .mask=mask, .type=type};
    VkPipeline pipe = mc_pipeline_key(&key); if (!pipe) return 0;
    VkCommandBuffer cb = vk_cmd();
    if (rp_on()) {
        VkRenderPass rp = rp_compat(1, key.cfmt, 0, key.samples); if (!rp) return 0;
        VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, extent.width, extent.height, 1};
        VkFramebuffer fb;
        if (vkCreateFramebuffer(V.dev, &fi, NULL, &fb)) return 0;
        vk_defer_free_framebuffer(fb);   // the view/image may retire after this command buffer, never before it
        VkRenderPassBeginInfo ri = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0,0},extent}, 0, NULL};
        vkCmdBeginRenderPass(cb, &ri, VK_SUBPASS_CONTENTS_INLINE);
    } else {
        VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view, VK_IMAGE_LAYOUT_GENERAL,
            VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {0}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0,0},extent}, 1, 0, 1, &a, NULL, NULL};
        vkCmdBeginRendering(cb, &ri);
    }
    VkViewport vp = {0,0,(float)extent.width,(float)extent.height,0,1};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &rect);
    vkCmdPushConstants(cb, g_mc.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, value);
    vkCmdDraw(cb, 3, 1, 0, 0);
    if (rp_on()) vkCmdEndRenderPass(cb); else vkCmdEndRendering(cb);
    S.bound_pipe = VK_NULL_HANDLE; g_bc_stale = 1;
    vk_img_barrier_all(cb);
    return 1;
}
static VkClearColorValue clear_tex_value(const p_clear_tex *c, unsigned type) {
    VkClearColorValue value;
    for (int i=0;i<4;i++) {
        double v = c->c.f[i];
        if (c->is_uint && type) value.uint32[i] = c->c.u[i];
        else if (!type) value.float32[i] = c->is_uint ? (float)c->c.u[i] : c->c.f[i];
        else if (type == 1) value.uint32[i] = !(v > 0) ? 0 : v >= UINT32_MAX ? UINT32_MAX : (uint32_t)v;
        else value.int32[i] = v != v ? 0 : v <= INT32_MIN ? INT32_MIN : v >= INT32_MAX ? INT32_MAX : (int32_t)v;
    }
    return value;
}
// A compressed VkFormat cannot be cleared by vkCmdClearColorImage. Upload valid
// constant-color blocks instead, retaining the compatibility whole-subresource
// result. One immutable scratch chunk is reused until this submission completes.
static int clear_tex_compressed(vkimg_t *im, const vkfmt_t *f, const p_clear_tex *c,
                                uint32_t lv, uint32_t first, uint32_t count, uint32_t w, uint32_t h) {
    if (im->samples != 1 || !(im->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !f->bw || !f->bh) {
        VKRED_ONCE(71, "compressed texture clear has invalid transfer parameters"); return 0;
    }
    VkClearColorValue color = clear_tex_value(c, 0); uint8_t block[16];
    int bytes = vk_compressed_clear_block(im->fmt, color.float32, block);
    if (!bytes || bytes != f->bpb) { VKRED_ONCE(71, "compressed texture clear format has no constant block encoding"); return 0; }
    VkDeviceSize row = (((VkDeviceSize)w + f->bw - 1) / f->bw) * (unsigned)bytes;
    if (!row || row > SIZE_MAX) { VKRED_ONCE(71, "compressed texture clear row size overflow"); return 0; }
    uint32_t rows = h / f->bh + (h % f->bh != 0);
    VkDeviceSize chunk_rows = 65536 / row; if (!chunk_rows) chunk_rows = 1;
    if (chunk_rows > rows) chunk_rows = rows;
    VkDeviceSize n = row * chunk_rows, off; VkBuffer buf;
    uint8_t *dst = vk_scratch(n, 16, &buf, &off);
    for (VkDeviceSize at = 0; at < n; at += (unsigned)bytes) memcpy(dst + at, block, (size_t)bytes);
    int volume = im->target == NVN_TEXTURE_TARGET_3D;
    uint32_t slices = volume ? (im->d >> lv ? im->d >> lv : 1) : count;
    VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, im); vk_img_barrier_all(cb);
    for (uint32_t slice = 0; slice < slices; slice++) for (uint32_t y = 0; y < h;) {
        uint32_t height = (uint32_t)chunk_rows * f->bh; if (height > h - y) height = h - y;
        VkBufferImageCopy copy = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, lv, volume ? 0 : first + slice, 1},
            {0, (int32_t)y, volume ? (int32_t)slice : 0}, {w, height, 1}};
        vkCmdCopyBufferToImage(cb, buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        y += height;
    }
    vk_img_barrier_all(cb); im->gpu_w = 1;
    return 1;
}
static void clear_tex(const p_clear_tex *c) {
    unsigned mask = (unsigned)c->mask & 15;
    if (!c->tex || c->r.width <= 0 || c->r.height <= 0) return;
    vk_end_pass();
    vkimg_t *im = vk_tex(c->tex); if (!im) return;
    if (!mask && (im->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) return;   // NVN masks only color channels
    uint32_t lv = c->has_view && c->view.set_levels ? (uint32_t)c->view.base_level : 0;
    if (lv >= im->levels) { VKRED_ONCE(71, "texture clear outside mip levels"); return; }
    uint32_t w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1;
    int64_t x = c->r.xoffset, y = c->r.yoffset, x1 = x + c->r.width, y1 = y + c->r.height;
    if (x < 0) x = 0; if (y < 0) y = 0; if (x1 > w) x1 = w; if (y1 > h) y1 = h;
    if (x >= x1 || y >= y1) return;
    VkRect2D rect = {{(int32_t)x,(int32_t)y},{(uint32_t)(x1-x),(uint32_t)(y1-y)}};
    int volume = im->target == NVN_TEXTURE_TARGET_3D;
    uint32_t layers = volume ? (im->d >> lv ? im->d >> lv : 1) : im->layers;
    uint32_t base = !volume && c->has_view && c->view.set_layers ? (uint32_t)c->view.min_layer : 0;
    uint32_t count = c->r.depth ? (uint32_t)c->r.depth : 1;
    uint64_t first = (uint64_t)base + (uint32_t)c->r.zoffset;
    if (!volume && im->layers == 1) { first = 0; count = 1; }   // plain 2D ignores the region's layer coordinates, as sub_of did
    else if (c->r.zoffset < 0 || c->r.depth < 0 || first + count > layers ||
        (!volume && c->has_view && c->view.set_layers && c->view.nlayers && (uint64_t)c->r.zoffset + count > (uint32_t)c->view.nlayers)) {
        VKRED_ONCE(71, "texture clear outside view layers"); return;
    }
    const vkfmt_t *f = vk_fmt(c->has_view && c->view.set_format ? c->view.format : c->tex->d.format);
    const vkfmt_t *storage = vk_fmt(im->nvn_format);
    if (!f || !storage) { VKRED_ONCE(71, "texture clear format is unknown"); return; }
    if (storage->compressed && !storage->dec) {
        if (rect.extent.width != w || rect.extent.height != h || mask != 15 || (volume && count != layers) || f->vk != im->fmt)
            VKRED_ONCE(71, "compressed texture region/view clear: clearing whole subresource with encoded blocks");
        clear_tex_compressed(im, storage, c, lv, (uint32_t)first, count, w, h);
        return;
    }
    unsigned type = f->is_int ? (f->is_sint ? 2 : 1) : 0;
    VkClearColorValue value = clear_tex_value(c, type);
    VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, im); vk_img_barrier_all(cb);
    // Keep the existing whole depth/stencil path; channel masks below concern color.
    int whole = rect.extent.width == w && rect.extent.height == h &&
        (mask == 15 || !(im->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) && (!volume || count == layers);
    // Vulkan whole-image clears also support multisampled images.
    if (whole && (f->vk == im->fmt || im->samples > 1)) goto whole_clear;
    // Keep the old whole-subresource result when the region path cannot run.
    if (im->target == NVN_TEXTURE_TARGET_1D || im->target == NVN_TEXTURE_TARGET_1D_ARRAY) {
        VKRED_ONCE(71, "texture region clear of a 1D image: clearing whole subresource"); goto whole_clear;
    }
    if (im->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        VKRED_ONCE(71, "texture region clear of depth/stencil: clearing whole subresource"); goto whole_clear;
    }
    if (!(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        VKRED_ONCE(71, "texture region clear without attachment usage: clearing whole subresource"); goto whole_clear;
    }
    if (f->compressed || f->dec || storage->dec) {
        VKRED_ONCE(71, "texture region clear with a compressed/decoded format: clearing whole subresource"); goto whole_clear;
    }
    view_t view = c->has_view ? c->view : (view_t){0};
    view.set_levels = view.set_layers = 1; view.base_level = lv; view.nlevels = view.nlayers = 1;
    for (uint32_t i=0;i<count;i++) {
        view.min_layer = (uint32_t)first+i;
        VkImageView v = vk_tex_view(c->tex, &view, 0xff, 0, 1, NULL);
        if (!v) { VKRED_ONCE(71, "texture region clear view failed: clearing whole subresource"); goto whole_clear; }
        if (vk_view_fmt != f->vk) { VKRED_ONCE(71, "texture clear view format is not supported by the image: clearing whole subresource"); goto whole_clear; }
        if (!clear_tex_slice(im,v,vk_view_fmt,(VkExtent2D){w,h},rect,mask,type,&value)) {
            VKRED_ONCE(71, "texture region clear failed: clearing whole subresource"); goto whole_clear;
        }
    }
    im->gpu_w = 1;
    return;
whole_clear:;
    VkImageSubresourceRange range = {im->aspect, lv, 1, volume ? 0 : (uint32_t)first, volume ? 1 : count};
    if (im->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        // A transfer uses the image's actual format, including after a rejected view.
        value = clear_tex_value(c, storage->is_int ? (storage->is_sint ? 2 : 1) : 0);
        vkCmdClearColorImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    } else { VkClearDepthStencilValue ds = {c->c.f[0],c->c.u[1]}; vkCmdClearDepthStencilImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &ds, 1, &range); }
    vk_img_barrier_all(cb);
    im->gpu_w = 1;
}

