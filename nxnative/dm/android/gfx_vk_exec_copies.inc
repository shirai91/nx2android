// ---------------- copies and clears ----------------
static VkImageSubresourceLayers sub_of(vkimg_t *im, const view_t *v, int has_v, int z) {
    uint32_t lv = has_v && v->set_levels ? v->base_level : 0, ly = has_v && v->set_layers ? v->min_layer : 0;
    int layered = im->layers > 1;
    return (VkImageSubresourceLayers){im->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT), lv, ly + (layered ? (uint32_t)z : 0), 1};
}
static void copy_b2t(const p_copy_b2t *c, int to_texture) {
    vk_end_pass_land();
    vkimg_t *im = vk_tex(c->tex); const vkfmt_t *f = vk_fmt(c->tex->d.format);
    if (!im || !f) return;
    VkBuffer b; VkDeviceSize o;
    if (!vk_buf_at(c->src, 1, &b, &o)) { VKRED_ONCE(70, "buffer of a buffer ↔ texture copy outside the memory pools"); return; }
    VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, im); vk_img_barrier_all(cb);
    unsigned bw = f->bw ? f->bw : 1, bh = f->bh ? f->bh : 1;
    uint32_t rowlen = S.row_stride > 0 && f->bpb ? (uint32_t)(S.row_stride / f->bpb) * bw : 0;
    uint32_t imgh = S.image_stride > 0 && S.row_stride > 0 ? (uint32_t)(S.image_stride / S.row_stride) * bh : 0;
    int layered = im->layers > 1, depth = c->r.depth ? c->r.depth : 1;
    VkBufferImageCopy r = {o, rowlen, imgh, sub_of(im, &c->view, c->has_view, c->r.zoffset), {c->r.xoffset, c->r.yoffset, layered ? 0 : c->r.zoffset},
                           {(uint32_t)c->r.width, (uint32_t)(c->r.height ? c->r.height : 1), layered ? 1 : (uint32_t)depth}};
    if (layered) r.imageSubresource.layerCount = (uint32_t)depth;
    if (f->dec) {   // a BC format the device lacks (gfx_vk_tex.c bc_decode): the blocks decoded on the CPU from the buffer's memory as the replay reaches the copy
        if (!to_texture) { VKRED_ONCE(71, "copy of a BC texture decoded on the CPU into a buffer: skipped"); return; }
        uint32_t w = (uint32_t)c->r.width, h = (uint32_t)(c->r.height ? c->r.height : 1), nz = (uint32_t)depth;
        size_t pitch = S.row_stride > 0 ? (size_t)S.row_stride : (size_t)((w + 3) / 4) * f->bpb, slice = S.image_stride > 0 ? (size_t)S.image_stride : pitch * ((h + 3) / 4), out = (size_t)w * h * f->dbpp;
        VkBuffer sb; VkDeviceSize so; uint8_t *p = vk_scratch(out * nz, 16, &sb, &so);
        for (uint32_t z = 0; z < nz; z++) vk_bc_decode(f, (const uint8_t *)(uintptr_t)c->src + z * slice, pitch, w, h, 1, p + z * out);
        b = sb; r.bufferOffset = so; r.bufferRowLength = 0; r.bufferImageHeight = 0;
    }
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
    if (g_watch_off >= 0) { char w[96]; snprintf(w, sizeof w, "buffer copy %s (buffer %#llx, %dx%d)", to_texture ? "destination" : "source", (unsigned long long)c->src, c->r.width, c->r.height); vk_watch(im, w); }
    if (to_texture) { vkCmdCopyBufferToImage(cb, b, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r); im->gpu_w = 1; }
    else { vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
        void vk_gpu_wrote(uint64_t a, uint64_t n); vkpool_t *p = vk_pool_of(c->src, 1); vk_gpu_wrote(c->src, p ? p->hi - c->src : 1); vk_readback_unknown(); }   // (to the pool's end: the copy's extent in memory is not worked out here)
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
static void copy_t2t(const p_copy_t2t *c) {
    vk_end_pass();
    vkimg_t *s = vk_tex(c->src), *d = vk_tex(c->dst);
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
    vk_img_barrier_all(cb);
}
static void clear_tex(const p_clear_tex *c) {
    vk_end_pass();
    vkimg_t *im = vk_tex(c->tex); if (!im) return;
    VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, im); vk_img_barrier_all(cb); im->gpu_w = 1;
    VkImageSubresourceLayers l = sub_of(im, &c->view, c->has_view, c->r.zoffset);
    VkImageSubresourceRange rg = {l.aspectMask, l.mipLevel, 1, l.baseArrayLayer, im->layers > 1 ? (uint32_t)(c->r.depth ? c->r.depth : 1) : 1};
    if (c->r.xoffset || c->r.yoffset || (uint32_t)c->r.width < (im->w >> l.mipLevel) || (uint32_t)c->r.height < (im->h >> l.mipLevel)) VKRED_ONCE(71, "partial texture clear: the whole level is cleared");
    if (im->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue v; memcpy(&v, c->c.u, 16);
        const vkfmt_t *f = vk_fmt(c->has_view && c->view.set_format ? c->view.format : c->tex->d.format);
        if (f && ((f->vk == VK_FORMAT_R8G8B8A8_SRGB && im->fmt == VK_FORMAT_R8G8B8A8_UNORM) ||
                  (f->vk == VK_FORMAT_B8G8R8A8_SRGB && im->fmt == VK_FORMAT_B8G8R8A8_UNORM))) {
            // ClearColorImage encodes only an sRGB image; family backing is UNORM.
            for (int i = 0; i < 3; i++) { float x = fminf(fmaxf(v.float32[i], 0.0f), 1.0f); v.float32[i] = x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f; }
        }
        vkCmdClearColorImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rg);
    }
    else { VkClearDepthStencilValue v = {c->c.f[0], c->c.u[1]}; rg.aspectMask = im->aspect; vkCmdClearDepthStencilImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rg); }
    vk_img_barrier_all(cb);
}

