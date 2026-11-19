// ---------------- transform feedback ----------------
// NVN's capture state (OP_TF_BIND / OP_TF_STATE). Each capturing draw (feedback on, a vertex shader translated with Xfb: shader_vk.py xfb_layout) is a Vulkan capture of its own:
// the buffers bound at the bytes written so far, Begin, the draw, End. So no pipeline is bound and no pass ends while a capture is active, and no counter buffer is read back:
// the host counts the bytes, as nvn_mtl.m tf_draw does (vertices the primitives emit × the buffer's stride). Xenoblade Chronicles 3 steps its GPU particles this way (8 shaders,
// up to 9 outputs interleaved in one buffer); before, those draws wrote nothing and the particles kept stale or zero state
static uint32_t xfb_verts(int prim, uint32_t n) {   // vertices a capture writes for n vertices of this primitive (strips and fans are written as lists)
    switch (topo(prim)) {
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return n & ~1u;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return n >= 2 ? (n - 1) * 2 : 0;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return n / 3 * 3;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return n >= 3 ? (n - 2) * 3 : 0;
    default: return n;
    }
}
// A geometry shader's capture (Xenoblade Chronicles 3's particle update, 293ecebc0136519b: what it emits, 172 bytes a particle) writes a count no host can know: the bytes go to a
// counter buffer, 16 bytes for each control buffer's slot (one u32 a feedback buffer), which the next capture of the sequence starts from and DrawTransformFeedback draws by
// (vkCmdDrawIndirectByteCountEXT). Without it the particle update wrote nothing: no red particles at husks, none rising at a send-off, no glow on far items
enum { TFC = 4096 }; static struct { uint64_t ctl; uint32_t verts; uint8_t gs, ctr_ok, dirty; } g_tf_ctl[TFC];
static VkBuffer g_tfc_buf; static int g_tfc_cur = -1, g_bc_slot = -1;   // the counter buffer; the slot of the open sequence; a DrawTransformFeedback drawn by its counter
static int tfc_ready(void) {
    static int failed; if (g_tfc_buf || failed) return g_tfc_buf != VK_NULL_HANDLE;
    if (!vkCmdDrawIndirectByteCountEXT) { failed = 1; VKRED_ONCE(91, "transform feedback from a geometry shader: no vkCmdDrawIndirectByteCountEXT, its captures are dropped"); return 0; }
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)TFC * 16, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkDeviceMemory m; VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &g_tfc_buf)); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, g_tfc_buf, &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &m)); VKCHK(vkBindBufferMemory(V.dev, g_tfc_buf, m, 0));
    VKLOG("transform feedback from a geometry shader: counter buffer of %d slots", TFC); return 1;
}
static vkshader_t *xfb_shader(void) { return S.sh[2] && S.sh[2]->xfb ? S.sh[2] : S.sh[0]; }   // the last stage before rasterization captures
// Preserve the donor's known-write / GPU-producer order. Unlike copies and
// dispatches, a draw can otherwise leave an older UpdateUniformBuffer deferred
// until after its transform-feedback or linear-target writeback. Land those
// overlapping bytes before the draw, while no per-draw allocation exists.
static void graphics_updates_ready(void) {
    if (g_lin_back) for (int i = 0; i < S.rt_n && i < 8; i++) {
        const tex_t *t = S.rt_has_col[i] ? &S.rt_col[i] : NULL;
        if (t && t->mem && (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) &&
                vk_ubd_overlaps((uintptr_t)t->mem, t->L.size)) { vk_end_pass_land(); return; }
    }
    const vkshader_t *s = xfb_shader();
    if (g_tf_on && V.has_xfb && g_xfb_on && s && s->xfb) for (int i = 0; i < 4; i++)
        if (s->xfb_stride[i] && g_tf[i].addr && vk_ubd_overlaps(g_tf[i].addr, g_tf[i].size)) { vk_end_pass_land(); return; }
}
static int xfb_begin(VkCommandBuffer cb) {   // inside the pass, after the pipeline is bound; 1 when a capture began, 2 when it counts on the GPU (a geometry shader)
    vkshader_t *vs = xfb_shader();
    if (!g_tf_on || !V.has_xfb || !vs || !vs->xfb || !g_xfb_on) return 0;
    int gs = vs == S.sh[2];
    if (gs && (g_tfc_cur < 0 || !tfc_ready())) return 0;
    vk_readback_unknown();   // capture extents need not match the input vertex count
    VkBuffer b[4]; VkDeviceSize o[4], n[4];
    for (int k = 0; k < 4; k++) {
        uint64_t from = gs ? 0 : g_tf_off[k];   // a counted capture is bound at the buffer's start: the counter says where it goes on
        vkpool_t *p = g_tf[k].addr ? vk_pool_of(g_tf[k].addr + from, 4) : NULL;
        if (vs->xfb_stride[k] && p && from < g_tf[k].size && vk_buf_at(g_tf[k].addr + from, 4, &b[k], &o[k])) {
            n[k] = g_tf[k].size - from; if (o[k] - p->boff + n[k] > p->hi - p->lo) n[k] = p->hi - p->lo - (o[k] - p->boff);   // (o is the buffer's offset: a slab slot's starts at boff)
        } else { vk_scratch(64, 64, &b[k], &o[k]); n[k] = 64; if (vs->xfb_stride[k]) VKRED_ONCE(83, "transform feedback buffer %d unbound, full or outside the memory pools: its capture is dropped", k); }
    }
    vkCmdBindTransformFeedbackBuffersEXT(cb, 0, 4, b, o, n);
    if (gs) { VkBuffer cbs[4] = {g_tfc_buf, g_tfc_buf, g_tfc_buf, g_tfc_buf}; VkDeviceSize co[4]; for (int k = 0; k < 4; k++) co[k] = (VkDeviceSize)g_tfc_cur * 16 + 4 * k;
        int resume = g_tf_ctl[g_tfc_cur].ctr_ok; vkCmdBeginTransformFeedbackEXT(cb, 0, 4, resume ? cbs : NULL, resume ? co : NULL); return 2; }
    vkCmdBeginTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
    return 1;
}
static void xfb_end(VkCommandBuffer cb, int prim, uint32_t count, uint32_t inst, int xf) {   // after the draw: the capture ends and the bytes it wrote are counted
    void vk_gpu_wrote(uint64_t a, uint64_t n);
    if (xf == 2) {
        VkBuffer cbs[4] = {g_tfc_buf, g_tfc_buf, g_tfc_buf, g_tfc_buf}; VkDeviceSize co[4]; for (int k = 0; k < 4; k++) co[k] = (VkDeviceSize)g_tfc_cur * 16 + 4 * k;
        vkCmdEndTransformFeedbackEXT(cb, 0, 4, cbs, co); g_tf_in_pass = 1; g_tf_ctl[g_tfc_cur].dirty = 1;
        g_tf_ctl[g_tfc_cur].ctr_ok = 1; g_tf_ctl[g_tfc_cur].gs = 1;
        for (int k = 0; k < 4; k++) if (S.sh[2]->xfb_stride[k] && g_tf[k].addr) vk_gpu_wrote(g_tf[k].addr, g_tf[k].size);   // how much is only known on the GPU
        { static int n; if (n++ < 4) VKLOG("transform feedback: geometry shader %016llx captured, counted on the GPU (control buffer %#llx)", (unsigned long long)S.sh[2]->key, (unsigned long long)g_tf_ctl[g_tfc_cur].ctl); }
        return;
    }
    vkCmdEndTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
    uint32_t v = xfb_verts(prim, count) * (inst ? inst : 1); g_tf_in_pass = 1; g_tf_verts += v;
    for (int k = 0; k < 4; k++) {
        uint64_t m = (uint64_t)v * S.sh[0]->xfb_stride[k]; if (!m || !g_tf[k].addr) continue;
        if (g_tf_off[k] + m > g_tf[k].size) m = g_tf[k].size > g_tf_off[k] ? g_tf[k].size - g_tf_off[k] : 0;
        if (m) vk_gpu_wrote(g_tf[k].addr + g_tf_off[k], m);
        g_tf_off[k] += m;
    }
}
// DrawTransformFeedback draws the vertices the captures ended into one control buffer wrote: counted since Begin (g_tf_verts) and kept by control buffer address at End / Pause
// (a table by address: Xenoblade Chronicles 3 gives every particle emitter a control buffer of its own, hundreds of them, 32 bytes apart)
static int tf_ctl_slot(uint64_t ctl, int add) {   // the entry of a control buffer; with add, a new one (the oldest in its probe range is replaced when they are all taken)
    unsigned h = (unsigned)((ctl >> 5) * 2654435761u) % TFC;
    for (int k = 0; k < 16; k++) { unsigned i = (h + (unsigned)k) % TFC; if (g_tf_ctl[i].ctl == ctl) return (int)i; if (!g_tf_ctl[i].ctl) return add ? (int)i : -1; }
    return add ? (int)h : -1;
}
static void tf_state(const p_tf_state *t) {
    switch (t->what & 3) {
    case 0: memset(g_tf_off, 0, sizeof g_tf_off); g_tf_verts = 0; g_tf_on = 1;   // Begin: written from the start of each buffer
        g_tfc_cur = tf_ctl_slot(t->ctl, 1); g_tf_ctl[g_tfc_cur].ctl = t->ctl; g_tf_ctl[g_tfc_cur].ctr_ok = 0; g_tf_ctl[g_tfc_cur].gs = 0; break;
    case 3: g_tf_on = 1; g_tfc_cur = tf_ctl_slot(t->ctl, 1); if (g_tf_ctl[g_tfc_cur].ctl != t->ctl) { g_tf_ctl[g_tfc_cur].ctl = t->ctl; g_tf_ctl[g_tfc_cur].ctr_ok = 0; g_tf_ctl[g_tfc_cur].gs = 0; } break;   // Resume: on from the bytes written
    default: g_tf_on = 0;                                                // End / Pause: what was captured is read by later draws (as vertices), after the barrier the next pass begins with
        if (g_tf_in_pass) { vk_end_pass(); g_tf_in_pass = 0; }
        int i = tf_ctl_slot(t->ctl, 1); g_tf_ctl[i].ctl = t->ctl; g_tf_ctl[i].verts = g_tf_verts;
        if (g_tf_ctl[i].gs && g_tf_ctl[i].dirty) {   // a counted capture: its bytes into the game's control buffer too (+0, one u32 a feedback buffer), for anything that reads it there
            VkBuffer b; VkDeviceSize o; g_tf_ctl[i].dirty = 0;
            if (vk_ubd_overlaps(t->ctl, 16)) vk_end_pass_land();
            if (vk_buf_at(t->ctl, 16, &b, &o)) { VkCommandBuffer cb = vk_cmd(); VkBufferCopy c = {(VkDeviceSize)i * 16, o, 16};
                VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL); vkCmdCopyBuffer(cb, g_tfc_buf, b, 1, &c);
                void vk_gpu_wrote(uint64_t a, uint64_t n); vk_gpu_wrote(t->ctl, 16); } }
    }
}
static void draw(const p_draw *d);
static void draw_tf(const p_draw_tf *t) {
    int i = tf_ctl_slot(t->ctl, 0);
    if (i < 0) { VKRED_ONCE(86, "DrawTransformFeedback with a control buffer no capture ended into (%#llx): nothing drawn", (unsigned long long)t->ctl); return; }
    if (g_tf_ctl[i].gs) {   // drawn by the counter its geometry shader's captures wrote (see tfc_ready)
        if (!g_tf_ctl[i].ctr_ok) return;
        p_draw d = {t->prim, 0, 1, 1, 0, 0, 0, 0, 0}; g_bc_slot = i; draw(&d); g_bc_slot = -1; return; }
    p_draw d = {t->prim, 0, (int32_t)g_tf_ctl[i].verts, 1, 0, 0, 0, 0, 0};
    if (d.count) draw(&d);
}
static void trace_draw(const char *kind, int prim, uint32_t count, uint32_t inst, int itype) {
    VKLOG("draw: %s prim %d count %u instances %u itype %d; shaders %016llx %016llx %016llx; targets %d (formats %d %d %d %d) depth format %d samples %u, pass %ux%u; viewport %.0f %.0f %.0f %.0f; streams:",
          kind, prim, count, inst, itype, (unsigned long long)(S.sh[0] ? S.sh[0]->key : 0), (unsigned long long)(S.sh[1] ? S.sh[1]->key : 0), (unsigned long long)(S.sh[2] ? S.sh[2]->key : 0),
          S.ncol, S.cfmt[0], S.cfmt[1], S.cfmt[2], S.cfmt[3], S.dfmt, S.samples, S.pass_w, S.pass_h, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3]);
    for (int i = 0; i < S.rt_n; i++) if (S.rt_bad[i]) VKLOG("  colour target %d: not a texture object (first word %#x): left out", i, S.rt_bad[i]);
    for (int i = 0; i < S.rt_n; i++) if (S.rt_has_col[i]) { vkimg_t *im = vk_tex(&S.rt_col[i]); const tex_t *t = &S.rt_col[i];
        VKLOG("  colour target %d: serial %llu → image %p: %ux%ux%u layers %u levels %u samples %u fmt %d vtype %d; nvn target %d format %#x flags %#x stride %u size %llu pool %p+%#llx",
              i, (unsigned long long)t->serial, (void *)im, im ? im->w : 0, im ? im->h : 0, im ? im->d : 0, im ? im->layers : 0, im ? im->levels : 0, im ? im->samples : 0, im ? im->fmt : 0, im ? im->vtype : 0,
              t->d.target, t->d.format, t->d.flags, t->d.stride, (unsigned long long)t->L.size, (const void *)t->pool, (unsigned long long)t->off);
        if (S.rt_has_cview[i]) { const view_t *v = &S.rt_cview[i]; VKLOG("  its view: levels %d (%u+%u) layers %d (%u+%u) target %d (%u) format %d (%#x)",
              v->set_levels, v->base_level, v->nlevels, v->set_layers, v->min_layer, v->nlayers, v->set_target, v->target, v->set_format, v->format); } }
    if (S.rt_has_dep) { vkimg_t *im = vk_tex(&S.rt_dep); const tex_t *t = &S.rt_dep;
        VKLOG("  depth target: serial %llu → image %p: %ux%u fmt %d; nvn format %#x flags %#x size %llu pool %p+%#llx", (unsigned long long)t->serial, (void *)im, im ? im->w : 0, im ? im->h : 0,
              im ? im->fmt : 0, t->d.format, t->d.flags, (unsigned long long)t->L.size, (const void *)t->pool, (unsigned long long)t->off); }
    VKLOG("  scissor set %d: %d %d %d %d; depth range %.3f %.3f; viewport count first %.1f %.1f %.1f %.1f", S.sc_set, S.sc.x, S.sc.y, S.sc.w, S.sc.h, S.dnear, S.dfar, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3]);
    vkshader_t *vsh = S.sh[0]; uint32_t in_mask = vsh ? vsh->local[0] : 0;
    for (int l = 0; l < 16; l++) if (in_mask >> l & 1) { uint32_t a = S.vattr[l]; VKLOG("  location %d: attrib state %#x (stream %u) stride %d divisor %d buffer %#llx +%llu", l, a, (a >> 16) & 0x1f,
        S.vstream[(a >> 16) & 0xf].stride, S.vstream[(a >> 16) & 0xf].divisor, (unsigned long long)S.vbuf[(a >> 16) & 0xf].address, (unsigned long long)S.vbuf[(a >> 16) & 0xf].size); }
}
// Vertex fetches past a stream's bound size. NVN binds each stream with a size (BindVertexBuffers), and Maxwell's fetch reads zeros past it; Vulkan's binding carries no
// size here (robustBufferAccess is off), so Adreno read on into whatever followed the stream in the pool. agl draws full-screen passes past their streams: Super Mario
// Odyssey's alNoiseShader 9 vertices from a 36-byte stream (one triangle) and an alProgramShader pass 18 vertices from 72 bytes, every frame in Cap Kingdom; the extra
// vertices made triangles of the pool's next bytes (on Metal the hard-edged mist slabs of the Cascade Kingdom, seedds/nxnative issue #558; issue #42 here, seams on
// volumetrics). For a direct draw, a stream whose last
// element read (the last vertex, the last instance of a per-instance stream, element 0 of a stride-0 stream) reaches past its bound size is bound as a copy of its bound
// bytes followed by zeros. Indexed and indirect draws are not bounded (their last element is not known without reading the indices or the arguments).
// DM_VK_VBOUND=0 (live): bound as before (A/B)
static uint32_t vfmt_bytes(VkFormat f) {   // a vertex attribute's bytes (the formats vertex_format returns)
    if (f >= VK_FORMAT_R8_UNORM && f <= VK_FORMAT_R8_SRGB) return 1;
    if (f >= VK_FORMAT_R8G8_UNORM && f <= VK_FORMAT_R8G8_SRGB) return 2;
    if (f >= VK_FORMAT_R8G8B8_UNORM && f <= VK_FORMAT_B8G8R8_SRGB) return 3;
    if (f >= VK_FORMAT_R8G8B8A8_UNORM && f <= VK_FORMAT_A2B10G10R10_SINT_PACK32) return 4;
    if (f >= VK_FORMAT_R16_UNORM && f <= VK_FORMAT_R16_SFLOAT) return 2;
    if (f >= VK_FORMAT_R16G16_UNORM && f <= VK_FORMAT_R16G16_SFLOAT) return 4;
    if (f >= VK_FORMAT_R16G16B16_UNORM && f <= VK_FORMAT_R16G16B16_SFLOAT) return 6;
    if (f >= VK_FORMAT_R16G16B16A16_UNORM && f <= VK_FORMAT_R16G16B16A16_SFLOAT) return 8;
    if (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32_SFLOAT) return 4;
    if (f >= VK_FORMAT_R32G32_UINT && f <= VK_FORMAT_R32G32_SFLOAT) return 8;
    if (f >= VK_FORMAT_R32G32B32_UINT && f <= VK_FORMAT_R32G32B32_SFLOAT) return 12;
    if (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) return 4;
    return 16;
}
static uint64_t g_vbound_n;
static void vbuf_bound(VkCommandBuffer cb, uint32_t first, uint32_t nvert, uint32_t binst, uint32_t inst) {
    vkshader_t *vsh = S.sh[0]; if (!g_vbound || !vsh || !nvert || (dbg_draw() & 2) || g_var_now == 5) return;
    uint32_t in_mask = vsh->local[0], int_mask = vsh->local[1] | vsh->local[2], sint_mask = vsh->local[1]; uint64_t need[16] = {0};
    for (int loc = 0; loc < 16; loc++) {
        if (!(in_mask >> loc & 1)) continue;
        va_st a; memcpy(&a, &S.vattr[loc], 4); if (!a.fmt || !S.vbuf[a.stream].address) continue;
        VkFormat f = vertex_format(a.fmt, int_mask >> loc & 1, sint_mask >> loc & 1); if (f == VK_FORMAT_UNDEFINED) continue;
        uint32_t s = a.stream, st = S.vstream[s].stride, dv = S.vstream[s].divisor;
        uint64_t last = !st ? 0 : dv ? binst + (inst - 1) / dv : (uint64_t)first + nvert - 1, e = last * st + a.offset + vfmt_bytes(f);
        if (e > need[s]) need[s] = e;
    }
    for (uint32_t s = 0; s < 16; s++) {
        uint64_t n = need[s], sz = S.vbuf[s].size; if (n <= sz || !sz) continue;
        if (n > (16u << 20)) { VKRED_ONCE(87, "vertex stream %u: a draw reads %llu bytes of a %llu-byte binding: not bounded", s, (unsigned long long)n, (unsigned long long)sz); continue; }
        VkBuffer b; VkDeviceSize o; uint8_t *p = vk_scratch(n, 16, &b, &o);
        vk_ubd_read(S.vbuf[s].address, sz, p); memset(p + sz, 0, (size_t)(n - sz));
        vkCmdBindVertexBuffers(cb, s, 1, &b, &o); g_bc.vb[s] = b; g_bc.vo[s] = o;
        if (g_vbound_n++ < 8) VKLOG("vertex stream %u: draw of %u vertices from %u (shader %016llx) reads %llu bytes past its bound %llu: bound as a copy padded with zeros",
                                    s, nvert, first, (unsigned long long)vsh->key, (unsigned long long)(n - sz), (unsigned long long)sz);
    }
}
// An indirect-count multi-draw (hle_nvn.c nvnCommandBufferMultiDrawElementsIndirectCount) arrives as one draw per record: only those below the u32 count the game's GPU culling wrote may be
// made. Drawn regardless, the records past it (stale, written for other meshes) put wrong geometry on the screen, at the camera angles where the count was low (MH Rise, big flat polygons and
// streaks). The whole group is one draw with a count at its first record, the rest are skipped: 1 skip this record, 0 draw it as the group (*cb, *co: the count) or, without the feature, alone
static int mdi_log_now(void) { static int64_t f = -2; if (f == -2) { const char *e = getenv("DM_VK_MDI_LOG"); f = e ? atoll(e) : -1; } return f >= 0 && (int64_t)g_tex_frame >= f && (int64_t)g_tex_frame < f + 2; }
static int mdi_group(uint64_t cnt, uint64_t ind, int i, int max, int stride, int rec, VkBuffer *cb, VkDeviceSize *co) {
    if (!cnt) return 0;
    static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_MDI"); on = e ? atoi(e) : 1; }   // 0: every record drawn, whatever the count
    static int64_t logf = -2; if (logf == -2) { const char *e = getenv("DM_VK_MDI_LOG"); logf = e ? atoll(e) : -1; }
    if (logf >= 0 && (int64_t)g_tex_frame >= logf && (int64_t)g_tex_frame < logf + 2 && !i && ind) {   // the count as the GPU copy holds it now (written by the GPU earlier) and as the CPU's memory has it; records with instances below and past it
        vkpool_t *cp = vk_pool_of(cnt, 4), *rp = vk_pool_of(ind, (uint64_t)stride * (uint64_t)max); uint32_t gc = ~0u, cc = 0; vk_ubd_read(cnt, 4, (uint8_t *)&cc);
        if (cp && cp->shadow) memcpy(&gc, cp->shadow + (cnt - cp->lo), 4);
        int below = 0, past = 0, zero = 0; if (rp && rp->shadow) for (int k = 0; k < max; k++) { uint32_t w[2]; memcpy(w, rp->shadow + (ind - rp->lo) + (uint64_t)k * (uint64_t)stride, 8); if (!w[0] || !w[1]) zero++; else if ((uint32_t)k < gc) below++; else past++; }
        VKLOG("[mdi] frame %llu count at %#llx (pool %#llx): GPU copy %u, CPU %u; max %d; records at %#llx (pool %#llx): %d live below the count, %d live past it, %d empty", (unsigned long long)g_tex_frame, (unsigned long long)cnt, (unsigned long long)(cp ? cp->lo : 0), gc, cc, max, (unsigned long long)ind, (unsigned long long)(rp ? rp->lo : 0), below, past, zero);
        static int nrec; if (max > 1 && rp && rp->shadow && nrec < 40) { nrec++; for (int k = 0; k < max && k < 8; k++) { uint32_t w[5]; memcpy(w, rp->shadow + (ind - rp->lo) + (uint64_t)k * (uint64_t)stride, sizeof w); VKLOG("[mdir] %#llx #%d: %u %u %u %d %u", (unsigned long long)ind, k, w[0], w[1], w[2], (int)w[3], w[4]); } } }
    if (!on) return 0;
    if (on == 2 || on == 3) { vkpool_t *cp = vk_pool_of(cnt, 4); int copied = cp && cp->shadow; if (copied != (on == 2)) return 0; }   // A/B: the count honoured only in copied pools (2) or only outside them (3)
    if (!V.has_draw_count || stride < rec || stride & 3 || (uint32_t)max > V.props.limits.maxDrawIndirectCount) { VKRED_ONCE(52, "multi-draw indirect with a count, not usable here (max %d, stride %d): every record drawn", max, stride); return 0; }
    if (i) return 1;
    if (!vk_buf_at(cnt, 4, cb, co)) { VKRED_ONCE(55, "multi-draw indirect: count outside the memory pools, nothing drawn"); return 1; }
    { static int n; if (!n++) VKLOG("multi-draw indirect with a count: drawn by the GPU's count (first: max %d, stride %d)", max, stride); }
    return 0;
}
// GS stores on Adreno run once per possible output vertex. Keep their effects in
// a discarded one-output pass, then rasterize without the stores. On NieR's index
// builders this changed three copies of each triangle to one and recovered the
// colour pass without changing depth comparisons. Indirect arguments, XFB, user
// UBO/image aliases and VS memory effects stay on the original path.
// Off unless a profile sets DM_VK_GS_SPLIT=1 (NieR:Automata): each split draw ends the pass and adds a barrier.
// The switch is read with the other live ones (x_read); DM_VK_GS_SPLIT=0 restores the original path for an A/B.
static int gs_ranges_overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return a && b && n && m && (a >= b ? a - b < m : b - a < n);
}
// The second draw re-runs the VS and vertex/index fetch. Its inputs must not be
// among the GS's mutable buffers, even if the GS's own outputs are independent.
static int gs_split_inputs_safe(vkshader_t *gs, uint64_t ibuf, uint64_t in) {
    for (int i = 0; i < gs->nbind; i++) {
        const dmsv_bind *b = gs->b + i;
        if (b->kind != RK_SBUF) continue;
        if (b->reg < 0 || b->reg >= 16) return 0;
        p_bind_buf *wr = &S.ssbo[2][b->reg];
        if (!wr->addr || !wr->size || wr->size > UINT64_MAX - wr->addr) return 0;
        if (gs_ranges_overlap(wr->addr, wr->size, ibuf, in)) return 0;
        for (int v = 0; v < 16; v++) {
            if (S.vbuf[v].address && !S.vbuf[v].size) return 0;
            if (gs_ranges_overlap(wr->addr, wr->size, S.vbuf[v].address, S.vbuf[v].size)) return 0;
        }
        for (int v = 0; v < S.sh[0]->nbind; v++) {
            const dmsv_bind *r = S.sh[0]->b + v;
            if (r->kind == RK_SUPPORT || r->kind == RK_DRV || r->kind == RK_C1) continue;
            if (r->reg < 0 || r->reg >= 16) return 0;
            p_bind_buf *rd;
            if (r->kind == RK_CBUF || r->kind == RK_UBO_PTR) rd = &S.ubo[0][r->reg];
            else if (r->kind == RK_SBUF) rd = &S.ssbo[0][r->reg];
            else return 0; // Textures and indirect handles can conceal a buffer alias.
            if (rd->addr && !rd->size) return 0;
            if (gs_ranges_overlap(wr->addr, wr->size, rd->addr, rd->size)) return 0;
        }
    }
    return 1;
}
static int gs_split_draw(int prim, uint64_t indirect, uint64_t ibuf, uint64_t in) {
    if (!S.sh[2] || !g_gs_split || !vk_quirk(VQ_GS_SPLIT)) return 0;
    if (g_gs_split_mode || indirect || g_tf_on || g_bc_slot >= 0 || !S.sh[0] || S.sh[0]->xfb || shader_wmem(S.sh[0]) || g_spv_alt) return 0;
    vkshader_t *gs = vk_shader_gs_topo(S.sh[2], topo(prim == 7 ? 4 : prim));
    if (!gs || (S.sh[1] && (S.sh[1]->in_locs & ~gs->out_locs))) return 0;
    return vk_shader_gs_split_ready(gs) && gs_split_inputs_safe(gs, ibuf, in);
}
static void draw(const p_draw *d) {
    graphics_updates_ready();
    graphics_cpu_ready();
    if (gs_split_draw(d->prim, d->ind, 0, 0)) {
        int discard = S.rast_discard;
        g_gs_split_mode = 1; S.rast_discard = 1; draw(d);
        vk_end_pass(); vk_img_barrier_all(vk_cmd());
        S.rast_discard = discard;
        if (!discard) { g_gs_split_mode = 2; draw(d); }
        g_gs_split_mode = 0; return;
    }
    VkBuffer mcb = VK_NULL_HANDLE; VkDeviceSize mco = 0; if (mdi_group(d->mdi_cnt, d->ind, d->mdi_i, d->mdi_max, d->mdi_stride, 16, &mcb, &mco)) return;
    int prim = d->prim;
    // before prepare_draw, whose driver segment carries it (c0 + 0x644): set after it, each draw's segment had the draw before's. An indirect draw's is in GPU memory: 0
    g_draw_binst = d->ind ? 0 : (uint32_t)d->base_instance;
    if (!prepare_draw(prim == 7 ? 4 : prim, prim == 7)) return;
    VkCommandBuffer cb = vk_cmd();
    int g_ok = draw_allowed(); if (g_ok == 2 || (g_tracing && g_x_tracepass >= 0)) trace_draw("draw", d->prim, (uint32_t)d->count, (uint32_t)d->instances, -1);
    if (g_tracing && g_x_tracepass >= 0 && !d->ind && S.vbuf[0].address) {   // the first vertices stream 0 gives the draw (with the deferred updates), every word as a float
        uint32_t st = S.vstream[0].stride ? S.vstream[0].stride : 16, nw = st / 4 < 16 ? st / 4 : 16;
        for (uint32_t v = 0; v < (uint32_t)d->count && v < 4; v++) { uint64_t o = (uint64_t)((uint32_t)d->first + v) * st; if (o + nw * 4 > S.vbuf[0].size) break;
            float w[16]; vk_ubd_read(S.vbuf[0].address + o, nw * 4, (uint8_t *)w); char t[400]; int k = 0; for (uint32_t i = 0; i < nw; i++) k += snprintf(t + k, sizeof t - k, " %g", w[i]);
            VKLOG("  vertex %u of stream 0 (+%llu):%s", (uint32_t)d->first + v, (unsigned long long)o, t); } }
    if (g_var_now == 10 && prim == 7) g_ok = 0; if (g_var_now == 11 && prim != 7) g_ok = 0;   // bisection: quads from the found draw on, or the rest
    { static int nq, nn; if (g_var_now) { if (prim == 7) nq++; else nn++; if ((nq + nn) % 50 == 1) VKLOG("draws from the found one on: %d quads, %d others", nq, nn); } }
    uint32_t inst = d->instances ? (uint32_t)d->instances : 1;
    { static int n; if (n < 12 && (d->ind || (uint32_t)d->count > (1u << 20) || inst > (1u << 16))) { n++;
        VKLOG("unusual draw: count %u instances %u indirect %#llx prim %d first %d", (unsigned)d->count, inst, (unsigned long long)d->ind, d->prim, d->first); } }
    if (d->ind) {
        VkBuffer b; VkDeviceSize o;
        if (g_tf_on && S.sh[0] && S.sh[0]->xfb) VKRED_ONCE(84, "indirect draw under transform feedback: not captured");
        if (!vk_buf_at(d->ind, mcb ? (uint64_t)d->mdi_stride * (uint64_t)d->mdi_max : 16, &b, &o)) VKRED_ONCE(53, "indirect draw arguments outside the memory pools");   // (an else after the nested if once went to the inner one)
        else if (!(dbg_draw() & 1) && g_ok) { if (mcb) vkCmdDrawIndirectCount(cb, b, o, mcb, mco, (uint32_t)d->mdi_max, (uint32_t)d->mdi_stride); else vkCmdDrawIndirect(cb, b, o, 1, 16); }
        return;
    }
    if (prim == 7) {   // quads: the triangle list indices of one static buffer (built once), the first vertex as the vertex offset
        uint32_t nq = (uint32_t)d->count / 4; if (!nq) return;
        vbuf_bound(cb, (uint32_t)d->first, nq * 4, (uint32_t)d->base_instance, inst);
        if (g_tf_on && S.sh[0] && S.sh[0]->xfb) VKRED_ONCE(85, "quads under transform feedback: not captured");
        VkBuffer qb = quad_index_buffer(&nq);
        bc_index(cb, qb, 0, VK_INDEX_TYPE_UINT32); if (!(dbg_draw() & 1) && g_ok) vkCmdDrawIndexed(cb, nq * 6, inst, 0, g_var_now == 4 ? 0 : d->first, (uint32_t)d->base_instance); return; }
    if (prim == 2) VKRED_ONCE(54, "line loops drawn as line strips");
    vbuf_bound(cb, (uint32_t)d->first, (uint32_t)d->count, (uint32_t)d->base_instance, inst);
    if (g_bc_slot >= 0) {   // DrawTransformFeedback of a geometry shader's capture: its vertex count is the counter's bytes over the stream's stride
        vkshader_t *vsh = S.sh[0]; uint32_t st = 0;
        for (int loc = 0; vsh && loc < 16 && !st; loc++) if (vsh->local[0] >> loc & 1) { va_st a; memcpy(&a, &S.vattr[loc], 4); if (a.fmt) st = S.vstream[a.stream].stride; }
        if (!st) { VKRED_ONCE(92, "DrawTransformFeedback of a geometry shader's capture with no vertex stride: nothing drawn"); return; }
        // The particles' update draws last frame's capture this way while capturing this frame's (a sequence of its own): the capture goes around the draw as at any other
        if (!(dbg_draw() & 1) && g_ok) { int xf = xfb_begin(cb); vkCmdDrawIndirectByteCountEXT(cb, inst, (uint32_t)d->base_instance, g_tfc_buf, (VkDeviceSize)g_bc_slot * 16, 0, st);
            if (xf == 2) xfb_end(cb, prim, 0, inst, xf); else if (xf) { vkCmdEndTransformFeedbackEXT(cb, 0, 0, NULL, NULL); VKRED_ONCE(93, "a vertex shader's capture around a DrawTransformFeedback of a counted capture: its bytes are not counted"); }
            { static int n; if (n++ < 6) VKLOG("transform feedback: DrawTransformFeedback by the counter of control buffer %#llx, stride %u%s", (unsigned long long)g_tf_ctl[g_bc_slot].ctl, st, xf == 2 ? ", captured again" : ""); } }
        return;
    }
    if (!(dbg_draw() & 1) && g_ok) { int xf = xfb_begin(cb); vkCmdDraw(cb, (uint32_t)d->count, inst, (uint32_t)d->first, (uint32_t)d->base_instance); if (xf) xfb_end(cb, prim, (uint32_t)d->count, inst, xf); }
}
static void draw_idx(const p_draw_idx *d) {
    graphics_updates_ready();
    graphics_cpu_ready();
    uint64_t index_bytes = (uint64_t)(uint32_t)d->count * (d->itype == 0 ? 1u : d->itype == 1 ? 2u : 4u);
    if (gs_split_draw(d->prim, d->ind, d->ibuf, index_bytes)) {
        int discard = S.rast_discard;
        g_gs_split_mode = 1; S.rast_discard = 1; draw_idx(d);
        vk_end_pass(); vk_img_barrier_all(vk_cmd());
        S.rast_discard = discard;
        if (!discard) { g_gs_split_mode = 2; draw_idx(d); }
        g_gs_split_mode = 0; return;
    }
    VkBuffer mcb = VK_NULL_HANDLE; VkDeviceSize mco = 0; if (mdi_group(d->mdi_cnt, d->ind, d->mdi_i, d->mdi_max, d->mdi_stride, 20, &mcb, &mco)) return;
    int prim = d->prim;
    g_draw_binst = d->ind ? 0 : (uint32_t)d->base_instance;   // before prepare_draw (see draw)
    if (!prepare_draw(prim == 7 ? 4 : prim, 1)) return;
    VkCommandBuffer cb = vk_cmd();
    int g_ok = draw_allowed(); if (g_ok == 2 || (g_tracing && g_x_tracepass >= 0)) trace_draw("indexed draw", d->prim, (uint32_t)d->count, (uint32_t)d->instances, d->itype);
    if (d->ind && g_tracing && g_x_tracepass >= 0) { uint32_t w[5]; vk_ubd_read(d->ind, 20, (uint8_t *)w);   // the arguments as the CPU sees them now (a GPU writer's last merged copy)
        VKLOG("  indirect arguments at %#llx: count %u instances %u first index %u base vertex %d base instance %u", (unsigned long long)d->ind, w[0], w[1], w[2], (int)w[3], w[4]); }
    if (g_tracing && g_x_tracepass >= 0 && !d->ind && S.vbuf[0].address && d->ibuf) {   // the vertices of the first indices, stream 0, every word as a float (as draw does)
        uint32_t st = S.vstream[0].stride ? S.vstream[0].stride : 16, nw = st / 4 < 16 ? st / 4 : 16, isz = d->itype == 0 ? 1 : d->itype == 1 ? 2 : 4;
        for (uint32_t i = 0; i < (uint32_t)d->count && i < 6; i++) { uint8_t ix[4] = {0}; vk_ubd_read(d->ibuf + (uint64_t)i * isz, isz, ix);
            uint32_t v = isz == 1 ? ix[0] : isz == 2 ? (uint32_t)(ix[0] | ix[1] << 8) : (uint32_t)(ix[0] | ix[1] << 8 | ix[2] << 16 | (uint32_t)ix[3] << 24);
            uint64_t o = (uint64_t)((int64_t)v + d->base_vertex) * st; if (o + nw * 4 > S.vbuf[0].size) break;
            float w[16]; vk_ubd_read(S.vbuf[0].address + o, nw * 4, (uint8_t *)w); char t[400]; int k = 0; for (uint32_t j = 0; j < nw; j++) k += snprintf(t + k, sizeof t - k, " %g", w[j]);
            VKLOG("  index %u → vertex %u of stream 0 (+%llu):%s", i, v, (unsigned long long)o, t); } }
    if (g_var_now == 11) g_ok = 0;
    VkIndexType it = d->itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : d->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    uint32_t isz = d->itype == 0 ? 1 : d->itype == 1 ? 2 : 4;
    VkBuffer ib; VkDeviceSize io;
    if (!vk_buf_at(d->ibuf, 1, &ib, &io)) { VKRED_ONCE(56, "index buffer outside the memory pools"); return; }
    if (!d->ind) { vkpool_t *ip = vk_pool_of(d->ibuf, 1); uint64_t in = (uint64_t)d->count * isz; if (ip && d->ibuf + in > ip->hi && ip->hi < ip->end) vk_cross(3, d->ibuf, in, ip); }
    if (g_ubolog_key && S.sh[0] && S.sh[0]->key == g_ubolog_key && g_ubolog_n > 0) {   // DM_VK_UBO_LOG: the index data the draw reads (CPU view at replay)
        const uint8_t *ix = (const uint8_t *)(uintptr_t)d->ibuf; uint64_t h = 1469598103934665603ull; uint32_t mn = ~0u, mx = 0;
        for (uint32_t i = 0; i < (uint32_t)d->count; i++) { uint32_t v = isz == 1 ? ix[i] : isz == 2 ? ((const uint16_t *)ix)[i] : ((const uint32_t *)ix)[i]; h = (h ^ v) * 1099511628211ull; if (v < mn) mn = v; if (v > mx) mx = v; }
        VKLOG("ubo: present %llu index buffer %#llx count %d base vertex %d indirect %#llx hash %016llx range %u..%u", (unsigned long long)g_tex_frame, (unsigned long long)d->ibuf, d->count, d->base_vertex,
              (unsigned long long)d->ind, (unsigned long long)h, mn, mx);
        if (S.vbuf[0].address && mn != ~0u) { uint32_t st = S.vstream[0].stride ? S.vstream[0].stride : 12; uint64_t v0 = (uint64_t)(int64_t)d->base_vertex + mn;   // the first vertex this draw reads, every word
            if ((v0 + 1) * st <= S.vbuf[0].size) { const float *vf = (const float *)(uintptr_t)(S.vbuf[0].address + v0 * st); char t[512]; int k = 0;
                for (uint32_t i = 0; i < st / 4 && i < 24; i++) k += snprintf(t + k, sizeof t - k, " %g", vf[i]);
                VKLOG("ubo: present %llu first vertex read (%llu):%s", (unsigned long long)g_tex_frame, (unsigned long long)v0, t); } }
        static uint64_t dumped; if (dumped != h && S.vbuf[0].address && d->count <= 4096) { dumped = h; char t[1024]; int k = 0; uint32_t i0 = 0;   // once per index data: every index,
            for (uint32_t i = 0; i < (uint32_t)d->count; i++) { uint32_t v = isz == 1 ? ix[i] : isz == 2 ? ((const uint16_t *)ix)[i] : ((const uint32_t *)ix)[i];   // then every word of stream 0's vertices
                k += snprintf(t + k, sizeof t - k, " %u", v); if (k > 900 || i + 1 == (uint32_t)d->count) { VKLOG("ubo: indices from %u:%s", i0, t); k = 0; i0 = i + 1; } }
            uint32_t st = S.vstream[0].stride ? S.vstream[0].stride : 12, nv = (uint32_t)(S.vbuf[0].size / st);
            for (uint32_t i = 0; i < nv && i <= mx; i++) { const float *vf = (const float *)(uintptr_t)(S.vbuf[0].address + (uint64_t)i * st);   // every word of stream 0's vertex (up to 8)
                for (uint32_t w = 0; w < st / 4 && w < 8; w++) k += snprintf(t + k, sizeof t - k, "%s%g", w ? "," : " ", vf[w]);
                if (k > 800 || i + 1 == nv || i == mx) { VKLOG("ubo: vertices to %u:%s", i, t); k = 0; } } } }
    uint32_t inst = d->instances ? (uint32_t)d->instances : 1;
    { static int n; if (n < 12 && (d->itype == 0 || d->ind || (uint32_t)d->count > (1u << 20) || inst > (1u << 16))) { n++;
        VKLOG("unusual indexed draw: itype %d count %u instances %u indirect %#llx prim %d base vertex %d", d->itype, (unsigned)d->count, inst, (unsigned long long)d->ind, d->prim, d->base_vertex); } }
    if (d->ind) { VkBuffer b; VkDeviceSize o; bc_index(cb, ib, io, it); if (!vk_buf_at(d->ind, mcb ? (uint64_t)d->mdi_stride * (uint64_t)d->mdi_max : 20, &b, &o)) VKRED_ONCE(97, "indexed indirect draw arguments outside the memory pools");
        else if (!(dbg_draw() & 1) && g_ok) { if (mcb) vkCmdDrawIndexedIndirectCount(cb, b, o, mcb, mco, (uint32_t)d->mdi_max, (uint32_t)d->mdi_stride); else vkCmdDrawIndexedIndirect(cb, b, o, 1, 20); } return; }
    if (prim == 7) { VkBuffer b; VkDeviceSize o; int n = quad_indices((uint32_t)d->count, (const uint8_t *)(uintptr_t)d->ibuf, d->itype, 0, &b, &o); if (!n) return;
        bc_index(cb, b, o, VK_INDEX_TYPE_UINT32); if (!(dbg_draw() & 1) && g_ok) vkCmdDrawIndexed(cb, (uint32_t)n, inst, 0, d->base_vertex, (uint32_t)d->base_instance); return; }
    if (io % isz) { VKRED_ONCE(57, "index buffer offset not aligned to its index size"); return; }
    bc_index(cb, ib, io, it);
    if (!(dbg_draw() & 1) && g_ok) { int xf = xfb_begin(cb); vkCmdDrawIndexed(cb, (uint32_t)d->count, inst, 0, d->base_vertex, (uint32_t)d->base_instance); if (xf) xfb_end(cb, prim, (uint32_t)d->count, inst, xf); }
}
static nvn_hmap g_cpipes = {.klen = 24};   // {shader, its integer-texel bindings}
static pthread_mutex_t g_cpipe_mu = PTHREAD_MUTEX_INITIALIZER;   // the replay thread and compute_warm
static VkPipeline compute_build(vkshader_t *cs, uint64_t um, uint64_t im) {   // (VkPipeline)1 when it fails
    VkDescriptorSetLayout dsl[5] = {cs->dsl}; VkPipeline p;
    int bs = build_begin();   // (the crash dump's note: build_pipeline)
    if (bs >= 0) { snprintf(g_build[bs].what, sizeof g_build[bs].what, "compute pipeline, shader %016llx, integer-texel views %#llx/%#llx", (unsigned long long)cs->key, (unsigned long long)um, (unsigned long long)im); g_build[bs].phase = 1; }
    // DM_VK_SPV_ALT=1 in env.txt (with DM_VK_SPV_DIR): compute shaders take the alternative code too, from the start (compute_warm builds them all then, and they are kept)
    static int alt = -1; if (alt < 0) { const char *e = getenv("DM_VK_SPV_ALT"); alt = e && atoi(e); }
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, vk_sg_pin(VK_SHADER_STAGE_COMPUTE_BIT, cs->local), 0, VK_SHADER_STAGE_COMPUTE_BIT, vk_shader_module(cs, um, im, alt && !um && !im ? 1u << 31 : 0, 0, 0), "main", NULL}, vk_pipeline_layout(dsl, 1), VK_NULL_HANDLE, -1};   // (pNext: DM_VK_SUBGROUP_PIN)
    if (bs >= 0) g_build[bs].phase = 2;
    VkResult r = vkCreateComputePipelines(V.dev, V.pcache, 1, &ci, NULL, &p); build_end(bs);
    vk_pipeline_cache_changed();
    if (r) { LOG("[RED] [vk] compute pipeline failed: %016llx", (unsigned long long)cs->key); p = (VkPipeline)1; }
    return p;
}
// Compute pipelines made ahead. One depends only on its shader (and integer-texel variants, rare), and building one costs Adreno 45-105 ms: Super Mario Odyssey's
// particle emitters run on compute shaders (EffectPtcl), and the first cap throw at a breakable object built one on the replay thread (a 65 ms frame), the first scenes
// several. A game has few (Odyssey 41), so at the first submission a background thread builds every compute shader in the bundle with no integer-texel variant (up to a cap, below)
static void *compute_warm(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk compute warm"); setpriority(PRIO_PROCESS, 0, 10);
    int vk_shader_keys(uint32_t stage, uint64_t *out, int max); static uint64_t keys[4096]; int n = vk_shader_keys(5, keys, 4096), made = 0; uint64_t t0 = tick();
    // A bundle with many compute shaders is left to the replay: Monster Hunter Rise's (4096 or more) held ~550 MB more from the title on, and a Retroid Pocket 5
    // (Adreno 650, 8 GB) ran out of GPU memory loading the first area (vkAllocateMemory -2 at a 4 MB pool). DM_VK_COMPUTE_WARM_MAX (start-up, default 256): the most made ahead
    { const char *e = getenv("DM_VK_COMPUTE_WARM_MAX"); int cap = e ? atoi(e) : 256;
      if (n > cap) { VKLOG("compute pipelines made ahead: none, the bundle has %d%s compute shaders (more than %d, DM_VK_COMPUTE_WARM_MAX)", n, n == 4096 ? " or more" : "", cap); return NULL; } }
    for (int i = 0; i < n; i++) {
        uint64_t ck[3] = {keys[i], 0, 0};
        pthread_mutex_lock(&g_cpipe_mu); int have = nvn_hm_get(&g_cpipes, ck) != NULL; pthread_mutex_unlock(&g_cpipe_mu);
        vkshader_t *cs = have ? NULL : vk_shader(keys[i], 5); if (!cs) continue;
        VkPipeline p = compute_build(cs, 0, 0); made++;
        pthread_mutex_lock(&g_cpipe_mu); if (!nvn_hm_get(&g_cpipes, ck)) nvn_hm_put(&g_cpipes, ck, p); else if (p != (void *)1) vkDestroyPipeline(V.dev, p, NULL); pthread_mutex_unlock(&g_cpipe_mu);
    }
    VKLOG("compute pipelines made ahead: %d of %d compute shaders in %.0f ms", made, n, (tick() - t0) / 1e6);
    return NULL;
}
static void compute_warm_start(void) {
    if (getenv("DM_VK_NOPREWARM")) return;
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, compute_warm, NULL); pthread_attr_destroy(&a);
}
// Validate all three indirect argument words before recording any copy. A pool can have several VkBuffers (chunks), and a slab's
// buffer begins at boff. CPU memory is not a source: a preceding GPU command may generate these counts during this submission.
static int dispatch_indirect_spans(uint64_t addr, VkBuffer src[3], VkBufferCopy spans[3]) {
    if ((addr & 3) || addr > UINT64_MAX - 12) return 0;
    uint64_t base = 0, end = 0; int n = 0;
    for (unsigned i = 0; i < 3; i++) {
        uint64_t a = addr + 4 * i; vkpool_t *p = vk_pool_of(a, 4);
        if (!p || !p->buf || a < p->lo || a >= p->hi || p->hi - a < 4 || a < p->base || a >= p->end || p->end - a < 4) return 0;
        if (!i) { base = p->base; end = p->end; if (end - addr < 12) return 0; }
        else if (p->base != base || p->end != end) return 0;
        uint64_t off = a - p->lo;
        if (p->boff > UINT64_MAX - off) return 0;
        off += p->boff;
        if ((off & 3) || off > UINT64_MAX - 4) return 0;
        if (n && src[n - 1] == p->buf && spans[n - 1].srcOffset + spans[n - 1].size == off) spans[n - 1].size += 4;
        else { src[n] = p->buf; spans[n++] = (VkBufferCopy){off, 4 * i, 4}; }
    }
    return n;
}
static void dispatch(const p_dispatch *d) {
    vkshader_t *cs = S.sh[5];
    if (!cs) { VKRED_ONCE(60, "dispatch with no compute shader in the bundle: skipped"); return; }
    shader_cpu_ready(cs, 5);
    VkBuffer src[3]; VkBufferCopy spans[3]; int nspans = 0;
    if (d->ind && !(nspans = dispatch_indirect_spans(d->ind, src, spans))) { VKRED_ONCE(61, "indirect dispatch arguments are not a valid aligned memory pool range: %#llx", (unsigned long long)d->ind); return; }
    g_vkst.dispatches++;
    vk_end_pass_land();
    VkCommandBuffer cb = vk_cmd();
    VkDescriptorSetLayout dsl[5] = {cs->dsl}; VkPipelineLayout layout = vk_pipeline_layout(dsl, 1);
    // the group counts before the descriptors: the driver segment stage_set builds carries them at c0 + 0x200 (drv_segment), as nvn_mtl.m's does. Set after it, every
    // dispatch read the previous one's: Link's Awakening's light culling (tile = workgroup y × c0[0x200] + workgroup x) ran after a 2x1x1 dispatch, so of its 30x17
    // tiles it wrote only 0-61. Indirect counts are copied on the GPU into a fresh c0 segment below
    g_dispatch[0] = (uint32_t)d->x; g_dispatch[1] = (uint32_t)d->y; g_dispatch[2] = (uint32_t)d->z;
    VkDescriptorBufferInfo indirect_c0 = {0};
    uint32_t dofs[96]; VkDescriptorSet set = stage_set(5, cs, dofs, d->ind ? &indirect_c0 : NULL);
    uint64_t ck[3] = {cs->key, g_num_um, g_num_im};
    pthread_mutex_lock(&g_cpipe_mu); VkPipeline p = nvn_hm_get(&g_cpipes, ck); pthread_mutex_unlock(&g_cpipe_mu);
    if (!p) {
        uint64_t tb = g_prof_on ? tick() : 0; p = compute_build(cs, ck[1], ck[2]); if (g_prof_on) g_prof_ns[7] += tick() - tb;
        pthread_mutex_lock(&g_cpipe_mu); VkPipeline had = nvn_hm_get(&g_cpipes, ck);
        if (had) { if (p != (void *)1) vkDestroyPipeline(V.dev, p, NULL); p = had; } else nvn_hm_put(&g_cpipes, ck, p);   // (compute_warm made it meanwhile)
        pthread_mutex_unlock(&g_cpipe_mu);
    }
    if (p == (void *)1) return;
    vk_img_barrier_all(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, (uint32_t)cs->ndyn, dofs);
    void vk_pass_ts(int end, uint32_t w, uint32_t h, uint32_t fmt, uint32_t draws); vk_pass_ts(0, 0, 0, 0, 0);   // DM_VK_PASS_TIMES: dispatches are timed as passes
    if (d->ind) {
        VkBuffer b = indirect_c0.buffer; VkDeviceSize o = indirect_c0.offset + 0x200;
        if (!b) vk_scratch(12, 4, &b, &o);   // the shader does not read c0; the dispatch still needs one contiguous GPU record
        for (int i = 0; i < nspans; i++) { spans[i].dstOffset += o; vkCmdCopyBuffer(cb, src[i], b, 1, &spans[i]); }
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkCmdDispatchIndirect(cb, b, o);   // the same snapshot supplies both the dispatch and the shader's group counts
    }
    else vkCmdDispatch(cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    vk_pass_ts(1, (uint32_t)d->x, (uint32_t)d->y, PT_DISPATCH, (uint32_t)d->z);
    if (!g_x_nodispbar) vk_img_barrier_all(cb);   // DM_VK_X_NODISPBAR=1 (live, a cost measurement that may draw wrong): no barrier after the dispatch
    if (crumbs_on()) { crumb_t *c = crumb_cur(); if (c->n < 8) { c->k[c->n][0] = 0; c->k[c->n][1] = 0; c->k[c->n][2] = cs->key; c->n++; } crumb_mark(1); }
    if (g_x_tracepass == -2 || mdi_log_now()) {   // (DM_VK_TRACE_PASS=-2 lists the dispatches too, with their storage buffers: the addresses for a DM_CTL memdump)
        char sb[512]; int o = 0; for (int r = 0; r < 16 && o < (int)sizeof sb - 40; r++) if (S.ssbo[5][r].addr) o += snprintf(sb + o, sizeof sb - (size_t)o, " s%d %#llx+%#llx", r, (unsigned long long)S.ssbo[5][r].addr, (unsigned long long)S.ssbo[5][r].size);
        VKLOG("--- dispatch %016llx %dx%dx%d%s after pass %d;%s", (unsigned long long)cs->key, d->x, d->y, d->z, d->ind ? " (indirect)" : "", g_sub_pass - 1, o ? sb : " no storage buffers");
    }
}

