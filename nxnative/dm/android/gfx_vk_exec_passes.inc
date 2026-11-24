// ---------------- render passes ----------------
// A pass is begun lazily: begin_pass() only sets it up (pass_open 2), and the first command that must run inside it begins it (pass_commit, pass_open 1).
// Until then a clear of a whole attachment becomes that attachment's load op, so a tiler clears in tile memory instead of loading the
// attachment from memory and clearing it with a draw; a pass that is set up and never used is dropped.
static VkRenderingAttachmentInfo P_ca[8], P_da, P_sa; static VkRenderingInfo P_ri; static int P_cleared;
// NVN may bind one subresource in multiple colour slots while masking all but
// one writer. Separate Vulkan attachments would load/store the same image twice
// (MK8's full-resolution normals in slots 3/4), which can lose the writer's contents.
// Keep writable aliases and route inactive duplicates to the first writer, or
// the first slot when none writes. Distinct views/layers are never combined.
static VkImageView P_alias_views[8]; static uint8_t P_alias[8];
static void color_alias_map(uint32_t n, const VkImageView views[8], uint32_t mask, uint8_t map[8]) {
    for (uint32_t i=0;i<8;i++) map[i]=(uint8_t)i;
    for (uint32_t i=0;i<n && i<8;i++) if (views[i] && map[i]==i) {
        uint32_t keep=i;
        for (uint32_t j=0;j<n && j<8;j++) if (views[j]==views[i] && ((mask>>(4*j))&15u)) {keep=j;break;}
        for (uint32_t j=0;j<n && j<8;j++) if (j!=keep && views[j]==views[i] && !((mask>>(4*j))&15u)) map[j]=(uint8_t)keep;
    }
}
static void bind_chmask(uint32_t mask) {
    if (S.pass_open && mask!=S.chmask.mask) {
        uint8_t map[8];color_alias_map((uint32_t)S.ncol,P_alias_views,mask,map);
        if (memcmp(map,P_alias,sizeof map)) vk_end_pass();
    }
    S.chmask.mask=mask;
}

// Render passes. Adreno's driver spent about 40 µs in each vkCmdBeginRendering (a quarter of the replay thread in Xenoblade Chronicles 3's battles, 128 passes a
// frame); a VkRenderPass is made once per set of formats and load operations, and an imageless framebuffer once per set of attachment descriptions, and passes
// then begin with vkCmdBeginRenderPass (the battle's replay went from about 20 to 17.5 ms a frame). Pipelines are made against a compatible render pass (the same
// formats, every load operation LOAD). DM_VK_RP=0 (start-up): dynamic rendering
typedef struct { uint32_t ncol, cfmt[8], cload[8], dfmt, dload, sload, dstore, samples, cstore[8]; } rpkey;   // cstore: 0 (memset) STORE; DONT_CARE for slot 0's scratch (DM_VK_SLOT0_FILL)
typedef struct { VkRenderPass rp; uint32_t w, h, layers, n; struct { uint32_t usage, flags, w, h, layers, fmt, fmt2, fmt3, fmt4; } a[9]; } fbkey;   // fmt2..fmt4: a format-listed image's other formats (an imageless framebuffer names the image's whole list)
static nvn_hmap g_rps = {.klen = sizeof(rpkey)}, g_fbs = {.klen = sizeof(fbkey)}; static pthread_mutex_t g_rp_mu = PTHREAD_MUTEX_INITIALIZER;
// Without dynamic rendering (a Vulkan 1.1 / 1.2 device, V.has_dynrender) passes must be render pass objects, whatever DM_VK_RP says (vk_init refuses a device with neither)
static int rp_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_RP"); on = V.has_imageless && (!V.has_dynrender || !(e && !atoi(e))); VKLOG("passes: %s", on ? "render pass objects with imageless framebuffers" : "dynamic rendering"); } return on; }
static int fmt_has_depth(uint32_t f) { return f && f != VK_FORMAT_S8_UINT; }
static int fmt_has_stencil(uint32_t f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT; }
static VkRenderPass rp_get(const rpkey *k) {
    pthread_mutex_lock(&g_rp_mu); VkRenderPass rp = nvn_hm_get(&g_rps, k);
    if (!rp) {
        VkAttachmentDescription2 ad[9]; VkAttachmentReference2 cr[8], dr; uint32_t na = 0;
        for (uint32_t i = 0; i < k->ncol; i++) {
            if (!k->cfmt[i]) { cr[i] = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT}; continue; }
            ad[na] = (VkAttachmentDescription2){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)k->cfmt[i], (VkSampleCountFlagBits)k->samples, (VkAttachmentLoadOp)k->cload[i], (VkAttachmentStoreOp)k->cstore[i],
                VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
            cr[i] = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, na++, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT};
        }
        if (k->dfmt) {
            int hd = fmt_has_depth(k->dfmt), hs = fmt_has_stencil(k->dfmt);
            ad[na] = (VkAttachmentDescription2){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)k->dfmt, (VkSampleCountFlagBits)k->samples,
                hd ? (VkAttachmentLoadOp)k->dload : VK_ATTACHMENT_LOAD_OP_DONT_CARE, hd ? (VkAttachmentStoreOp)k->dstore : VK_ATTACHMENT_STORE_OP_DONT_CARE,
                hs ? (VkAttachmentLoadOp)k->sload : VK_ATTACHMENT_LOAD_OP_DONT_CARE, hs ? (VkAttachmentStoreOp)k->dstore : VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
            dr = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, na++, VK_IMAGE_LAYOUT_GENERAL, (hd ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (hs ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)};
        }
        VkSubpassDescription2 sd = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, NULL, 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, NULL, k->ncol, cr, NULL, k->dfmt ? &dr : NULL, 0, NULL};
        VkRenderPassCreateInfo2 ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, NULL, 0, na, ad, 1, &sd, 0, NULL, 0, NULL};
        if (vkCreateRenderPass2(V.dev, &ci, NULL, &rp)) { VKRED_ONCE(70, "vkCreateRenderPass2 failed"); rp = VK_NULL_HANDLE; } else nvn_hm_put(&g_rps, k, rp);
    }
    pthread_mutex_unlock(&g_rp_mu); return rp;
}
static VkRenderPass rp_compat(uint32_t ncol, const uint32_t *cfmt, uint32_t dfmt, uint32_t samples) {   // for pipelines: every load operation LOAD
    rpkey k; memset(&k, 0, sizeof k); k.ncol = ncol; for (uint32_t i = 0; i < ncol && i < 8; i++) { k.cfmt[i] = cfmt[i]; k.cload[i] = VK_ATTACHMENT_LOAD_OP_LOAD; }
    k.dfmt = dfmt; k.dload = k.sload = VK_ATTACHMENT_LOAD_OP_LOAD; k.dstore = VK_ATTACHMENT_STORE_OP_STORE; k.samples = samples ? samples : 1; return rp_get(&k);
}
static VkFramebuffer fb_get(const fbkey *k) {
    pthread_mutex_lock(&g_rp_mu); VkFramebuffer fb = nvn_hm_get(&g_fbs, k);
    if (!fb) {
        VkFramebufferAttachmentImageInfo ai[9]; VkFormat vf[9][4];
        for (uint32_t i = 0; i < k->n; i++) { uint32_t c[4] = {k->a[i].fmt, k->a[i].fmt2, k->a[i].fmt3, k->a[i].fmt4}, nf = 0;   // the image's list, without the unset entries (its own format first)
            for (int j = 0; j < 4; j++) if (c[j] || (j == 0 && !nf)) vf[i][nf++] = (VkFormat)c[j];
            ai[i] = (VkFramebufferAttachmentImageInfo){VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO, NULL, k->a[i].flags, k->a[i].usage, k->a[i].w, k->a[i].h, k->a[i].layers, nf, vf[i]}; }
        VkFramebufferAttachmentsCreateInfo aci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO, NULL, k->n, ai};
        VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, &aci, VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT, k->rp, k->n, NULL, k->w, k->h, k->layers};
        if (vkCreateFramebuffer(V.dev, &ci, NULL, &fb)) { VKRED_ONCE(71, "vkCreateFramebuffer failed"); fb = VK_NULL_HANDLE; } else nvn_hm_put(&g_fbs, k, fb);
    }
    pthread_mutex_unlock(&g_rp_mu); return fb;
}
// the attachments begin_pass found, for the render pass begin: each colour slot's (and the depth's) image, view, view format and layers, size at the view's level
static struct { vkimg_t *im; VkImageView v; uint32_t fmt, layers, w, h, bc_lv, bc_ly, bc_nly; VkImageUsageFlags usage; } P_att[9];   // (bc_*: a block-compressed image's stand-in, the level and layers this pass draws)

// Before a pass that goes on with a depth buffer (loads it), one texel of its depth is copied out: a transfer read makes Adreno's driver drop the buffer's
// low-resolution depth (LRZ), which it otherwise kept from earlier passes even within one command buffer. NEO's menu → field wipe drew whole 32x32 squares of
// its old frame brown for two frames (the overlay's tiles rejected); with the read, never. Copying the colour target out instead, or a barrier alone, did not help.
// Depth-only buffers too: 13 Sentinels: Aegis Rim draws its frame (206 sprites, depth test off) and then copies it to the window in one quad, both passes on one
// D32; the quad's second triangle came out as a checkerboard of stale bins in every frame (either diagonal; a command-buffer cut or this read cleared it, the
// depth attachment not loaded did not). write: the texel is also written back (unchanged), a transfer write. DM_VK_LRZ_TOUCH=0 (a DM_CTL "env" line) leaves both touches out
static void lrz_touch(vkimg_t *im, int write) {
    static VkBuffer b; static VkDeviceMemory m;
    if (!im || !(im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || im->samples > 1) return;   // (multisampled images cannot be copied to a buffer)
    if (!b) { VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &b)); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, b, &rq);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
        VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &m)); VKCHK(vkBindBufferMemory(V.dev, b, m, 0)); }
    VkCommandBuffer cb = vk_cmd();
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}};
    if ((g_lrz_touch & LRZ_CORNER) && im->w && im->h) c.imageOffset = (VkOffset3D){(int32_t)im->w - 1, (int32_t)im->h - 1, 0};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c); vk_img_barrier_all(cb);   // (begin_pass's barrier already ordered it after the writes)
    if (write) { vkCmdCopyBufferToImage(cb, b, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c); vk_img_barrier_all(cb); }
}
// The depth image of the last pass that had one. When a pass draws with another, that one gets the texel written back: Adreno otherwise applied its LRZ in later
// passes on other depth buffers and rejected whole triangles there. Xenoblade Chronicles 3's cloud layer (one 720-index dome drawn into a 200x112 target, its D32
// depth downsampled from the scene's 776x440 D24S8 by a shader writing gl_FragDepth) lost whole triangles while the camera turned and kept losing them with the
// camera still at some angles: a primitive-ID dump showed whole triangles missing, triangles reaching past the screen edge kept, the same with the dome's depth
// forced to a constant; the dome drawn with gl_FragDepth written (no LRZ for it) lost none in 47 frames. Reading the dome's depth buffer before its pass (the
// read above), or writing it back, did not help; writing back every depth buffer before every pass that loads one did (0 broken frames of 53 against 17 of 46,
// GPU +1 ms), as did writing back only the one a pass switches away from (0 genuine drops in about 140 frames, GPU +0.6-0.8 ms of 13-18)
static vkimg_t *g_lrz_prev;
void vk_lrz_forget(vkimg_t *im) { if (g_lrz_prev == im) g_lrz_prev = NULL; }   // an image dropped (gfx_vk_tex.c img_drop)
// 0: no pass could be begun (no render pass or framebuffer, and no dynamic rendering to fall back on): the caller records nothing that needs one
static int pass_commit(void) {
    if (S.pass_open != 2) return S.pass_open == 1;
    vkimg_t *dim = S.daspect ? P_att[8].im : NULL;
    if ((g_lrz_touch & LRZ_SWITCH) && dim && g_lrz_prev && g_lrz_prev != dim) lrz_touch(g_lrz_prev, 1);
    if (dim) g_lrz_prev = dim;
    if ((g_lrz_touch & LRZ_READ) && dim && P_da.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) lrz_touch(dim, 0);
    void vk_pass_ts(int end, uint32_t w, uint32_t h, uint32_t fmt, uint32_t draws); vk_pass_ts(0, 0, 0, 0, 0);
    if (rp_on()) {
        rpkey k; memset(&k, 0, sizeof k); k.ncol = P_ri.colorAttachmentCount; k.samples = S.samples ? S.samples : 1;
        fbkey fk; memset(&fk, 0, sizeof fk); VkImageView views[9]; VkClearValue cv[9]; uint32_t na = 0;
        for (uint32_t i = 0; i < k.ncol; i++) if (P_ca[i].imageView) {
            k.cfmt[i] = P_att[i].fmt; k.cload[i] = P_ca[i].loadOp; k.cstore[i] = P_ca[i].storeOp; cv[na] = P_ca[i].clearValue; views[na] = P_ca[i].imageView;
            // Imageless framebuffer usage matches the narrowed view (VUID 04627).
            fk.a[na].usage = P_att[i].usage; fk.a[na].flags = P_att[i].im->cflags; fk.a[na].w = P_att[i].w; fk.a[na].h = P_att[i].h; fk.a[na].layers = P_att[i].layers;
            fk.a[na].fmt = P_att[i].im->fixed_fmt ? (uint32_t)P_att[i].im->fmt : P_att[i].fmt; fk.a[na].fmt2 = (uint32_t)P_att[i].im->fmt2;
            fk.a[na].fmt3 = (uint32_t)P_att[i].im->fmt_cls[0]; fk.a[na].fmt4 = (uint32_t)P_att[i].im->fmt_cls[1]; na++; }   // (a format-listed image (DM_VK_UBWC): its whole list, as imageless framebuffers require)
        if (P_ri.pDepthAttachment || P_ri.pStencilAttachment) {
            k.dfmt = P_att[8].fmt; k.dload = P_da.loadOp; k.sload = P_sa.imageView ? P_sa.loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE; k.dstore = P_da.storeOp;
            cv[na] = P_da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? P_da.clearValue : P_sa.clearValue; if (P_sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) cv[na].depthStencil.stencil = P_sa.clearValue.depthStencil.stencil;
            views[na] = P_da.imageView;
            fk.a[na].usage = P_att[8].usage; fk.a[na].flags = P_att[8].im->cflags; fk.a[na].w = P_att[8].w; fk.a[na].h = P_att[8].h; fk.a[na].layers = P_att[8].layers; fk.a[na].fmt = P_att[8].fmt; na++; }
        VkRenderPass rp = rp_get(&k);
        fk.rp = rp_compat(k.ncol, k.cfmt, k.dfmt, k.samples); fk.w = P_ri.renderArea.extent.width; fk.h = P_ri.renderArea.extent.height; fk.layers = P_ri.layerCount; fk.n = na;
        VkFramebuffer fb = rp && fk.rp ? fb_get(&fk) : VK_NULL_HANDLE;
        if (fb) { VkRenderPassAttachmentBeginInfo abi = {VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO, NULL, na, views};
            VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, &abi, rp, fb, P_ri.renderArea, na, cv};
            vkCmdBeginRenderPass(vk_cmd(), &rbi, VK_SUBPASS_CONTENTS_INLINE); S.rp_pass = 1; }
        else if (V.has_dynrender) { vkCmdBeginRendering(vk_cmd(), &P_ri); S.rp_pass = 0; }
        else { VKRED_ONCE(76, "render pass or framebuffer failed without dynamic rendering: pass skipped"); return 0; }   // (still pass_open 2: vk_end_pass drops it)
    } else { vkCmdBeginRendering(vk_cmd(), &P_ri); S.rp_pass = 0; }
    S.pass_open = 1; g_vkst.passes++; g_sub_pass++; g_pass_wmem = 0;
    for (int i = 0; i < 9; i++) { vkimg_t *a = P_att[i].im; if (a && a->rt_last != g_tex_frame + 1) { a->rt_frames = a->rt_last == g_tex_frame ? a->rt_frames + 1 : 1; a->rt_last = g_tex_frame + 1; } }   // (async_pipe)
    { extern uint64_t g_cmd_gen; g_cmd_gen++; }   // the bind cache starts empty in each pass: Adreno does not keep the state a pass's draws set for the next pass (XC3 drew its lighting black)
    if (g_x_tracepass == -2) { g_x_listpasses = 1; list_reads("before it, dispatches");
        char t[512]; int n = 0; static const char LO[] = "LCD";   // load ops: Load, Clear, Don't care; each attachment's image by pool offset
        for (uint32_t i = 0; i < P_ri.colorAttachmentCount; i++) if (P_ca[i].imageView && P_att[i].im) n += snprintf(t + n, sizeof t - n, " c%u=%llx:%c(%ux%u)", i, (unsigned long long)P_att[i].im->off, LO[P_ca[i].loadOp < 3 ? P_ca[i].loadOp : 2], P_att[i].im->w, P_att[i].im->h);
        if (P_ri.pDepthAttachment && P_att[8].im) n += snprintf(t + n, sizeof t - n, " d=%llx:%c(%ux%u)", (unsigned long long)P_att[8].im->off, LO[P_da.loadOp < 3 ? P_da.loadOp : 2], P_att[8].im->w, P_att[8].im->h);
        VKLOG("--- pass %d: %ux%u, colour formats %d %d, depth format %d;%s", g_sub_pass - 1, S.pass_w, S.pass_h, S.cfmt[0], S.cfmt[1], S.dfmt, t); }
    if (g_x_traceframe[0]) {   // DM_VK_TRACE_FRAME=WxH[:fmt[:fmt]] (live): trace every pass of the next submission whose targets match (a whole effect chain in one log)
        char b[48]; snprintf(b, sizeof b, "%ux%u:%d:%d", S.pass_w, S.pass_h, S.cfmt[0], S.cfmt[1]);
        g_x_tracepass = !strncmp(b, g_x_traceframe, strlen(g_x_traceframe)) ? g_sub_pass - 1 : -1; g_x_traceframe_on = 1; }
    if (g_x_tracesize[0] && g_x_tracepass < 0) {   // DM_VK_TRACE_SIZE=WxH[:format[:format]] (live): trace the next pass whose first colour target is that (passes are numbered anew each frame)
        char b[48]; snprintf(b, sizeof b, "%ux%u:%d:%d", S.pass_w, S.pass_h, S.cfmt[0], S.cfmt[1]);
        const char *h = strchr(g_x_tracesize, '#'); size_t l = h ? (size_t)(h - g_x_tracesize) : strlen(g_x_tracesize);   // #n: the (n+1)th such pass
        if (!strncmp(b, g_x_tracesize, l)) { if (h && atoi(h + 1) > 0) { snprintf(g_x_tracesize + l, sizeof g_x_tracesize - l, "#%d", atoi(h + 1) - 1); setenv("DM_VK_TRACE_SIZE", g_x_tracesize, 1); }
            else { g_x_tracepass = g_sub_pass - 1; g_x_tracesize[0] = 0; setenv("DM_VK_TRACE_SIZE", "", 1); } } }
    if (g_x_tracepass >= 0 && g_sub_pass - 1 == g_x_tracepass) VKLOG("=== traced pass %d: %ux%u, colour formats %d %d, depth format %d", g_x_tracepass, S.pass_w, S.pass_h, S.cfmt[0], S.cfmt[1], S.dfmt); g_pass_draws = 0; g_list_vs = g_list_fs = 0; g_pass_bb[0] = g_pass_bb[1] = INT32_MAX; g_pass_bb[2] = g_pass_bb[3] = INT32_MIN;
    return 1;
}
void vk_ubd_land(VkCommandBuffer cb); int vk_ubd_shadow(uint64_t a, uint64_t n, VkBuffer *buf, VkDeviceSize *off); void vk_ubd_read(uint64_t a, uint64_t n, uint8_t *dst);
// DM_VK_PASSDUMP=1 (a DM_CTL "env" line; =N waits N - 1 presents first): the next submissions' passes each leave their first colour target in a buffer (up to 48 passes), written as
// DM_ROOT/pass_NN.ppm when the GPU is done (RGBA8 as is, RGBA16F clamped to 0..1, with its NaN / infinite pixel count logged)
// DM_VK_PASSDUMP_SKIP=n (read when a dump starts): leave out each submission's first n passes, to reach a frame's later passes (Xenoblade Chronicles 3's title frame has about 200);
// =WxH: start at the first pass of that size (pass numbers move by tens from frame to frame)
// DM_VK_PASSDUMP_SIZE=WxH[,WxH...] (read when a dump starts): only passes whose first colour target has one of these sizes, over every submission of the frame (no depth),
// to follow one effect's targets when the pass numbers move from frame to frame (Xenoblade Chronicles 3's 384x216 clouds)
// DM_VK_PASSDUMP_FRAMES=N (read when a dump starts): keep dumping for N presents (up to 96 dumps in all), to catch an effect that is wrong in single frames
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; uint32_t w, h; VkFormat f; uint64_t fence; int pass; uint64_t present; } pdump_t;
enum { PD_MAX = 96 };
static pdump_t g_pd[PD_MAX]; static int g_npd, g_pd_state, g_pd_skip, g_pd_frames; static char g_pd_size[128], g_pd_from[32];   // state 0 off, 1 dumping this submission's passes, 2 waiting to write
static void pass_dump_depth(void);
static void texdump_flush(void);
static void pass_dump(void) {
    texdump_flush();
    if (g_pd_state != 1) return;   // no dump armed: nothing below does anything then, and its three getenv calls at every pass's end were about 1% of the replay thread
    { const char *fs = getenv("DM_VK_PASSDUMP_FS"); if (g_pd_state == 1 && fs && *fs) { int hit = 0; for (const char *q = fs; *q;) { char *e; uint64_t key = strtoull(q, &e, 16); if (e == q) break; if (key == g_prev_fs && g_pass_draws) hit = 1; q = *e ? e + 1 : e; } if (!hit) return; } }
    if (g_pd_state == 1 && g_sub_pass - 1 < g_pd_skip) return;   // (a submission with fewer passes dumps nothing, and the next one is tried)
    { const char *pp = getenv("DM_VK_PASSDUMP_PASS"); if (g_pd_state == 1 && pp && *pp) {   // DM_VK_PASSDUMP_PASS=n[,n...] (live): only these pass numbers of each submission,
        int hit = 0; for (const char *q = pp; *q;) { char *e; long v = strtol(q, &e, 10); if (e == q) break; if (v == g_sub_pass - 1) hit = 1; q = *e ? e + 1 : e; }   // to follow a few passes
        if (!hit) return; } }                                                                                                                                       // over many frames
    { const char *pf = getenv("DM_VK_PASSDUMP_FMT"); int a, b;   // DM_VK_PASSDUMP_FMT=a/b (live): only passes whose first two colour formats are VkFormat a and b (pass numbers move)
      if (g_pd_state == 1 && pf && sscanf(pf, "%d/%d", &a, &b) == 2 && ((int)S.cfmt[0] != a || (int)S.cfmt[1] != b)) return; }
    if (g_pd_state == 1 && g_pd_from[0]) { char wh[32]; snprintf(wh, sizeof wh, "%ux%u", S.pass_w, S.pass_h); if (strcmp(wh, g_pd_from)) return; g_pd_from[0] = 0; }
    if (!g_pd_size[0]) pass_dump_depth();
    int ci = 0; while (ci < S.ncol && ci < 8 && !S.rt_has_col[ci]) ci++;   // the first colour target bound (Pokémon Legends: Arceus's G-buffer leaves colour 0 unbound)
    const char *pt = getenv("DM_VK_PASSDUMP_TARGET"); if (pt && *pt && atoi(pt) >= 0 && atoi(pt) < 8 && S.rt_has_col[atoi(pt)]) ci = atoi(pt);   // =n: colour target n where the pass has one
    if (g_pd_state != 1 || g_npd >= PD_MAX || !S.ncol || ci >= S.ncol || ci >= 8) return;
    vkimg_t *im = vk_tex(&S.rt_col[ci]);
    if (im && g_pd_size[0]) { char wh[32]; snprintf(wh, sizeof wh, "%ux%u", im->w, im->h); const char *f = strstr(g_pd_size, wh); size_t l = strlen(wh);
        if (!f || (f != g_pd_size && f[-1] != ',') || (f[l] && f[l] != ',')) return;
        const char *dd = getenv("DM_VK_PASSDUMP_DEPTH"); if (dd && atoi(dd)) pass_dump_depth(); }   // DM_VK_PASSDUMP_DEPTH=1: the size-filtered passes' depth too
    if (!im || (im->fmt != VK_FORMAT_R8G8B8A8_UNORM && im->fmt != VK_FORMAT_R8G8B8A8_SRGB && im->fmt != VK_FORMAT_R16G16B16A16_SFLOAT && im->fmt != VK_FORMAT_B8G8R8A8_UNORM &&
                                                    im->fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && im->fmt != VK_FORMAT_R16_SFLOAT && im->fmt != VK_FORMAT_R8_UNORM && im->fmt != VK_FORMAT_A2B10G10R10_UNORM_PACK32 && im->fmt != VK_FORMAT_R16G16_SFLOAT)) return;
    const view_t *cv = S.rt_has_cview[ci] ? &S.rt_cview[ci] : NULL;   // the view's level and layer (a cube face of an array, a mip)
    uint32_t lv = cv && cv->set_levels ? cv->base_level : 0, ly = cv && cv->set_layers ? cv->min_layer : 0, w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1;
    if (lv >= im->levels || ly >= im->layers) lv = ly = 0, w = im->w, h = im->h;
    uint32_t ox = 0, oy = 0; const char *crop = getenv("DM_VK_PASSDUMP_CROP"); unsigned cx, cy, cw, ch;
    if (crop && sscanf(crop, "%u,%u,%u,%u", &cx, &cy, &cw, &ch) == 4 && cx < w && cy < h && cw && ch) { ox = cx; oy = cy; w = cw < w - cx ? cw : w - cx; h = ch < h - cy ? ch : h - cy; VKLOG("pass dump crop: image %ux%u, origin %u,%u, size %ux%u", im->w, im->h, ox, oy, w, h); }
    uint32_t bpp = im->fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : im->fmt == VK_FORMAT_R16_SFLOAT ? 2 : im->fmt == VK_FORMAT_R8_UNORM ? 1 : 4; VkDeviceSize n = (VkDeviceSize)w * h * bpp;
    pdump_t *d = &g_pd[g_npd++]; d->w = w; d->h = h; d->f = im->fmt; d->pass = g_sub_pass - 1; d->present = g_tex_frame;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &d->b)); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, d->b, &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0)};
    VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &d->m)); VKCHK(vkBindBufferMemory(V.dev, d->b, d->m, 0)); VKCHK(vkMapMemory(V.dev, d->m, 0, VK_WHOLE_SIZE, 0, &d->p));
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1}, {(int32_t)ox, (int32_t)oy, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, d->b, 1, &c); vk_img_barrier_all(cb);
}
static void texdump_flush(void) {   // DM_VK_TEXDUMP: outside a render pass (pass_dump runs after its end)
    vkimg_t *im = g_td_im; if (!im || g_pd_state != 1) return; g_td_im = NULL; g_td_done = 1;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(V.pd, im->fmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) { VKLOG("texdump: format %d cannot be blitted", im->fmt); return; }
    const char *tl = getenv("DM_VK_TEXDUMP_LEVEL"); uint32_t lv = tl && *tl ? (uint32_t)atoi(tl) : 0; if (lv >= im->levels) lv = im->levels - 1;   // =n: that mip level instead of level 0
    uint32_t w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1;
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    for (uint32_t ly = 0; ly < im->layers && ly < 6 && g_npd < PD_MAX; ly++) {
        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage ti; VKCHK(vkCreateImage(V.dev, &ic, NULL, &ti)); VkMemoryRequirements rq; vkGetImageMemoryRequirements(V.dev, ti, &rq);
        VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
        VkDeviceMemory tm; VKCHK(vkAllocateMemory(V.dev, &ma, NULL, &tm)); VKCHK(vkBindImageMemory(V.dev, ti, tm, 0));
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ti, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkImageBlit r = {{VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1}, {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
        vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, ti, VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_NEAREST); vk_img_barrier_all(cb);
        pdump_t *d = &g_pd[g_npd++]; d->w = w; d->h = h; d->f = VK_FORMAT_R16G16B16A16_SFLOAT; d->pass = -100 - (int)ly; d->present = g_tex_frame;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)w * h * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &d->b)); vkGetBufferMemoryRequirements(V.dev, d->b, &rq);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0)};
        VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &d->m)); VKCHK(vkBindBufferMemory(V.dev, d->b, d->m, 0)); VKCHK(vkMapMemory(V.dev, d->m, 0, VK_WHOLE_SIZE, 0, &d->p));
        VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, ti, VK_IMAGE_LAYOUT_GENERAL, d->b, 1, &c); vk_img_barrier_all(cb);
        vk_defer_free_image(ti, tm);
    }
    VKLOG("texdump: %ux%u fmt %d layers %u levels %u (level %u dumped) at pool %#llx+%#llx: dumps with pass -100 - layer", im->w, im->h, im->fmt, im->layers, im->levels, lv, (unsigned long long)im->pool, (unsigned long long)im->off);
}
static void pass_dump_depth(void) {   // the pass's depth (D24S8 / D32 / D16) as a pdump_t with f = the depth format; written as a grayscale stretched between its min and max
    if (g_pd_state != 1 || g_npd >= PD_MAX || !S.rt_has_dep) return;
    vkimg_t *im = vk_tex(&S.rt_dep); if (!im || !(im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    uint32_t bpp = im->fmt == VK_FORMAT_D16_UNORM ? 2 : 4; VkDeviceSize n = (VkDeviceSize)im->w * im->h * bpp;
    pdump_t *d = &g_pd[g_npd++]; d->w = im->w; d->h = im->h; d->f = im->fmt; d->pass = g_sub_pass - 1; d->present = g_tex_frame;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &d->b)); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, d->b, &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0)};
    VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &d->m)); VKCHK(vkBindBufferMemory(V.dev, d->b, d->m, 0)); VKCHK(vkMapMemory(V.dev, d->m, 0, VK_WHOLE_SIZE, 0, &d->p));
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {im->w, im->h, 1}};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, d->b, 1, &c); vk_img_barrier_all(cb);
}
static inline float half2f(uint16_t h) { uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, f;
    if (!e) { if (!m) f = s; else { e = 1; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; f = s | ((e + 112) << 23) | (m << 13); } } else if (e == 31) f = s | 0x7f800000 | (m << 13); else f = s | ((e + 112) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r; }
static void pixel_history_poll(uint64_t done_fence);
static int pixel_history_busy(void);
void vk_pass_dump_poll(uint64_t done_fence) {   // gfx_vk_present.c, every present: start on the switch, write once the GPU finished
    pixel_history_poll(done_fence);
    if (pixel_history_busy()) return;
    const char *e = getenv("DM_VK_PASSDUMP");
    if (g_pd_state == 0 && e && atoi(e) > 1) { char n[16]; snprintf(n, sizeof n, "%d", atoi(e) - 1); setenv("DM_VK_PASSDUMP", n, 1); return; }   // =N: the frame after N - 1 more presents
    if (g_pd_state == 0 && e && atoi(e)) { const char *sk = getenv("DM_VK_PASSDUMP_SKIP"); g_pd_skip = sk && !strchr(sk, 'x') ? atoi(sk) : 0;
        snprintf(g_pd_from, sizeof g_pd_from, "%s", sk && strchr(sk, 'x') ? sk : "");
        const char *sz = getenv("DM_VK_PASSDUMP_SIZE"); snprintf(g_pd_size, sizeof g_pd_size, "%s", sz && strcmp(sz, "0") ? sz : "");
        { const char *td = getenv("DM_VK_TEXDUMP"); const char *pl = td ? strchr(td, '+') : NULL; g_td_pool = pl ? strtoull(td, NULL, 16) : 0; g_td_off = pl ? strtoll(pl + 1, NULL, 16) : -1; g_td_im = NULL; g_td_done = 0; }
        const char *fr = getenv("DM_VK_PASSDUMP_FRAMES"); g_pd_frames = fr && atoi(fr) > 1 ? atoi(fr) : 1;
        g_pd_state = 1; g_npd = 0; setenv("DM_VK_PASSDUMP", "0", 1); VKLOG("pass dump: the next submission's passes from pass %d", g_pd_skip); return; }
    if (g_pd_state == 1 && g_npd && --g_pd_frames > 0 && g_npd < PD_MAX - 8) return;   // DM_VK_PASSDUMP_FRAMES=N: N presents in a row (with DM_VK_PASSDUMP_SIZE)
    if (g_pd_state == 1 && g_npd) { g_pd_state = 2; for (int i = 0; i < g_npd; i++) g_pd[i].fence = mtl_last_fence() + 1; return; }
    if (g_pd_state != 2 || !g_npd || done_fence < g_pd[g_npd - 1].fence) return;
    const char *root = getenv("DM_ROOT");
    for (int i = 0; i < g_npd; i++) {
        pdump_t *d = &g_pd[i]; VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, d->m, 0, VK_WHOLE_SIZE};
        VKCHK(vkInvalidateMappedMemoryRanges(V.dev, 1, &mr));
        char path[1024]; snprintf(path, sizeof path, "%s/pass_%02d.ppm", root ? root : ".", i);
        VKLOG("pass dump %02d is the submission's pass %d (present %llu)", i, d->pass, (unsigned long long)d->present);
        FILE *f = fopen(path, "wb"); if (!f) continue; fprintf(f, "P6\n%u %u\n255\n", d->w, d->h);
        uint8_t *row = malloc((size_t)d->w * 3); uint64_t bad = 0, n_nan = 0, n_inf = 0, n_finite = 0; float mx = 0, mn = 0; double sum = 0;
        if (d->f == VK_FORMAT_D24_UNORM_S8_UINT || d->f == VK_FORMAT_D32_SFLOAT || d->f == VK_FORMAT_D32_SFLOAT_S8_UINT || d->f == VK_FORMAT_D16_UNORM || d->f == VK_FORMAT_X8_D24_UNORM_PACK32) {
            size_t np = (size_t)d->w * d->h; float lo = 1e30f, hi = -1e30f; float *v = malloc(np * sizeof *v);
            for (size_t k = 0; k < np; k++) { float x;
                if (d->f == VK_FORMAT_D16_UNORM) x = ((const uint16_t *)d->p)[k] / 65535.f;
                else if (d->f == VK_FORMAT_D32_SFLOAT || d->f == VK_FORMAT_D32_SFLOAT_S8_UINT) memcpy(&x, (const uint8_t *)d->p + 4 * k, 4);
                else x = (((const uint32_t *)d->p)[k] & 0xffffff) / 16777215.f;
                v[k] = x; if (x < lo) lo = x; if (x > hi) hi = x; }
            for (uint32_t y = 0; y < d->h; y++) { for (uint32_t x = 0; x < d->w; x++) { float t = hi > lo ? (v[(size_t)y * d->w + x] - lo) / (hi - lo) : 0; row[3 * x] = row[3 * x + 1] = row[3 * x + 2] = (uint8_t)(t * 255); } fwrite(row, 1, (size_t)d->w * 3, f); }
            { char rp[1100]; snprintf(rp, sizeof rp, "%s.f32", path); FILE *rf = fopen(rp, "wb"); if (rf) { fwrite(v, sizeof *v, np, rf); fclose(rf); } }   // the values themselves
            free(v); free(row); fclose(f); VKLOG("pass dump %02d: depth %ux%u format %d, values %.6f .. %.9g → %s", i, d->w, d->h, d->f, lo, hi, path);
            vkDestroyBuffer(V.dev, d->b, NULL); vkFreeMemory(V.dev, d->m, NULL); continue;
        }
        if (d->f == VK_FORMAT_R16G16_SFLOAT) {   // signed two-channel fields (velocities): red and green around grey, 1/16 a step; the values themselves in .f32 (r, g a texel)
            size_t np = (size_t)d->w * d->h; float *v = malloc(np * 2 * sizeof *v), lo = 0, hi = 0; uint64_t nz = 0;
            for (size_t k = 0; k < np * 2; k++) { v[k] = half2f(((const uint16_t *)d->p)[k]); if (v[k] < lo) lo = v[k]; if (v[k] > hi) hi = v[k]; }
            for (uint32_t y = 0; y < d->h; y++) { for (uint32_t x = 0; x < d->w; x++) { const float *t = v + 2 * ((size_t)y * d->w + x); int on = t[0] != 0 || t[1] != 0; nz += on;
                for (int c = 0; c < 2; c++) { float q = 128 + t[c] * 16; row[3 * x + c] = (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q); } row[3 * x + 2] = on ? 255 : 128; }
                fwrite(row, 1, (size_t)d->w * 3, f); }
            { char rp[1100]; snprintf(rp, sizeof rp, "%s.f32", path); FILE *rf = fopen(rp, "wb"); if (rf) { fwrite(v, sizeof *v, np * 2, rf); fclose(rf); } }
            free(v); free(row); fclose(f); VKLOG("pass dump %02d: %ux%u RG16F, values %.3f .. %.3f, nonzero texels %llu → %s", i, d->w, d->h, lo, hi, (unsigned long long)nz, path);
            vkDestroyBuffer(V.dev, d->b, NULL); vkFreeMemory(V.dev, d->m, NULL); continue;
        }
        const char *raw_env = getenv("DM_VK_PASSDUMP_RAW"); float *raw = raw_env && atoi(raw_env) && (d->f == VK_FORMAT_R16G16B16A16_SFLOAT || d->f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || d->f == VK_FORMAT_R16_SFLOAT) ? malloc((size_t)d->w * d->h * 3 * sizeof(float)) : NULL;
        for (uint32_t y = 0; y < d->h; y++) { for (uint32_t x = 0; x < d->w; x++) {
            if (d->f == VK_FORMAT_R16G16B16A16_SFLOAT || d->f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || d->f == VK_FORMAT_R16_SFLOAT) { float v3[3];
                if (d->f == VK_FORMAT_R16G16B16A16_SFLOAT) { const uint16_t *p = (const uint16_t *)d->p + ((size_t)y * d->w + x) * 4; for (int c = 0; c < 3; c++) v3[c] = half2f(p[c]); }
                else if (d->f == VK_FORMAT_R16_SFLOAT) v3[0] = v3[1] = v3[2] = half2f(((const uint16_t *)d->p)[(size_t)y * d->w + x]);
                else { uint32_t w = ((const uint32_t *)d->p)[(size_t)y * d->w + x];   // 11-bit and 10-bit floats: a half's exponent and top mantissa bits
                    v3[0] = half2f((uint16_t)((w & 0x7ff) << 4)); v3[1] = half2f((uint16_t)((w >> 11 & 0x7ff) << 4)); v3[2] = half2f((uint16_t)((w >> 22 & 0x3ff) << 5)); }
                for (int c = 0; c < 3; c++) { float v = v3[c]; uint32_t bits; memcpy(&bits, &v, 4); if (raw) raw[3 * ((size_t)y * d->w + x) + c] = v;
                    if ((bits & 0x7f800000u) == 0x7f800000u) { bad++; if (bits & 0x7fffffu) n_nan++; else n_inf++; v = 1; }
                    else { if (!n_finite || v > mx) mx = v; if (!n_finite || v < mn) mn = v; sum += v; n_finite++; }
                    row[3 * x + c] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255 + .5f); } }
            else if (d->f == VK_FORMAT_R8_UNORM) row[3 * x] = row[3 * x + 1] = row[3 * x + 2] = ((const uint8_t *)d->p)[(size_t)y * d->w + x];
            else if (d->f == VK_FORMAT_A2B10G10R10_UNORM_PACK32) { uint32_t w = ((const uint32_t *)d->p)[(size_t)y * d->w + x]; for (int c = 0; c < 3; c++) row[3 * x + c] = (uint8_t)((w >> (10 * c) & 0x3ff) >> 2); }
            else { const uint8_t *p = (const uint8_t *)d->p + ((size_t)y * d->w + x) * 4; int bgr = d->f == VK_FORMAT_B8G8R8A8_UNORM; row[3 * x] = p[bgr ? 2 : 0]; row[3 * x + 1] = p[1]; row[3 * x + 2] = p[bgr ? 0 : 2]; } }
            fwrite(row, 1, (size_t)d->w * 3, f); }
        free(row); fclose(f);
        if (raw) { char rp[1100]; snprintf(rp, sizeof rp, "%s.f32", path); FILE *rf = fopen(rp, "wb");
            if (rf) { size_t n = (size_t)d->w * d->h * 3, written = fwrite(raw, sizeof(float), n, rf); int closed = fclose(rf);
                if (written == n && !closed) VKLOG("pass dump %02d RGB32F: %ux%u -> %s", i, d->w, d->h, rp);
                else VKLOG("[RED] incomplete RGB32F dump %s", rp); }
            else VKLOG("[RED] cannot write RGB32F dump %s", rp); free(raw); }
        VKLOG("pass dump %02d: %ux%u format %d → %s%s", i, d->w, d->h, d->f, path, d->f == VK_FORMAT_R16G16B16A16_SFLOAT ? "" : "");
        if (d->f == VK_FORMAT_R16G16B16A16_SFLOAT || d->f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || d->f == VK_FORMAT_R16_SFLOAT) {
            if (n_finite) VKLOG("   NaN/inf channels %llu (NaN %llu, inf %llu), finite values %.3f .. %.3f, mean %.5f", (unsigned long long)bad, (unsigned long long)n_nan, (unsigned long long)n_inf, mn, mx, sum / (double)n_finite);
            else VKLOG("   NaN/inf channels %llu (NaN %llu, inf %llu), no finite values", (unsigned long long)bad, (unsigned long long)n_nan, (unsigned long long)n_inf); }
        if (d->f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && d->w * d->h <= 4) { char t[200]; int k = 0;   // a tiny target (an exposure value): every texel
            for (uint32_t q = 0; q < d->w * d->h; q++) { uint32_t w = ((const uint32_t *)d->p)[q];
                k += snprintf(t + k, sizeof t - k, " (%g %g %g)", half2f((uint16_t)((w & 0x7ff) << 4)), half2f((uint16_t)((w >> 11 & 0x7ff) << 4)), half2f((uint16_t)((w >> 22 & 0x3ff) << 5))); }
            VKLOG("   texels:%s", t); }
        vkDestroyBuffer(V.dev, d->b, NULL); vkFreeMemory(V.dev, d->m, NULL);
    }
    g_npd = 0; g_pd_state = 0;
}
// DM_VK_CRUMBS=1 (start-up), for a device lost (a GPU hang): every pass end and dispatch writes {submission, pass} into host-visible memory after a full barrier, and the host
// keeps the shaders each pass drew with; when the device is lost (vk_crumbs_report), the last work the GPU finished and the passes after it are logged
static int g_crumbs = -1; static VkBuffer g_crumb_buf; static volatile uint32_t *g_crumb;
typedef struct { uint32_t sub; int pass, n; uint64_t k[32][3]; } crumb_t; static crumb_t g_cring[512]; static unsigned g_cn;
static int crumbs_on(void) {
    if (g_crumbs < 0) { const char *e = getenv("DM_VK_CRUMBS"); g_crumbs = e && atoi(e);
        if (g_crumbs) { VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
            VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &g_crumb_buf)); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, g_crumb_buf, &rq);
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
            VkDeviceMemory m; VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &m)); VKCHK(vkBindBufferMemory(V.dev, g_crumb_buf, m, 0)); void *p; VKCHK(vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &p)); g_crumb = p; g_crumb[0] = 0; } }
    return g_crumbs;
}
static crumb_t *crumb_cur(void) { crumb_t *c = &g_cring[(g_cn ? g_cn - 1 : 0) % 512]; uint32_t sub = (uint32_t)mtl_last_fence() + 1;
    if (!g_cn || c->sub != sub || c->pass != g_sub_pass - 1) { c = &g_cring[g_cn++ % 512]; memset(c, 0, sizeof *c); c->sub = sub; c->pass = g_sub_pass - 1; } return c; }
static void crumb_draw(uint64_t a, uint64_t b, uint64_t c3) { if (!crumbs_on()) return; crumb_t *c = crumb_cur();
    for (int i = 0; i < c->n; i++) if (c->k[i][0] == a && c->k[i][1] == b && c->k[i][2] == c3) return; if (c->n < 32) { c->k[c->n][0] = a; c->k[c->n][1] = b; c->k[c->n][2] = c3; c->n++; } }
static void crumb_mark(int dispatch) { if (!crumbs_on()) return; VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    vkCmdFillBuffer(cb, g_crumb_buf, 0, 4, ((uint32_t)(mtl_last_fence() + 1) & 0xffff) << 16 | (dispatch ? 0x8000u : 0) | ((uint32_t)(g_sub_pass - 1) & 0x7fff)); }
void vk_crumbs_report(void) {
    if (g_crumbs <= 0 || !g_crumb) return;
    uint32_t v = g_crumb[0], sub = v >> 16, pass = v & 0x7fff;
    VKLOG("[crumbs] the GPU last finished submission %u (low 16 bits) %s %u", sub, v & 0x8000 ? "a dispatch after pass" : "pass", pass);
    int shown = 0;
    for (unsigned i = g_cn > 512 ? g_cn - 512 : 0; i < g_cn && shown < 12; i++) { crumb_t *c = &g_cring[i % 512];
        if ((c->sub & 0xffff) < sub || ((c->sub & 0xffff) == sub && c->pass <= (int)pass)) continue;
        shown++; VKLOG("[crumbs] then submission %u pass %d:", c->sub, c->pass);
        for (int k = 0; k < c->n; k++) VKLOG("[crumbs]   shaders %016llx %016llx %016llx", (unsigned long long)c->k[k][0], (unsigned long long)c->k[k][1], (unsigned long long)c->k[k][2]); }
}
void vk_prof_end(void) {
    if (!g_prof_on || !g_prof_t0) return;
    extern uint64_t g_pkg_ns, g_pkg_n; uint64_t tot = tick() - g_prof_t0; g_prof_t0 = 0;
    if (tot < 40000000ull) return;
    double ms = 1e-6; uint64_t known = g_prof_ns[0] + g_prof_ns[1] + g_prof_ns[2] + g_prof_ns[3] + g_prof_ns[4] + g_prof_ns[5] + g_prof_ns[6] + g_prof_ns[7];
    VKLOG("slow replay (present %llu): %.1f ms; pipelines built here %llu in %.1f ms, textures %.1f ms (%llu packaged uploads, %.1f ms of it), pools sync %.1f ms, shader modules %.1f ms, pipeline binds %.1f ms, descriptor sets %.1f ms, cuts %.1f ms, compute pipelines %.1f ms, other %.1f ms",
          (unsigned long long)g_tex_frame, tot * ms, (unsigned long long)(g_vkst.pipes_built - g_prof_pipes0), g_prof_ns[0] * ms, g_prof_ns[1] * ms, (unsigned long long)(g_pkg_n - g_prof_npkg0),
          (g_pkg_ns - g_prof_pkg0) * ms, g_prof_ns[2] * ms, g_prof_ns[3] * ms, g_prof_ns[4] * ms, g_prof_ns[5] * ms, g_prof_ns[6] * ms, g_prof_ns[7] * ms, (tot > known ? tot - known : 0) * ms);
}
// Linear render targets (NVN LINEAR_RENDER_TARGET) are their storage on the Switch: the CPU can read what the GPU drew straight from the pool. Here a Vulkan image holds
// the drawing and the pool memory kept what it held before. Super Mario Odyssey's fluid surface (al::FluidSimulateWave: the Cap Kingdom ground fog's wave solver ends in a
// 128x128 RGBA16F linear target that the fog's vertex shader samples, and a 256x128 R32F one) is such a target. So a pass that drew into one copies each such colour
// target back to its storage, at its stride, marked as written by the GPU (the pools' copy-back carries it to the CPU). DM_VK_LIN_BACK=0 (live, A/B): not
static vkimg_t g_s0_image;   // slot 0's scratch target (DM_VK_SLOT0_FILL, below): where P_att[0].im is this, S.rt_col[0] is not the pass's target (the game's earlier one, or an alias's)
static void lin_back(void) {
    for (int i = 0; i < S.ncol && i < 8; i++) {
        vkimg_t *im = P_att[i].im; const tex_t *t = S.rt_has_col[i] ? &S.rt_col[i] : NULL; const vkfmt_t *f = t ? vk_fmt(t->d.format) : NULL;
        if (!im || im == &g_s0_image || !t || !t->mem || !(t->d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) || !f || !f->bpb || f->bw > 1 || im->levels != 1 || im->layers != 1 || im->d != 1) continue;
        size_t stride = t->d.stride ? t->d.stride : (size_t)im->w * f->bpb, len = stride * (im->h - 1) + (size_t)im->w * f->bpb; VkBuffer b; VkDeviceSize o;
        if (stride % f->bpb || stride < (size_t)im->w * f->bpb || !vk_buf_at((uint64_t)(uintptr_t)t->mem, len, &b, &o) || o % f->bpb) continue;
        VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
        VkBufferImageCopy r = {o, (uint32_t)(stride / f->bpb), 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {im->w, im->h, 1}};
        vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r); vk_img_barrier_all(cb);
        void vk_gpu_wrote(uint64_t a, uint64_t n); vk_gpu_wrote((uint64_t)(uintptr_t)t->mem, len); vk_readback_unknown();
        { static int n; if (n++ < 4) VKLOG("linear render target %ux%u (nvn format %#x, stride %zu) copied back to its storage %p", im->w, im->h, t->d.format, stride, (void *)t->mem); }
    }
}
// A block-compressed image's stand-in (gfx_vk_tex.c vk_tex_view_as) and the image: the region of one level and its layers (a 3D image's depth slices), in blocks
// for the stand-in and in texels for the image, for the copies through a buffer in either direction. The buffer holds the blocks row by row as both read them
static int bc_region(const vkimg_t *d, uint32_t lv, uint32_t ly, uint32_t n, VkBufferImageCopy *rd, VkBufferImageCopy *rp, VkDeviceSize *bytes) {
    const vkimg_t *p = d->bc_of; const vkfmt_t *uf = vk_fmt(d->nvn_format); if (!p || !uf) return 0;
    int is3d = p->vtype == VK_IMAGE_VIEW_TYPE_3D; if (!n) n = 1;
    uint32_t bw = d->w >> lv ? d->w >> lv : 1, bh = d->h >> lv ? d->h >> lv : 1, pw = p->w >> lv ? p->w >> lv : 1, ph = p->h >> lv ? p->h >> lv : 1;
    *rd = (VkBufferImageCopy){0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, lv, is3d ? 0 : ly, is3d ? 1 : n}, {0, 0, is3d ? (int32_t)ly : 0}, {bw, bh, is3d ? n : 1}};
    *rp = *rd; rp->imageExtent.width = pw; rp->imageExtent.height = ph;
    *bytes = (VkDeviceSize)bw * bh * uf->bpb * n; return 1;
}
// Before a pass that draws into a stand-in: the level and layers it draws, copied from the image (image → buffer → stand-in), so that what the pass does not cover
// keeps the image's blocks when bc_draw_back copies them back. A new image (never used: its blocks undefined) leaves the stand-in as it is
static void bc_fill(vkimg_t *d, uint32_t lv, uint32_t ly, uint32_t n) {
    vkimg_t *p = d->bc_of; VkBufferImageCopy rd, rp; VkDeviceSize bytes; VkBuffer sb; VkDeviceSize so;
    if (!p || p->layout == VK_IMAGE_LAYOUT_UNDEFINED || !bc_region(d, lv, ly, n, &rd, &rp, &bytes) || !vk_scratch(bytes, 16, &sb, &so)) return;
    rd.bufferOffset = rp.bufferOffset = so;
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    vkCmdCopyImageToBuffer(cb, p->img, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &rp); vk_img_barrier_all(cb);
    vkCmdCopyBufferToImage(cb, sb, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rd); vk_img_barrier_all(cb);
}
// After a pass that drew into a stand-in: the level and layers it drew, copied into the image as blocks through scratch memory (stand-in → buffer → image, the
// second copy as an upload's)
static void bc_draw_back(void) {
    for (int i = 0; i < 8; i++) { vkimg_t *d = P_att[i].im; if (!d || !d->bc_of) continue;
        vkimg_t *p = d->bc_of; VkBufferImageCopy rd, rp; VkDeviceSize bytes; VkBuffer sb; VkDeviceSize so;
        if (!bc_region(d, P_att[i].bc_lv, P_att[i].bc_ly, P_att[i].bc_nly, &rd, &rp, &bytes) || !vk_scratch(bytes, 16, &sb, &so)) continue;
        rd.bufferOffset = rp.bufferOffset = so;
        VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, p); vk_img_barrier_all(cb);
        vkCmdCopyImageToBuffer(cb, d->img, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &rd); vk_img_barrier_all(cb);
        vkCmdCopyBufferToImage(cb, sb, p->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rp); vk_img_barrier_all(cb);
        p->gpu_w = 1; }
}
void vk_end_pass(void) {
    if (g_x_listpasses && S.pass_open == 1) { Dl_info di; void *ra = __builtin_return_address(0); uintptr_t base = dladdr((void *)vk_end_pass, &di) ? (uintptr_t)di.dli_fbase : 0; VKLOG("    ended from %#llx (%llu draws)", (unsigned long long)((uintptr_t)ra - base), (unsigned long long)g_pass_draws); list_reads("it"); }   // (DM_VK_TRACE_PASS=-2: who ends each pass)
    if (S.pass_open == 2 && P_cleared) (void)pass_commit();   // only clears: the pass still runs, for its load ops
    if (S.pass_open == 1) { if (S.rp_pass) vkCmdEndRenderPass(vk_cmd()); else vkCmdEndRendering(vk_cmd()); bc_draw_back(); void vk_pass_ts(int end, uint32_t w, uint32_t h, uint32_t fmt, uint32_t draws); vk_pass_ts(1, S.pass_w, S.pass_h, (uint32_t)S.cfmt[0], (uint32_t)g_pass_draws | (g_pass_draws ? (uint32_t)((g_pass_bb[2] - g_pass_bb[0]) & 0xffff) << 16 : 0));  pass_dump(); if (g_x_tracepass >= 0 && g_sub_pass - 1 == g_x_tracepass) { setenv("DM_VK_TRACE_PASS", "-1", 1); g_x_tracepass = -1; } }
    if (S.pass_open == 1 && g_lin_back && g_pass_draws) lin_back();
    if (S.pass_open == 1) crumb_mark(0);
    S.pass_open = 0; S.bound_pipe = VK_NULL_HANDLE; g_tf_in_pass = 0;
}
// a pass end before something that reads or writes pool memory other than through a uniform binding (copies, fills, dispatches, buffer ↔ texture
// copies, the submission's end): the deferred uniform updates land first (draws read them through their shadows, so ordinary pass ends keep them)
#include "gfx_vk_pixel_history.h"
// DM_VK_PIXEL_HISTORY=WxH:X,Y (live, one frame): trace retained HDR colour and its depth after each logical draw/clear. Persistent non-finite HDR pixels
// survived pass-level captures and a late-depth probe; identify their depth writer and matching material before changing rendering. The per-draw pass cuts
// and transfer reads are intrusive and can hide driver hazards: compare the final colour with an ordinary capture. Never use this mode for performance.
enum { PH_MAX = 4096 };
typedef struct { uint64_t frame, vs, fs, vb, ib, ubo[16]; int pass, draw, kind, prepared, count, instances, base, dt, dw, df, dfmt, sc[5]; uint32_t mask, blend, attr, stride, poly[4]; float bias[3], vp[4], range[2]; } phrow_t;
static struct { int state, n, truncated; uint32_t area[4]; uint64_t fence; VkBuffer b; VkDeviceMemory mem; uint8_t *map; phrow_t *rows; } g_ph;
static int pixel_history_busy(void) { return g_ph.state != 0; }
static void pixel_history_record(int kind, int draw_id, int prepared, int count, int instances, uint64_t ib, int base) {
    if (g_ph.state != 1 || S.pass_w != g_ph.area[0] || S.pass_h != g_ph.area[1] || S.pass_open == 0 || !S.rt_has_col[0]) return;
    vkimg_t *im = P_att[0].im, *dep = (S.daspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? P_att[8].im : NULL;
    if (!im || im->fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || im->samples != VK_SAMPLE_COUNT_1_BIT) return;
    uint32_t lv = S.rt_has_cview[0] && S.rt_cview[0].set_levels ? S.rt_cview[0].base_level : 0;
    uint32_t ly = S.rt_has_cview[0] && S.rt_cview[0].set_layers ? S.rt_cview[0].min_layer : 0;
    uint32_t x = g_ph.area[2], y = g_ph.area[3];
    if (lv >= im->levels || ly >= im->layers || x >= (im->w >> lv) || y >= (im->h >> lv)) return;
    if (g_ph.n == PH_MAX) { g_ph.truncated = 1; return; }
    if (S.pass_open == 2 && !pass_commit()) return;
    uint32_t dlv = S.rt_has_dview && S.rt_dview.set_levels ? S.rt_dview.base_level : 0;
    uint32_t dly = S.rt_has_dview && S.rt_dview.set_layers ? S.rt_dview.min_layer : 0;
    if (dep && (dep->samples != VK_SAMPLE_COUNT_1_BIT || dlv >= dep->levels || dly >= dep->layers || x >= (dep->w >> dlv) || y >= (dep->h >> dlv))) dep = NULL;
    va_st a; memcpy(&a, &S.vattr[0], sizeof a);
    phrow_t *r = &g_ph.rows[g_ph.n]; *r = (phrow_t){.frame = g_tex_frame, .vs = S.sh[0] ? S.sh[0]->key : 0, .fs = S.sh[1] ? S.sh[1]->key : 0,
        .vb = a.stream < 16 ? S.vbuf[a.stream].address : 0, .ib = ib, .pass = g_sub_pass - 1, .draw = draw_id, .kind = kind, .prepared = prepared,
        .count = count, .instances = instances, .base = base, .dt = S.ds.dt, .dw = S.ds.dw, .df = S.ds.df, .dfmt = dep ? (int)dep->fmt : 0,
        .mask = S.chmask.mask, .blend = S.color.blend_en, .attr = S.vattr[0], .stride = a.stream < 16 ? S.vstream[a.stream].stride : 0,
        .bias = {S.pofs.factor, S.pofs.units, S.pofs.clamp}, .sc = {S.sc_set, S.sc.x, S.sc.y, S.sc.w, S.sc.h},
        .poly = {S.poly.cull, S.poly.front, S.poly.mode, S.poly.offset_en}, .range = {S.dnear, S.dfar}};
    memcpy(r->vp, S.vp[0], sizeof r->vp);
    for (int i = 0; i < 16; i++) r->ubo[i] = S.ubo[0][i].addr;
    VkDeviceSize off = (VkDeviceSize)g_ph.n++ * 16;
    vk_end_pass(); VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    VkBufferImageCopy copy = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1}, {(int32_t)x, (int32_t)y, 0}, {1, 1, 1}};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, g_ph.b, 1, &copy);
    if (dep) { copy.bufferOffset = off + 4; copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT, dlv, dly, 1};
        vkCmdCopyImageToBuffer(cb, dep->img, VK_IMAGE_LAYOUT_GENERAL, g_ph.b, 1, &copy); }
    vk_img_barrier_all(cb);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void pixel_history_poll(uint64_t done_fence) {
    if (g_ph.state == 1) { g_ph.state = 2; g_ph.fence = mtl_last_fence() + 1; return; }
    if (g_ph.state == 2) {
        if (done_fence < g_ph.fence) return;
        VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, g_ph.mem, 0, VK_WHOLE_SIZE}; VKCHK(vkInvalidateMappedMemoryRanges(V.dev, 1, &mr));
        const char *root = getenv("DM_ROOT"); char path[1024]; snprintf(path, sizeof path, "%s/pixel_history.tsv", root ? root : "."); FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "# pixel history %ux%u:%u,%u; intrusive pass cuts/copies; logical draws, prepared does not prove a GPU draw was issued; truncated=%d\n", g_ph.area[0], g_ph.area[1], g_ph.area[2], g_ph.area[3], g_ph.truncated);
            fprintf(f, "event\tframe\tpass\tdraw\tkind\tprepared\tvs\tfs\tcount\tinstances\tbase\tvb\tib\tattr0\tstride\tdt\tdw\tdf\tbias_factor\tbias_units\tbias_clamp\tmask\tblend\thdr_word\tdepth_word\tdepth_format\tr\tg\tb\tdepth");
            fprintf(f, "\tsc_set\tsc_x\tsc_y\tsc_w\tsc_h\tpoly_cull\tpoly_front\tpoly_mode\tpoly_bias_mask\tvp_x\tvp_y\tvp_w\tvp_h\tz_near\tz_far");
            for (int i = 0; i < 16; i++) fprintf(f, "\tvs_ubo%d", i); fputc('\n', f);
            for (int i = 0; i < g_ph.n; i++) { phrow_t *r = &g_ph.rows[i]; uint32_t c, d = 0; memcpy(&c, g_ph.map + 16 * i, 4); if (r->dfmt) memcpy(&d, g_ph.map + 16 * i + 4, 4); float z = NAN;
                if (r->dfmt == VK_FORMAT_D16_UNORM) z = (d & 65535u) / 65535.f;
                else if (r->dfmt == VK_FORMAT_D32_SFLOAT || r->dfmt == VK_FORMAT_D32_SFLOAT_S8_UINT) memcpy(&z, &d, 4);
                else if (r->dfmt == VK_FORMAT_D24_UNORM_S8_UINT || r->dfmt == VK_FORMAT_X8_D24_UNORM_PACK32) z = (d & 0xffffffu) / 16777215.f;
                fprintf(f, "%d\t%llu\t%d\t%d\t%d\t%d\t%016llx\t%016llx\t%d\t%d\t%d\t%llx\t%llx\t%x\t%u\t%d\t%d\t%d\t%.9g\t%.9g\t%.9g\t%x\t%x\t%08x\t%08x\t%d\t%.9g\t%.9g\t%.9g\t%.9g",
                    i, (unsigned long long)r->frame, r->pass, r->draw, r->kind, r->prepared, (unsigned long long)r->vs, (unsigned long long)r->fs, r->count, r->instances, r->base,
                    (unsigned long long)r->vb, (unsigned long long)r->ib, r->attr, r->stride, r->dt, r->dw, r->df, r->bias[0], r->bias[1], r->bias[2], r->mask, r->blend, c, d, r->dfmt,
                    vk_pixel_history_ufloat(c & 2047u, 6), vk_pixel_history_ufloat(c >> 11 & 2047u, 6), vk_pixel_history_ufloat(c >> 22, 5), z);
                for (int j = 0; j < 5; j++) fprintf(f, "\t%d", r->sc[j]); for (int j = 0; j < 4; j++) fprintf(f, "\t%u", r->poly[j]);
                for (int j = 0; j < 4; j++) fprintf(f, "\t%.9g", r->vp[j]); fprintf(f, "\t%.9g\t%.9g", r->range[0], r->range[1]);
                for (int j = 0; j < 16; j++) fprintf(f, "\t%llx", (unsigned long long)r->ubo[j]); fputc('\n', f); }
            fclose(f); VKLOG("pixel history: %d events%s at %u,%u -> %s", g_ph.n, g_ph.truncated ? " (truncated)" : "", g_ph.area[2], g_ph.area[3], path);
        } else VKLOG("[RED] cannot write pixel history %s", path);
        vkDestroyBuffer(V.dev, g_ph.b, NULL); vkFreeMemory(V.dev, g_ph.mem, NULL); free(g_ph.rows); memset(&g_ph, 0, sizeof g_ph);
    }
    const char *e = getenv("DM_VK_PIXEL_HISTORY"); if (!e || !*e || !strcmp(e, "0")) return;
    uint32_t area[4]; int ok = vk_pixel_history_parse(e, area); setenv("DM_VK_PIXEL_HISTORY", "", 1);
    if (!ok || g_pd_state) { VKLOG("[RED] pixel history: need WxH:X,Y in bounds, with ordinary pass dumps inactive"); return; }
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, PH_MAX * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &g_ph.b)); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, g_ph.b, &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0)};
    VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &g_ph.mem)); VKCHK(vkBindBufferMemory(V.dev, g_ph.b, g_ph.mem, 0)); VKCHK(vkMapMemory(V.dev, g_ph.mem, 0, VK_WHOLE_SIZE, 0, (void **)&g_ph.map));
    g_ph.rows = calloc(PH_MAX, sizeof *g_ph.rows); if (!g_ph.rows) { vkDestroyBuffer(V.dev, g_ph.b, NULL); vkFreeMemory(V.dev, g_ph.mem, NULL); memset(&g_ph, 0, sizeof g_ph); VKLOG("[RED] pixel history: metadata allocation failed"); return; }
    memcpy(g_ph.area, area, sizeof area); g_ph.state = 1; VKLOG("pixel history: next frame at %ux%u:%u,%u (intrusive, at most %d events)", area[0], area[1], area[2], area[3], PH_MAX);
}
void vk_end_pass_land(void) { vk_end_pass(); if (vk_sub_open()) vk_ubd_land(vk_cmd()); }
static int pass_whole(VkRect2D r) { return !r.offset.x && !r.offset.y && r.extent.width >= S.pass_w && r.extent.height >= S.pass_h; }
// An empty colour slot 0 (DM_VK_SLOT0_FILL; opt-in, any GPU). A pass whose colour attachments start past slot 0 (slot 0 a null view, VK_ATTACHMENT_UNUSED in a render
// pass) hung the GPU of an Adreno 830 and lost the device every frame: a OnePlus Pad 3 (driver 0x8032004a) whose pass bound a target in slot 5 with slots 0-4 empty, which a
// later pass drew into as slot 0; a scratch target in slot 0 alone fixed it (droidplay-core 87bd41d62, the idea). Not reproduced with nx2android: no such hang is known on
// the Odin 3 (Adreno 830, Qualcomm's driver 512.800), so it is opt-in. Slot 0 of such a pass gets a scratch RGBA8 image at least the pass's size (one, grown when a pass
// is larger; the one before goes to vk_defer_free_*, destroyed once the GPU is past the submissions that name it), loaded and stored DONT_CARE, a transient attachment
// in lazily allocated memory where the device has such a type (on a tiler it never leaves tile memory), and the pipelines write nothing to it (prepare_draw: write
// mask 0 where P_att[0] is the scratch). Single-sample, single-layer passes only. DM_VK_SLOT0_FILL=1 (start-up): on; off by default
static VkImageView g_s0_view; static uint64_t g_s0_passes; static int g_s0_failed;
static int slot0_fill_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_VK_SLOT0_FILL");
        on = e && *e && atoi(e) != 0;
        VKLOG("empty colour slot 0: %s (DM_VK_SLOT0_FILL%s; %s)", on ? "a scratch target there in multi-target passes" : "left empty", e && *e ? "" : " default off", V.props.deviceName); }
    return on;
}
// whether a pass of ncol colour slots, slot 0 without a view, gets the scratch there
static int slot0_fill_wanted(int ncol, int slot0_view, uint32_t samples, uint32_t layers) { return ncol > 1 && !slot0_view && samples == 1 && layers == 1 && slot0_fill_on(); }
static int slot0_scratch(uint32_t w, uint32_t h) {   // the scratch image, at least w x h; 0: none could be made (the slot stays empty)
    if (g_s0_view && g_s0_image.w >= w && g_s0_image.h >= h) return 1;
    if (g_s0_failed) return 0;
    vkimg_t im = {0}; VkImageView v = VK_NULL_HANDLE;
    im.w = w > g_s0_image.w ? w : g_s0_image.w; im.h = h > g_s0_image.h ? h : g_s0_image.h;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {im.w, im.h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};   // (transient: only ever an attachment, never loaded or stored)
    if (vkCreateImage(V.dev, &ci, NULL, &im.img)) goto fail;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(V.dev, im.img, &mr);
    uint32_t mt = vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, 0);   // lazily allocated where there is such a type
    if (mt == UINT32_MAX) mt = vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);                                 // else device-local, as any target
    if (mt == UINT32_MAX) goto fail;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
    VkResult r = vkAllocateMemory(V.dev, &ai, NULL, &im.mem);
    if (r || vkBindImageMemory(V.dev, im.img, im.mem, 0)) goto fail;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im.img, VK_IMAGE_VIEW_TYPE_2D, ci.format, {0, 0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(V.dev, &vi, NULL, &v)) goto fail;
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im.img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(vk_cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    im.fmt = ci.format; im.usage = ci.usage; im.aspect = VK_IMAGE_ASPECT_COLOR_BIT; im.d = im.layers = im.levels = im.samples = 1; im.vtype = VK_IMAGE_VIEW_TYPE_2D; im.layout = VK_IMAGE_LAYOUT_GENERAL;
    VKLOG("empty colour slot 0: scratch target %ux%u%s (DM_VK_SLOT0_FILL)", im.w, im.h, V.mem.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? ", lazily allocated" : "");
    if (g_s0_view) { vk_defer_free_view(g_s0_view); vk_defer_free_image(g_s0_image.img, g_s0_image.mem); }   // the smaller one: once the GPU is past the submissions that name it
    g_s0_image = im; g_s0_view = v; return 1;
fail:
    VKLOG("[RED] empty colour slot 0: no scratch target %ux%u could be made: such passes keep it empty from here", im.w, im.h); g_s0_failed = 1;
    if (v) vkDestroyImageView(V.dev, v, NULL);
    if (im.img) vkDestroyImage(V.dev, im.img, NULL);
    if (im.mem) vkFreeMemory(V.dev, im.mem, NULL);
    return 0;
}
static int begin_pass(void) {
    if (S.pass_open) return 1;
    int psplit = g_psplit_now; g_psplit_now = 0;   // a pass split (prepare_draw): taken before any early return, so the flag never forces a later pass's cut
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo *ca = P_ca; memset(ca, 0, sizeof P_ca);
    for (int i = 0; i < 9; i++) P_att[i].im = NULL;
    VkRenderingAttachmentInfo da = {0}, sa = {0};
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX, samples = 1; int ncol = 0, have = 0;
    for (int i = 0; i < S.rt_n; i++) {
        S.cfmt[i] = VK_FORMAT_UNDEFINED;
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (!S.rt_has_col[i]) continue;
        vkimg_t *im; VkImageView v = vk_tex_view(&S.rt_col[i], S.rt_has_cview[i] ? &S.rt_cview[i] : NULL, 0xff, 0, 1, &im);
        if (!v) continue;
        vk_tex_prepare(cb, im);
        uint32_t lv = S.rt_has_cview[i] && S.rt_cview[i].set_levels ? S.rt_cview[i].base_level : 0;
        uint32_t iw = im->w >> lv ? im->w >> lv : 1, ih = im->h >> lv ? im->h >> lv : 1;
        // a block-compressed texture drawn into through an uncompressed view (RGBA32UI over BC3: one texel per 4x4 block, Mario Kart 8 Deluxe compressing its
        // reflection cubemaps on the GPU) is that view's size in blocks: with the texel size the render area was 16 times the attachment, which Vulkan does not allow (the render area must lie within every attachment view)
        { const vkfmt_t *tf = vk_fmt(S.rt_col[i].d.format), *vf = S.rt_has_cview[i] && S.rt_cview[i].set_format ? vk_fmt(S.rt_cview[i].format) : tf;
          if (tf && vf && tf->compressed && !tf->dec && !vf->compressed && tf->bw && tf->bh) { if (!im->bc_of) { iw = (iw + tf->bw - 1) / tf->bw; ih = (ih + tf->bh - 1) / tf->bh; }   // (a stand-in is in blocks already: vk_tex_view_as)
              static int said; if (!said++) VKLOG("colour target: block-compressed format %#x drawn through view format %#x: pass in blocks, %ux%u", S.rt_col[i].d.format, S.rt_cview[i].format, iw, ih); } }
        if (iw < w) w = iw; if (ih < h) h = ih; samples = im->samples; have = 1;
        ca[i].imageView = v; S.cfmt[i] = vk_view_fmt; ncol = i + 1; P_att[i].im = im;   // (the view's format: a render pass's attachment and its pipelines take it)
        P_att[i].fmt = vk_view_fmt; P_att[i].layers = vk_view_layers ? vk_view_layers : 1; P_att[i].w = iw; P_att[i].h = ih; P_att[i].usage = vk_view_usage;
        P_att[i].bc_lv = im->bc_lv; P_att[i].bc_ly = im->bc_ly; P_att[i].bc_nly = im->bc_nly;
        if (im->bc_of) bc_fill(im, im->bc_lv, im->bc_ly, im->bc_nly);   // (before the pass begins: what it does not cover keeps the image's blocks)
        if (P_att[i].layers < layers) layers = P_att[i].layers;
    }
    for (int i=0;i<8;i++) P_alias_views[i]=ca[i].imageView;
    color_alias_map((uint32_t)ncol,P_alias_views,S.chmask.mask,P_alias);
    for (int i=0;i<ncol;i++) if (P_alias[i]!=i) {
        ca[i].imageView=VK_NULL_HANDLE;ca[i].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;ca[i].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        S.cfmt[i]=VK_FORMAT_UNDEFINED;P_att[i].im=NULL;
        static int said;if (!said++) VKLOG("inactive colour attachment alias: slot %d uses slot %u (one image load/store)",i,(unsigned)P_alias[i]);
    }
    S.dfmt = VK_FORMAT_UNDEFINED; S.daspect = 0;
    if (S.rt_has_dep) {
        vkimg_t *im; VkImageView v = vk_tex_view(&S.rt_dep, S.rt_has_dview ? &S.rt_dview : NULL, 0xff, 0, 1, &im);
        if (v) {
            vk_tex_prepare(cb, im);
            uint32_t lv = S.rt_has_dview && S.rt_dview.set_levels ? S.rt_dview.base_level : 0;
            uint32_t iw = im->w >> lv ? im->w >> lv : 1, ih = im->h >> lv ? im->h >> lv : 1;
            if (iw < w) w = iw; if (ih < h) h = ih; samples = im->samples; have = 1;
            da = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, v, VK_IMAGE_LAYOUT_GENERAL, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
            S.dfmt = im->fmt; S.daspect = im->aspect; P_att[8].im = im; P_att[8].fmt = vk_view_fmt; P_att[8].layers = vk_view_layers ? vk_view_layers : 1; P_att[8].w = iw; P_att[8].h = ih; P_att[8].usage = vk_view_usage;
            if (P_att[8].layers < layers) layers = P_att[8].layers;
            if (im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) sa = da;
        }
    }
    if (!have) {
        // No target set at all (a target that failed to bind still skips): an attachment-less pass. FFT's UI composite draws into its window image with image
        // stores in passes with no colour or depth target; skipped, the screen stayed black. Its size is the viewport's (else the scissor's), clamped to the device
        int none = !S.rt_has_dep; for (int i = 0; i < S.rt_n && i < 8; i++) if (S.rt_has_col[i] || S.rt_bad[i]) none = 0;
        static int notarget = -1; if (notarget < 0) { const char *e = getenv("DM_VK_X_NOTARGET"); notarget = !(e && !atoi(e)); }   // DM_VK_X_NOTARGET=0: skip these too, as before (A/B)
        if (!none || !notarget) { VKRED_ONCE(30, "draw with no usable render target: skipped"); return 0; }
        float ex = fmaxf(S.vp[0][0], 0.f) + fabsf(S.vp[0][2]), ey = fmaxf(S.vp[0][1], 0.f) + fabsf(S.vp[0][3]);
        if (S.vp[0][2] == 0.f && S.vp[0][3] == 0.f && S.sc_set) { ex = (float)S.sc.x + (float)S.sc.w; ey = (float)S.sc.y + (float)S.sc.h; }   // (set_dynamic's fallback)
        uint32_t mw = V.props.limits.maxFramebufferWidth, mh = V.props.limits.maxFramebufferHeight;
        w = ex >= 1.f ? (uint32_t)ceilf(ex) : 1; h = ey >= 1.f ? (uint32_t)ceilf(ey) : 1;
        if (mw && w > mw) w = mw; if (mh && h > mh) h = mh;
        layers = 1; samples = 1; ncol = 0; have = 1;
        for (int i = 0; i < 8; i++) S.cfmt[i] = VK_FORMAT_UNDEFINED;
        S.dfmt = VK_FORMAT_UNDEFINED; S.daspect = 0;
        static int said; if (!said++) VKLOG("draw with no render target: attachment-less pass %ux%u", w, h);
    }
    if (have && slot0_fill_wanted(ncol, ca[0].imageView != VK_NULL_HANDLE, samples, layers) && slot0_scratch(w, h)) {   // an empty slot 0 gets the scratch (DM_VK_SLOT0_FILL, opt-in; above)
        ca[0] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, g_s0_view, VK_IMAGE_LAYOUT_GENERAL, 0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE};
        S.cfmt[0] = VK_FORMAT_R8G8B8A8_UNORM; P_att[0].im = &g_s0_image; P_att[0].fmt = VK_FORMAT_R8G8B8A8_UNORM; P_att[0].layers = 1; P_att[0].w = g_s0_image.w; P_att[0].h = g_s0_image.h; P_att[0].usage = g_s0_image.usage;
        if (g_s0_passes++ < 4) VKLOG("empty colour slot 0: scratch target for a %ux%u pass with %d colour slots (DM_VK_SLOT0_FILL; %llu so far)", w, h, ncol, (unsigned long long)g_s0_passes);
    }
    // The cut (DM_VK_CB_DRAWS): only before a pass whose depth attachment no earlier pass of this submission drew with. Adreno keeps a low-resolution copy of a depth
    // attachment (LRZ) that it trusts within a command buffer; a pass that goes on with a depth buffer begun in an earlier command buffer had whole 8x8 blocks of its
    // nearest objects rejected in some frames (NEO: bushes, trees and the phone booth near the camera flickered light blue). DM_VK_X_CUTANY=1 cuts before any pass.
    // The rule looks only at the pass at the cut: a later pass may still go on with a depth buffer drawn before it. Hades II cuts before its depth-less passes and then
    // goes on with the 1280x720 D24S8 of the passes before the cut: its lights went out for single frames while Melinoe walked near them (one-frame dark spikes in
    // 6-23 of 600 recorded frames; 0 with DM_VK_CB_DRAWS=0, also 2-15 with the parts submitted together, 135 without the LRZ touches). Such a pass first gets a texel of its
    // depth written back (lrz_touch), the transfer write that made Adreno drop a stale LRZ for Xenoblade Chronicles 3 (DM_VK_LRZ_TOUCH=0 leaves it out)
    // The write has its own cost: Animal Crossing: New Horizons' island lost its lit ground in a full-width band over the top third (the first bin row of a later pass)
    // in 6-22 of 60 frames with it, 0 of 60 with only a read at the cut (DM_VK_LRZ_TOUCH bit 16 without bit 8) or with no cuts; the texel at the far corner instead
    // of the first kept the band at the top. That profile runs uncut (DM_VK_CB_DRAWS=0, also 2.4 ms less GPU); the default here stays the write Hades II was checked with
    static uint64_t g_cuts; int cut_touch = 0;
    vkimg_t *dim = NULL; if (da.imageView) vk_tex_view(&S.rt_dep, S.rt_has_dview ? &S.rt_dview : NULL, 0xff, 0, 1, &dim);
    if (psplit || (g_x_cbdraws > 0 && g_cb_draws >= g_x_cbdraws && (!dim || dim->depth_sub != g_set_epoch || g_x_cutany))) {   // (a pass split: prepare_draw checked the depth rule and the part budget)
        void vk_cmd_split(void); if (g_x_landcut) vk_ubd_land(vk_cmd());
        if (psplit) vk_cmd_split(); else if (g_ord_parts < 15) { vk_cmd_split(); g_ord_parts++; }   // ordinary cuts as before MAIN_CBS 32 (refused past 15; g_cuts still counts, as it did)
        g_cb_draws = 0; cb = vk_cmd(); g_cuts++;   // no pass is open here; deferred uniform updates stay deferred (draws read their shadows)
    }
    if (dim) { cut_touch = (g_lrz_touch & (LRZ_CUT | LRZ_CUT_RD)) && dim->depth_sub == g_set_epoch && dim->depth_cut != g_cuts; dim->depth_sub = g_set_epoch; dim->depth_cut = g_cuts; }
    { static int n; uint32_t cs[9] = {0}; int any = 0, mis = 0; uint32_t s0 = 0;   // multisampled passes: every attachment's sample count (they must agree)
      for (int i = 0; i < ncol; i++) if (ca[i].imageView && P_att[i].im != &g_s0_image) { vkimg_t *im2; vk_tex_view(&S.rt_col[i], S.rt_has_cview[i] ? &S.rt_cview[i] : NULL, 0xff, 0, 1, &im2); cs[i] = im2 ? im2->samples : 0; if (cs[i] > 1) any = 1; if (!s0) s0 = cs[i]; else if (cs[i] != s0) mis = 1; }
      if (da.imageView) { vkimg_t *im2; vk_tex_view(&S.rt_dep, S.rt_has_dview ? &S.rt_dview : NULL, 0xff, 0, 1, &im2); cs[8] = im2 ? im2->samples : 0; if (cs[8] > 1) any = 1; if (s0 && cs[8] != s0) mis = 1; }
      if ((any || mis) && n < 10) { n++; VKLOG("pass %ux%u samples: colour %u %u %u %u, depth %u%s", w, h, cs[0], cs[1], cs[2], cs[3], cs[8], mis ? " MISMATCH" : ""); } }
    if (!g_x_nobar) vk_img_barrier_all(cb);
    if (cut_touch) lrz_touch(dim, (g_lrz_touch & LRZ_CUT) != 0);   // (after the barrier: the earlier command buffer's depth writes come first)
    if (g_x_depth) { da.loadOp = sa.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; da.storeOp = sa.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; }
    if (g_x_color) for (int i = 0; i < ncol; i++) ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    P_da = da; P_sa = sa; P_cleared = 0;
    P_ri = (VkRenderingInfo){VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {w, h}}, layers, 0, (uint32_t)ncol, P_ca,
                             (S.daspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? &P_da : NULL, (S.daspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? &P_sa : NULL};
    S.pass_open = 2; S.pass_w = w; S.pass_h = h; S.samples = samples; S.ncol = ncol; S.bound_pipe = VK_NULL_HANDLE;
    return 1;
}
static VkRect2D scissor_rect(void) {
    int32_t x = 0, y = 0; int64_t x1 = S.pass_w, y1 = S.pass_h;
    if (S.sc_set) {
        int64_t sx = S.sc.x, sy = S.sc.y, sx1 = (int64_t)S.sc.x + S.sc.w, sy1 = (int64_t)S.sc.y + S.sc.h;
        if (g_lower_left && !g_flipy) { int64_t t = (int64_t)S.pass_h - sy1; sy1 = (int64_t)S.pass_h - sy; sy = t; }
        if (sx > x) x = (int32_t)sx; if (sy > y) y = (int32_t)sy; if (sx1 < x1) x1 = sx1; if (sy1 < y1) y1 = sy1;
    }
    if (x1 < x) x1 = x; if (y1 < y) y1 = y;
    return (VkRect2D){{x, y}, {(uint32_t)(x1 - x), (uint32_t)(y1 - y)}};
}
// A clear of some channels only (NVN's ClearColor mask): vkCmdClearAttachments writes every channel, so it is a draw of dm/android/shaders/maskclear.* with the mask as
// the colour write mask, cut to the clear's rectangle by the scissor. Paper Mario: The Thousand-Year Door clears alpha alone (mask 0x8) over a rectangle of its scene
// target when Mario comes out of the water: cleared in full, the floor and water there went black for a frame. One pipeline per pass layout and mask, for attachment 0
#include "maskclear_vert.spv.h"
#include "maskclear_frag.spv.h"
#include "flatdbg_frag.spv.h"
typedef struct { uint32_t ncol, cfmt[8], dfmt, samples, mask; } mckey;
static struct { VkPipelineLayout pl; VkShaderModule vs, fs; int bad; mckey k[32]; VkPipeline p[32]; int n; } g_mc;
static VkPipeline mc_pipeline(uint32_t mask) {
    mckey k; memset(&k, 0, sizeof k); k.ncol = (uint32_t)S.ncol; for (int i = 0; i < S.ncol && i < 8; i++) k.cfmt[i] = (uint32_t)S.cfmt[i];
    k.dfmt = (uint32_t)S.dfmt; k.samples = S.samples ? S.samples : 1; k.mask = mask;
    for (int i = 0; i < g_mc.n; i++) if (!memcmp(&g_mc.k[i], &k, sizeof k)) return g_mc.p[i];
    if (g_mc.bad || g_mc.n == 32) return VK_NULL_HANDLE;
    if (!g_mc.pl) {
        VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pr};
        VkShaderModuleCreateInfo vi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof maskclear_vert_spv, maskclear_vert_spv}, fi = vi;
        fi.codeSize = sizeof maskclear_frag_spv; fi.pCode = maskclear_frag_spv;
        if (vkCreatePipelineLayout(V.dev, &li, NULL, &g_mc.pl) || vkCreateShaderModule(V.dev, &vi, NULL, &g_mc.vs) || vkCreateShaderModule(V.dev, &fi, NULL, &g_mc.fs)) { g_mc.bad = 1; return VK_NULL_HANDLE; }
    }
    VkPipelineShaderStageCreateInfo ss[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, g_mc.vs, "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_mc.fs, "main", NULL}};
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
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, prp ? NULL : &rci, 0, 2, ss, &vin, &ia, NULL, &vs, &rs, &ms, &ds, &cb, &dy, g_mc.pl, prp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    if ((!prp && !V.has_dynrender) || vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &p)) { VKRED_ONCE(32, "masked clear pipeline failed: cleared in full"); p = VK_NULL_HANDLE; }
    vk_pipeline_cache_changed();
    g_mc.k[g_mc.n] = k; g_mc.p[g_mc.n++] = p; return p;
}
static int g_bc_stale;
static int clear_masked(const p_clear_color *c, VkRect2D r) {   // 1 = done
    const vkfmt_t *f = vk_fmt(S.rt_col[c->index].d.format);
    if (c->index != 0 || (f && f->is_int)) return 0;
    VkPipeline p = mc_pipeline(c->mask); if (!p) return 0;
    if (!pass_commit()) return 1;
    VkCommandBuffer cb = vk_cmd();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); S.bound_pipe = VK_NULL_HANDLE;
    VkViewport vp = {0, 0, (float)S.pass_w, (float)S.pass_h, 0, 1}; vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &r);
    vkCmdPushConstants(cb, g_mc.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, c->rgba);
    vkCmdDraw(cb, 3, 1, 0, 0);
    g_bc_stale = 1;   // the next draw sets its viewport, scissor and bindings again (bc_fresh)
    return 1;
}
static void clear_color(const p_clear_color *c) {
    if (!begin_pass()) return;
    if (c->index < 0 || c->index >= S.ncol) return;
    p_clear_color mapped=*c;mapped.index=P_alias[c->index];c=&mapped;
    if (S.cfmt[c->index] == VK_FORMAT_UNDEFINED || P_att[c->index].im == &g_s0_image || !c->mask) return;   // (slot 0's scratch is the game's empty slot: nothing to clear)
    if ((c->mask & 0xf) != 0xf) { VkRect2D r = scissor_rect(); if (!r.extent.width || !r.extent.height) return;
        if (clear_masked(c, r)) return;
        VKRED_ONCE(31, "partial channel clear mask %#x cleared in full", c->mask); }
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, {{{c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]}}}};
    const vkfmt_t *f = vk_fmt(S.rt_col[c->index].d.format);
    if (f && f->is_int) for (int i = 0; i < 4; i++) { if (f->is_sint) a.clearValue.color.int32[i] = (int32_t)c->rgba[i]; else a.clearValue.color.uint32[i] = c->rgba[i] > 0 ? (uint32_t)c->rgba[i] : 0; }
    // every layer of the pass: NVN clears the whole bound target, and a layered pass (a 3D texture drawn slice by slice, the layer from a geometry shader) cleared
    // only its layer 0 here. FINAL FANTASY RESONANCE adds its lights into 32x32x32 volumes (vertex shader f701ff79cec258e6, 32 instances, additive blend): slices 1-31
    // kept what earlier frames added, and the pass of fog boxes that reads them (vertex shader 1300ca45ddca3566) came out different every frame on its first road
    // (pass dumps over 4 frames, centre of the road: 29.6, 40.7, 31.7, 39.7; with every layer cleared 30.4 in each, the passes before it the same either way)
    VkClearRect r = {scissor_rect(), 0, P_ri.layerCount ? P_ri.layerCount : 1};
    if (S.pass_open == 2 && (c->mask & 0xf) == 0xf && pass_whole(r.rect)) {   // the whole attachment before any draw: its load op
        P_ca[c->index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; P_ca[c->index].clearValue.color = a.clearValue.color; P_cleared = 1; g_vkst.folded++; return;
    }
    if (!pass_commit()) return;
    if (r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(vk_cmd(), 1, &a, 1, &r);
}
static void clear_ds(const p_clear_ds *c) {
    if (!begin_pass() || !S.daspect) return;
    // Diagnostic only: test whether bad colour depends on data surviving a frame's depth clear.
    if (g_x_depth_clear_color && c->depth_mask && S.ncol && S.cfmt[0] == VK_FORMAT_B10G11R11_UFLOAT_PACK32) { p_clear_color z = {0}; z.mask = 0xf; clear_color(&z); }
    VkImageAspectFlags as = (c->depth_mask ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (c->stencil_mask && (S.daspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    as &= S.daspect; if (!as) return;
    VkClearAttachment a = {as, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect r = {scissor_rect(), 0, P_ri.layerCount ? P_ri.layerCount : 1};   // every layer (clear_color)
    if (g_x_tracepass == -2) VKLOG("--- depth-stencil clear: depth %g (mask %d), stencil %d (mask %#x), %ux%u at %d,%d, in pass %d%s", c->depth, (int)c->depth_mask, (int)c->stencil, (unsigned)c->stencil_mask,
        r.rect.extent.width, r.rect.extent.height, r.rect.offset.x, r.rect.offset.y, g_sub_pass - 1 + (S.pass_open == 2), S.pass_open == 2 ? " (before its first draw)" : "");
    if (S.pass_open == 2 && pass_whole(r.rect) && (!(as & VK_IMAGE_ASPECT_STENCIL_BIT) || (c->stencil_mask & 0xff) == 0xff)) {   // into the load ops
        if (as & VK_IMAGE_ASPECT_DEPTH_BIT) { P_da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; P_da.clearValue.depthStencil = a.clearValue.depthStencil; }
        if (as & VK_IMAGE_ASPECT_STENCIL_BIT) { P_sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; P_sa.clearValue.depthStencil = a.clearValue.depthStencil; }
        P_cleared = 1; g_vkst.folded++; return;
    }
    if (!pass_commit()) return;
    if (r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(vk_cmd(), 1, &a, 1, &r);
}

