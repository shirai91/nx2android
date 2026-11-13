// Existing Android-parity DrawTexture (1134e0e75), adapted to nx2android replay.
static VkRect2D draw_texture_rect(const NVNdrawTextureRegion *d, VkExtent2D size) {
    double left = fmax(0, fmin(d->x0, d->x1)), right = fmin(size.width, fmax(d->x0, d->x1));
    double top = fmax(0, fmin(d->y0, d->y1)), bottom = fmin(size.height, fmax(d->y0, d->y1));
    if (S.sc_set) {
        left = fmax(left, S.sc.x); right = fmin(right, (double)S.sc.x + S.sc.w);
        top = fmax(top, S.sc.y); bottom = fmin(bottom, (double)S.sc.y + S.sc.h);
    }
    if (!(left < right && top < bottom)) return (VkRect2D){0};
    int x = (int)ceil(left - .5), y = (int)ceil(top - .5);
    return (VkRect2D){{x,y},{(uint32_t)((int)ceil(right - .5) - x),(uint32_t)((int)ceil(bottom - .5) - y)}};
}

// nxnative 1134e0e75 draw_texture.inc: use this backend's utility-pipeline
// construction, descriptor allocator and pass tracking for the donor draw.
#include "draw_texture_frag.spv.h"
static struct {
    VkPipelineLayout pl; VkDescriptorSetLayout dsl; VkShaderModule vs, fs;
    int bad; nvn_hmap cache;
} g_dt = {.cache = {.klen = sizeof(mckey)}};
static VkPipeline dt_pipeline(uint32_t mask) {
    mckey k; memset(&k, 0, sizeof k); k.ncol = (uint32_t)S.ncol; for (int i = 0; i < S.ncol && i < 8; i++) k.cfmt[i] = (uint32_t)S.cfmt[i];
    k.dfmt = (uint32_t)S.dfmt; k.samples = S.samples ? S.samples : 1; k.mask = mask;
    VkPipeline cached = nvn_hm_get(&g_dt.cache, &k);
    if (cached || g_dt.bad) return cached;
    if (!g_dt.pl) {
        VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
        VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &binding};
        if (vkCreateDescriptorSetLayout(V.dev, &di, NULL, &g_dt.dsl)) { g_dt.bad = 1; return VK_NULL_HANDLE; }
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_dt.dsl, 1, &pr};
        VkShaderModuleCreateInfo vi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof maskclear_vert_spv, maskclear_vert_spv}, fi = vi;
        fi.codeSize = sizeof draw_texture_frag_spv; fi.pCode = draw_texture_frag_spv;
        if (vkCreatePipelineLayout(V.dev, &li, NULL, &g_dt.pl) || vkCreateShaderModule(V.dev, &vi, NULL, &g_dt.vs) || vkCreateShaderModule(V.dev, &fi, NULL, &g_dt.fs)) { g_dt.bad = 1; return VK_NULL_HANDLE; }
    }
    VkPipelineShaderStageCreateInfo ss[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, g_dt.vs, "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_dt.fs, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)k.samples, 0, 0, NULL, 0, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8]; memset(cba, 0, sizeof cba);
    cba[0].colorWriteMask = mask & 0xf;   // NVN's mask bits R G B A are Vulkan's
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, k.ncol, cba, {0, 0, 0, 0}};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkFormat cf[8]; for (uint32_t i = 0; i < k.ncol; i++) cf[i] = (VkFormat)k.cfmt[i];
    VkFormat df = (VkFormat)k.dfmt; int has_stencil = df == VK_FORMAT_D24_UNORM_S8_UINT || df == VK_FORMAT_D32_SFLOAT_S8_UINT || df == VK_FORMAT_S8_UINT;
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, k.ncol, cf, df == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : df, has_stencil ? df : VK_FORMAT_UNDEFINED};
    VkRenderPass prp = rp_on() ? rp_compat(k.ncol, k.cfmt, k.dfmt, k.samples) : VK_NULL_HANDLE;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, prp ? NULL : &rci, 0, 2, ss, &vin, &ia, NULL, &vs, &rs, &ms, &ds, &cb, &dy, g_dt.pl, prp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    if ((!prp && !V.has_dynrender) || vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &p)) { VKRED_ONCE(32, "DrawTexture pipeline failed: skipped"); p = VK_NULL_HANDLE; }
    vk_pipeline_cache_changed();
    if (p) nvn_hm_put(&g_dt.cache, &k, p); return p;
}

static void draw_texture(const p_draw_tex *c) {
    float dw = c->d.x1 - c->d.x0, dh = c->d.y1 - c->d.y0;
    if (!S.rt_n || !dw || !dh) return;
    vk_end_pass_land(); // pending clears and previous guest writes land first
    const tex_t *registered; const view_t *view;
    if (!vk_tex_by_id((int)(c->h & 0xfffff), &view, &registered)) return;
    tex_t texture = *registered; view_t vw = view ? *view : (view_t){0};
    VkSampler vk_samp_at(int sid, uint64_t seq, int nearest); uint64_t mtl_samp_seq(void);
    uint64_t seq = g_samp_snap && c->sseq < mtl_samp_seq() ? c->sseq : 0;
    int sid = (int)(c->h >> 20);
    VkSampler sampler = seq ? vk_samp_at(sid, seq, 0) : vk_samp_by_id(sid);
    vkimg_t *im = vk_tex(&texture);
    int type = vw.set_target ? vw.target : texture.d.target;
    const vkfmt_t *format = vk_fmt(vw.set_format ? vw.format : texture.d.format);
    if (!im || !sampler || !format) { VKRED_ONCE(102, "DrawTexture handle has no texture or sampler"); return; }
    if ((type != NVN_TEXTURE_TARGET_2D && type != NVN_TEXTURE_TARGET_RECTANGLE) ||
        im->samples != VK_SAMPLE_COUNT_1_BIT || format->depth || format->is_int) {
        VKRED_ONCE(102, "DrawTexture requires a single-sample float or normalized 2D view"); return;
    }
    uint32_t base = vw.set_levels ? vw.base_level : 0, layer = vw.set_layers ? vw.min_layer : 0;
    if (base >= im->levels || layer >= im->layers) return;
    uint32_t levels = im->levels - base;
    if (vw.set_levels && vw.nlevels && vw.nlevels < levels) levels = vw.nlevels;
    VkImageView source = vk_tex_view(&texture, &vw, 1, 0, 0, &im);
    if (!source) return;
    vk_tex_prepare(vk_cmd(), im); vk_img_barrier_all(vk_cmd());
    // Preserve all visible mips before writing any target, as in the donor.
    for (int i = 0; i < S.rt_n; ++i) {
        if (!S.rt_has_col[i] || !(S.chmask.mask >> (4*i) & 15)) continue;
        const view_t *v = S.rt_has_cview[i] ? &S.rt_cview[i] : NULL;
        uint32_t lv = v && v->set_levels ? v->base_level : 0, ly = v && v->set_layers ? v->min_layer : 0;
        if (ly == layer && lv - base < levels && vk_tex(&S.rt_col[i]) == im) {
            source = vk_tex_sampled_snapshot(im, source);
            if (!source) return;
            break;
        }
    }
    float params[] = {c->d.x0,c->d.y0,c->s.x0,c->s.y0,(c->s.x1-c->s.x0)/dw,(c->s.y1-c->s.y0)/dh};
    if (S.color.blend_en) VKRED_ONCE(102, "DrawTexture with blending enabled draws without blending, as on Metal");
    // The donor draws each target separately. Only temporarily replace the
    // target slot used by begin_pass; the guest's programs and bindings stay put.
    tex_t saved_tex = S.rt_col[0]; view_t saved_view = S.rt_cview[0];
    int count = S.rt_n, has_tex = S.rt_has_col[0], has_view = S.rt_has_cview[0], has_depth = S.rt_has_dep;
    uint32_t saved_bad = S.rt_bad[0], saved_mask = S.chmask.mask;
    for (int i = 0; i < count; ++i) {
        unsigned mask = saved_mask >> (4*i) & 15;
        if (!S.rt_has_col[i] || !mask) continue;
        const tex_t *t = &S.rt_col[i]; const view_t *v = S.rt_has_cview[i] ? &S.rt_cview[i] : NULL;
        const vkfmt_t *tf = vk_fmt(t->d.format), *f = vk_fmt(v && v->set_format ? v->format : t->d.format);
        if (!f || f->is_int || f->depth || !tf || tf->dec) {
            VKRED_ONCE(102, "DrawTexture requires a float or normalized color target"); continue;
        }
        S.rt_col[0] = *t; if (v) S.rt_cview[0] = *v;
        S.rt_has_col[0] = 1; S.rt_has_cview[0] = v != NULL;
        S.rt_bad[0] = 0; S.rt_n = 1; S.rt_has_dep = 0; S.chmask.mask = mask;
        if (begin_pass()) {
            VkExtent2D size = {S.pass_w, S.pass_h}; VkRect2D rect = draw_texture_rect(&c->d, size);
            VkPipeline pipeline = rect.extent.width && rect.extent.height ? dt_pipeline(mask) : VK_NULL_HANDLE;
            if (pipeline && pass_commit()) {
                VkDescriptorSet set = vk_alloc_set(g_dt.dsl);
                VkDescriptorImageInfo image = {sampler, source, VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image, NULL, NULL};
                vkUpdateDescriptorSets(V.dev, 1, &write, 0, NULL);
                VkCommandBuffer cb = vk_cmd(); VkViewport vp = {0,0,(float)size.width,(float)size.height,0,1};
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &rect);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dt.pl, 0, 1, &set, 0, NULL);
                vkCmdPushConstants(cb, g_dt.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof params, params);
                vkCmdDraw(cb, 3, 1, 0, 0);
                g_pass_draws++; g_cb_draws++; g_bc_stale = 1;
                g_pass_bb[0] = rect.offset.x; g_pass_bb[1] = rect.offset.y;
                g_pass_bb[2] = rect.offset.x + rect.extent.width; g_pass_bb[3] = rect.offset.y + rect.extent.height;
            }
        }
        vk_end_pass(); // includes the target's existing linear writeback
        vk_img_barrier_all(vk_cmd());
        S.rt_col[0] = saved_tex; S.rt_cview[0] = saved_view;
        S.rt_has_col[0] = has_tex; S.rt_has_cview[0] = has_view;
        S.rt_bad[0] = saved_bad; S.rt_n = count; S.rt_has_dep = has_depth; S.chmask.mask = saved_mask;
    }
}
