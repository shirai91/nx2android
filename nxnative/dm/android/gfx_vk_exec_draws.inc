// ---------------- draws ----------------
// What the draws of the command buffer being recorded set last: a draw that would set the same again records nothing. Vulkan keeps dynamic state and
// bindings for the whole command buffer, across render passes and across pipelines that declare the same dynamic state (every one of ours, build_pipeline);
// a new command buffer (g_cmd_gen, gfx_vk_core.c) starts empty. Xenoblade Chronicles 3 docked: ~1,900 draws a frame, each with 10 dynamic-state calls and
// its binds, most the same as the draw before; the driver's time grows with what is recorded (vkEndCommandBuffer too). DM_VK_X_NOBC=1: everything every draw
extern uint64_t g_cmd_gen;
static struct { uint64_t gen; VkViewport vp; VkRect2D sc; float bc[4]; int sm[2], sw[2], sr[2], line, stencil; p_poly_offset po; VkPipelineLayout lay; uint32_t nsets, ndofs;
                VkDescriptorSet sets[5]; uint32_t dofs[3 * 96]; VkBuffer vb[17]; VkDeviceSize vo[17]; VkBuffer ib; VkDeviceSize io; VkIndexType it; } g_bc;

static int bc_fresh(void) { if (g_bc.gen != g_cmd_gen || g_x_nobc || g_bc_stale) { g_bc_stale = 0; memset(&g_bc, 0, sizeof g_bc); g_bc.gen = g_cmd_gen; g_bc.it = (VkIndexType)-1; return 1; } return 0; }
#define BC_SAME(field, val) (!fresh && !memcmp(&g_bc.field, &(val), sizeof g_bc.field))
static void bc_index(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    int fresh = bc_fresh(); if (!fresh && g_bc.ib == b && g_bc.io == o && g_bc.it == t) return;
    vkCmdBindIndexBuffer(cb, b, o, t); g_bc.ib = b; g_bc.io = o; g_bc.it = t;
}
static void set_dynamic(VkCommandBuffer cb) {
    int fresh = bc_fresh();
    float x = S.vp[0][0], y = S.vp[0][1], w = S.vp[0][2], h = S.vp[0][3];
    if (w == 0 && h == 0) { w = (float)S.pass_w; h = (float)S.pass_h; }
    VkViewport vp = {x, y, w, h, S.dnear, S.dfar};
    if (g_lower_left && !g_flipy) { vp.y = (float)S.pass_h - y; vp.height = -h; }   // lower-left viewports count y from the bottom
    else if (!g_lower_left && g_flipy) { vp.y = y + h; vp.height = -h; }        // clip space y up over the same top-left rectangle
    if (!g_depth_zero && !V.has_depth_clip_ctl) { /* GL depth convention without clip control: left as is (rare) */ }
    // Viewport swizzle (Absolum's FNA3D sets NEGATIVE_Y every frame; Advance Wars 1+2's Unity sets it too): NEGATIVE_Y negates clip y, the same as the viewport flipped about
    // its centre; facing is decided after the viewport transform in Vulkan as on NVN, so the winding stays. Other swizzles are drawn unswizzled (nvn_mtl.m does the same)
    { uint32_t sw; memcpy(&sw, S.vp_sw[0], 4);
      if (sw == 0x100) { vp.y += vp.height; vp.height = -vp.height; }
      else if (sw) VKRED_ONCE(81, "viewport swizzle %d,%d,%d,%d not implemented (drawn unswizzled)", S.vp_sw[0][0], S.vp_sw[0][1] ^ 2, S.vp_sw[0][2] ^ 4, S.vp_sw[0][3] ^ 6); }
    if (vp.minDepth < 0) vp.minDepth = 0; if (vp.maxDepth > 1) vp.maxDepth = 1;
    if (!BC_SAME(vp, vp)) { vkCmdSetViewport(cb, 0, 1, &vp); g_bc.vp = vp; }
    VkRect2D sc = scissor_rect(); if (!BC_SAME(sc, sc)) { vkCmdSetScissor(cb, 0, 1, &sc); g_bc.sc = sc; }
    { int32_t vx0 = (int32_t)fmaxf(vp.x, 0), vy0 = (int32_t)fmaxf(fminf(vp.y, vp.y + vp.height), 0), vx1 = (int32_t)(vp.x + vp.width), vy1 = (int32_t)fmaxf(vp.y, vp.y + vp.height);   // what this draw can touch
      int32_t x0 = sc.offset.x > vx0 ? sc.offset.x : vx0, y0 = sc.offset.y > vy0 ? sc.offset.y : vy0, x1 = sc.offset.x + (int32_t)sc.extent.width < vx1 ? sc.offset.x + (int32_t)sc.extent.width : vx1, y1 = sc.offset.y + (int32_t)sc.extent.height < vy1 ? sc.offset.y + (int32_t)sc.extent.height : vy1;
      if (x0 < g_pass_bb[0]) g_pass_bb[0] = x0; if (y0 < g_pass_bb[1]) g_pass_bb[1] = y0; if (x1 > g_pass_bb[2]) g_pass_bb[2] = x1; if (y1 > g_pass_bb[3]) g_pass_bb[3] = y1; }
    if (!BC_SAME(bc, S.blendc)) { vkCmdSetBlendConstants(cb, S.blendc); memcpy(g_bc.bc, S.blendc, sizeof g_bc.bc); }
    if (S.rast_discard) g_bc.stencil = 0; // Ignored stencil state is static in discarded pipelines; re-emit on the next raster draw.
    else {
        if (!g_bc.stencil || !BC_SAME(sm, S.smask)) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.smask[0]); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.smask[1]); memcpy(g_bc.sm, S.smask, sizeof g_bc.sm); }
        if (!g_bc.stencil || !BC_SAME(sw, S.swmask)) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.swmask[0]); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.swmask[1]); memcpy(g_bc.sw, S.swmask, sizeof g_bc.sw); }
        if (!g_bc.stencil || !BC_SAME(sr, S.sref)) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.sref[0]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.sref[1]); memcpy(g_bc.sr, S.sref, sizeof g_bc.sr); }
        g_bc.stencil = 1;
    }
    if (!BC_SAME(po, S.pofs)) { vkCmdSetDepthBias(cb, S.pofs.units, S.pofs.clamp, S.pofs.factor); g_bc.po = S.pofs; }
    if (fresh || !g_bc.line) { vkCmdSetLineWidth(cb, 1.0f); g_bc.line = 1; }
}
static int g_no_cull = -1, g_front_ovr = -2;   // DM_VK_FRONT=0|1 forces the Vulkan front face for every draw (0 = clockwise, 1 = counter-clockwise: k.front, build_pipeline); =2 inverts the rule below for every draw (Criterion's games: Burnout
// Paradise culled its ground and showed cars' back faces, NFS Hot Pursuit drew its cars black; the rule suits NEO). -2: not read
// When the driver refuses a submission (vkEndCommandBuffer), the next ones find the draw it refuses: only draws below a limit are recorded, the limit
// bisected between a submission that passed and one that failed; the draw found is logged with its whole state (vk_bisect_result, gfx_vk_core.c)
int g_bis_on, g_bis_lo, g_bis_hi, g_bis_trace = -1, g_bis_var; static int g_draw_i;   // g_bis_var: the change tried on the found draw (1 depth bias values 0, 2 no depth bias, 3 dummy textures, 4 vertex offset 0, 5 zero vertex buffer, 6 a new pass)
static int draw_allowed(void) { int i = g_draw_i++; if (i == g_bis_trace) return g_bis_var == 7 ? 0 : 2;
    return !g_bis_on || i < (g_bis_lo + g_bis_hi) / 2; }
void vk_bisect_reset_count(void) { g_draw_i = 0; }
static int dbg_draw(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_DBG_DRAW"); v = e ? atoi(e) : 0; } return v; }   // debugging: 1 no draw calls, 2 no vertex buffers, 4 no descriptor sets
// a transfer read of 4 bytes of each buffer the addresses are in (outside a render pass, between barriers): see prepare_draw's runs of rasterizer discard
static void rd_read(const uint64_t *a, int n) {
    VkCommandBuffer cb = vk_cmd(); VkBuffer seen[16]; int ns = 0;
    for (int i = 0; i < n; i++) { VkBuffer b, db; VkDeviceSize o, dof; if (!vk_buf_at(a[i], 4, &b, &o)) continue;
        int dup = 0; for (int j = 0; j < ns; j++) dup |= seen[j] == b; if (dup) continue; seen[ns++] = b;
        vk_scratch(4, 4, &db, &dof); VkBufferCopy r = {o, dof, 4}; vk_img_barrier_all(cb); vkCmdCopyBuffer(cb, b, db, 1, &r); vk_img_barrier_all(cb); }
}
// everything a draw needs bound; 0 when the draw cannot be done (logged once per reason)
// A shader that writes memory: a store through a pointer into a storage buffer, a uniform block (only BufferBlock storage can be written) or an image, an image write, an
// atomic other than a load, a memory copy into such a pointer (SPIR-V scanned once). Without its SPIR-V: assumed to write
static int spv_wmem(const uint32_t *w, uint32_t n) {
    if (!w || n < 5 || w[0] != 0x07230203) return 1;
    uint32_t bound = w[3]; uint8_t *mp = calloc(bound ? bound : 1, 1); if (!mp) return 1;   // 1: a pointer type into memory, 2: a pointer into memory
    int wr = 0;
    for (uint32_t i = 5; i < n && !wr;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16; if (!wc || i + wc > n) break;
        const uint32_t *o = w + i;
        if (op == 32 && wc >= 4 && o[1] < bound && (o[2] == 2 || o[2] == 11 || o[2] == 12 || o[2] == 5349)) mp[o[1]] = 1;   // OpTypePointer: Uniform, Image, StorageBuffer, PhysicalStorageBuffer
        else if ((op == 59 || op == 65 || op == 66 || op == 67 || op == 60) && wc >= 3 && o[1] < bound && o[2] < bound && mp[o[1]] == 1) mp[o[2]] = 2;   // OpVariable, access chains, OpImageTexelPointer
        else if (op == 62 && wc >= 3 && o[1] < bound && mp[o[1]] == 2) wr = 1;   // OpStore
        else if (op == 63 && wc >= 3 && o[1] < bound && mp[o[1]] == 2) wr = 1;   // OpCopyMemory
        else if (op == 99) wr = 1;   // OpImageWrite
        else if ((op >= 228 && op <= 242) || op == 5614 || op == 5615 || op == 6035) wr = 1;   // atomics but OpAtomicLoad (227)
        i += wc;
    }
    free(mp); return wr;
}
static int shader_wmem(vkshader_t *s) { if (!s) return 0; if (!s->wmem) s->wmem = spv_wmem(s->spv, s->nspv) ? 1 : 2; return s->wmem == 1; }
static int order_feedback(int nst) {   // a stage of the draw samples one of the open pass's attachments (its images by their storage)
    for (int st = 0; st < nst; st++) {
        if (!S.sh[st]) continue;
        if (g_stn[st] > 32) return 1;
        for (int j = 0; j < g_stn[st]; j++) for (int a = 0; a < 9; a++) { const vkimg_t *im = P_att[a].im;
            if (im && im->pool == g_stimg[st][j].pool && g_stimg[st][j].off < im->off + im->size && im->off < g_stimg[st][j].off + g_stimg[st][j].size) return 1; }
    }
    return 0;
}
// DM_VK_TEXBIND=<shader key>[,<key>...]: once per matching pipeline, every texture the draw has bound, with the memory it lives in. vk_tex_by_id answers
// whether a texture is registered and fills the descriptor; the image itself is not made here (making one would upload it): the pool and offset in the
// descriptor are what DM_VK_TEXDUMP=<pool>+<off> takes to write the texture's content out.
static void texbind(const pkey *k) {
    // DM_VK_TEXBIND=<shader key>[,<key>...]  or  DM_VK_TEXBIND=all (every draw that samples something, one group per pixel shader).
    // The handles in the slots are what the draw bound; vk_tex_by_id answers whether a texture is registered and fills its descriptor, without making the
    // image (making one would upload it). The descriptor's pool and offset are what DM_VK_TEXDUMP=<pool>+<off> takes to write the texture's content out.
    static int on = -1, all, nk, nd; static uint64_t keys[8], done[48];
    if (on < 0) {
        on = 0; const char *e = getenv("DM_VK_TEXBIND");
        if (e && (!strcmp(e, "all") || !strcmp(e, "ALL"))) all = on = 1;
        else if (e) { for (char *q = (char *)e; *q && nk < 8; ) { char *nx; uint64_t v = strtoull(q, &nx, 16); if (nx == q) break; keys[nk++] = v; q = *nx == ',' ? nx + 1 : nx; } on = nk > 0; }
        if (on) VKLOG("texbind: %s, first draw's shaders %016llx %016llx", all ? "every draw with textures" : "the named shaders", (unsigned long long)k->key[0], (unsigned long long)k->key[1]);
    }
    if (!on) return;
    if (!all) {
        int hit = 0; for (int i = 0; i < nk; i++) if (keys[i] == k->key[0] || keys[i] == k->key[1]) hit = 1; if (!hit) return;
    } else {   // a draw that samples nothing, or only single-level render targets, is post-process or UI, not a material: skip it, so the groups are the game's textures
        int mipped = 0;
        for (int st = 0; st < 3 && !mipped; st++) for (int slot = 0; slot < 256 && !mipped; slot++) {
            uint64_t h = vk_tex_slot_handle(S.tex[st], S.img[st], (unsigned)slot); if (!h) continue;
            int tid = (int)(h & 0xfffff); const view_t *v = NULL; const tex_t *t = NULL;
            if (tid && vk_tex_by_id(tid, &v, &t) && t && t->d.levels > 1) mipped = 1;
        }
        if (!mipped) return;
    }
    uint64_t sig = k->key[1] ? k->key[1] : k->key[0];   // one group per pixel shader (per vertex shader for a depth-only draw)
    for (int i = 0; i < nd; i++) if (done[i] == sig) return;
    if (nd >= 48) return; done[nd++] = sig;
    // DM_VK_TEXBIND_DUMP=<group>: write that group's first mipmapped texture's storage, block-linear as the GPU has it. The pool pointer changes every launch, so
    // DM_VK_TEXDUMP=<pool>+<off> cannot be aimed at a texture seen in an earlier run; this and the descriptor in the log let it be deswizzled offline
    const char *de = getenv("DM_VK_TEXBIND_DUMP"); int dump_group = de && *de ? atoi(de) : -1, dumped = 0;
    for (int st = 0; st < 3; st++) for (int slot = 0; slot < 256; slot++) {
        uint64_t h = vk_tex_slot_handle(S.tex[st], S.img[st], (unsigned)slot); if (!h) continue;
        int tid = (int)(h & 0xfffff); const view_t *v = NULL; const tex_t *t = NULL;
        if (!tid || !vk_tex_by_id(tid, &v, &t) || !t) { VKLOG("texbind %016llx/%016llx stage %d slot %d: handle %#llx, not registered", (unsigned long long)k->key[0], (unsigned long long)k->key[1], st, slot, (unsigned long long)h); continue; }
        VKLOG("texbind %016llx/%016llx stage %d slot %d id %d: %ux%u nvn format %#x target %d levels %u flags %#x stride %u serial %llu pool %p+%#llx size %#llx",
              (unsigned long long)k->key[0], (unsigned long long)k->key[1], st, slot, tid, t->d.width, t->d.height, t->d.format, t->d.target,
              t->d.levels, t->d.flags, t->d.stride, (unsigned long long)t->serial, t->pool, (unsigned long long)t->off, (unsigned long long)t->L.size);
        int dump_this = de && (!strcmp(de, "all") ? (slot < 6) : ((int)nd - 1 == dump_group && t->d.levels > 1));
        if (dump_this && t->mem && t->L.size && !(de && strcmp(de, "all") && dumped)) {
            const char *root = getenv("DM_ROOT"); char path[1024];
            snprintf(path, sizeof path, "%s/texbind_%016llx_%016llx_s%d.bin", root ? root : ".", (unsigned long long)k->key[0], (unsigned long long)k->key[1], slot);
            FILE *df = fopen(path, "wb");
            if (df) { fwrite(t->mem, 1, t->L.size, df); fclose(df);
                VKLOG("texbind dump: %s: %llu bytes, %ux%u nvn format %#x levels %u flags %#x (deswizzle block-linear with these)", path, (unsigned long long)t->L.size, t->d.width, t->d.height, t->d.format, t->d.levels, t->d.flags); }
            else VKLOG("texbind dump: cannot write %s", path);
            dumped = 1;   // (single-texture mode: one per group)
        }
    }
}
static int prepare_draw(int prim, int indexed) {
    // DM_VK_CB_DRAWS (read every submission, default 128, 0 = never): the replay is cut every that many draws and what is recorded goes to the GPU at once
    // (vk_cmd_split), so the GPU works on the frame while the rest is replayed instead of after all of it: NEO's Scramble Crossing 36.7 → 15.7 ms a frame
    g_cb_draws++;   // the cut itself happens between passes (begin_pass); inside one only with DM_VK_PASS_SPLIT (below): a pass cut in two stores and reloads its attachments
    g_tracing = g_draw_i == g_bis_trace && g_bis_trace >= 0;
    // the change being tried applies to the found draw and every later one (the later draws are refused too: changing one draw alone proves nothing)
    int var = g_bis_trace >= 0 && g_draw_i >= g_bis_trace ? g_bis_var : 0; if (g_bis_var) g_tracing = 0;   // full logs only the first time
    g_var_now = 0;
    if (var == 6) vk_end_pass();   // a new pass for this draw
    // A program may have no fragment stage (a depth-only draw: NVN binds by stage mask, and Metroid Dread draws about nine a frame so in its intro; nvn_mtl.m makes them vertex-only pipelines):
    // drawn with no fragment shader and no colour written (Vulkan leaves colour outputs undefined without one), as before only rasterizer-discard draws were
    if (!S.sh[0]) { g_vkst.skip_prog++; VKRED_ONCE(50, "draw with a program whose shaders are not in the bundle: skipped"); return 0; }
    if (g_x_nogs && S.sh[2]) return 0;   // DM_VK_X_NOGS=1 (live, A/B): draws with a geometry shader are left out
    for (int i = 0; i < g_x_nskipvs; i++) if (S.sh[0]->key == g_x_skipvs[i]) return 0;   // DM_VK_X_SKIPVS=<key>[,<key>...] (live, up to 8): draws with these vertex shaders are left out
    for (int i = 0; i < g_x_nskipfs; i++) if (S.sh[1] && S.sh[1]->key == g_x_skipfs[i]) return 0;
    // A run of draws with rasterizer discard gets a pass of its own: the pass ends where such a run begins and where it ends (and begin_pass puts a barrier
    // before the next). They are vertex shaders that write memory the later draws read: Xenoblade Chronicles 3 transforms its meshes with point draws
    // (724ddce6 then 37d04447, cf66cef4), which share passes with its G-buffer draws. NVN's GPU runs draws in order; Adreno, a tiled GPU, runs the vertex
    // shaders of a pass's draws in its binning pass, and draws read vertices not yet written: thin black slivers over the cliffs and grass in 17-20 of
    // 361 frames, 0 with this (0 also with the draws of 37d04447 and cf66cef4 left out). A pass for each such draw instead, splitting the runs, lost
    // Noah's hair (724ddce6's draws come in pairs in one pass); the run kept together draws it. About +1 ms of replay, +0.1 ms of GPU a frame, 33.2 ms
    // frames held. DM_VK_X_RDKEEP=1 (live, A/B): they stay in the pass
    // Where a run ends, the memory its shaders wrote is read by a transfer (rd_read): with barriers alone, the draws of the next passes still met vertices the
    // run had not written yet. Xenoblade Chronicles 3 sways its grass with point draws (5f6491f9 for its depth pre-pass, c0882609 for its G-buffer) whose records
    // the grass cards draw from, the G-buffer's with depth EQUAL against the pre-pass: whole blades failed the test and stayed black, a different few each frame
    // (the grass flicker, stable on the Switch). Grass blades the G-buffer never wrote: 900-2,300 a frame, 0 with this (8 dumps); flicker on screen 4.2-7.7
    // spiking blocks a frame, 0.0-0.5. Everything the two passes read matched (records over 60 frames, instances, index data, shader code: the G-buffer's
    // vertex shader drawing the pre-pass too changed nothing), and a full GPU wait after each submission, explicit barrier bits or a barrier to vertex input
    // did not help; drawing from a copy of the records did, as did a command buffer cut or a transfer read of the written buffer at either end of the run.
    // About +0.45 ms of GPU a frame, 33.2 ms frames held. DM_VK_RD_TOUCH=0 (live, A/B): no read
    { static int prev; static uint64_t wa[16]; static int nwa; int rd = S.rast_discard; if (!g_x_rdkeep && rd != prev && S.pass_open) vk_end_pass();
      if (rd) { if (!prev) nwa = 0; for (int r = 0; r < 16 && nwa < 16; r++) { uint64_t x = S.ssbo[0][r].addr; int have = 0; for (int i = 0; i < nwa; i++) have |= wa[i] == x; if (x && !have) wa[nwa++] = x; } }
      else if (prev && g_rd_touch && !S.pass_open) rd_read(wa, nwa);
      prev = rd; }
    // DM_VK_PASS_SPLIT=n (live, 0 = off): a pass is ended after every n of its draws and begun again on the same attachments (every load op LOAD: clears folded into
    // the first part's load ops are not run again), with a cut before it (begin_pass, g_psplit_now), so its first parts reach the GPU while the replay records the rest:
    // DM_VK_CB_DRAWS cuts only between passes, and Unicorn Overlord's battle map draws ~2,030 of its ~3,546 draws in one 1280x720 pass (13.3 ms of GPU, all after its
    // replay: the 15.5 ms GPU tail). Not inside a run of rasterizer discard (it stays in one pass, see above), and only a pass without depth: a pass that goes on with a
    // depth buffer begun in an earlier command buffer meets Adreno's stale LRZ (begin_pass). DM_VK_PASS_SPLIT_DEPTH=1 (live, A/B) splits those too, with begin_pass's
    // write-back of a depth texel (lrz_touch) before the second part. The pipeline, dynamic state, sets and buffers are bound again (vk_end_pass, g_cmd_gen); deferred
    // uniform updates stay deferred (the draws read their shadows), as at any cut. Only while vk_cmd_split can still begin a part: otherwise the pass would be cut for nothing
    if (g_psplit > 0 && g_x_cbdraws > 0 && S.pass_open == 1 && g_pass_draws >= (uint64_t)g_psplit && !S.rast_discard && (!S.daspect || g_psplit_depth)) {
        int vk_cmd_parts_left(void); if (vk_cmd_parts_left() > 0) { { static int n; if (n++ < 3) VKLOG("pass split: pass %d (%ux%u) after %llu draws, depth %d", g_sub_pass - 1, S.pass_w, S.pass_h, (unsigned long long)g_pass_draws, S.dfmt); }
            vk_end_pass(); g_psplit_now = 1; } }
    else if (g_psplit > 0 && S.pass_open == 1 && g_pass_draws == (uint64_t)g_psplit && S.daspect && !g_psplit_depth) { static int n; if (n++ < 3) VKLOG("pass split: pass %d (%ux%u) has depth (format %d): not split", g_sub_pass - 1, S.pass_w, S.pass_h, S.dfmt); }
    { uint64_t fs = S.sh[1] ? S.sh[1]->key : 0; if (g_x_cutfs && S.pass_open == 1 && (fs == g_x_cutfs || g_prev_fs == g_x_cutfs)) vk_end_pass(); g_prev_fs = fs; }
    if (g_order_pending) { g_order_pending = 0; g_order_check = S.pass_open == 1; }   // an order-only barrier kept the pass open: decided below, once the draw's images are known
    if (!begin_pass() || !pass_commit()) { g_vkst.skip_rt++; return 0; }
    if (g_x_tracepass >= 0 && g_sub_pass - 1 == g_x_tracepass) g_tracing = 1;
    uint64_t t0 = tick();
    VkCommandBuffer cb = vk_cmd();
    if (g_no_cull < 0) g_no_cull = getenv("DM_VK_NOCULL") != NULL;
    pkey k; memset(&k, 0, sizeof k);
    for (int i = 0; i < 3; i++) k.key[i] = S.sh[i] ? S.sh[i]->key : 0;
    if (g_x_listpasses && (g_list_vs != k.key[0] || g_list_fs != k.key[1])) {
        VKLOG("    shaders at draw %llu: vs %016llx fs %016llx", (unsigned long long)g_pass_draws, (unsigned long long)k.key[0], (unsigned long long)k.key[1]); g_list_vs = k.key[0]; g_list_fs = k.key[1];
    }
    for (int i = 0; i < g_x_nflat; i++) if (k.key[0] == g_x_flat[i]) k.debug_mode = (uint32_t)g_x_flat_mode[i];
    k.topo = topo(prim); k.restart = S.prim_restart && (k.topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || k.topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || k.topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) && indexed;
    if (g_front_ovr < -1) { const char *e = getenv("DM_VK_FRONT"); g_front_ovr = e ? (atoi(e) == 2 ? 2 : atoi(e) ? 1 : 0) : -1; }   // =2: the rule's answer inverted (a game that uses both windings)
    k.raster_discard = S.rast_discard; k.alt = (uint32_t)g_spv_alt; k.gs_split = (uint32_t)g_gs_split_mode; k.cull = g_no_cull ? 0 : S.poly.cull;
    // counter-clockwise front faces in Vulkan's framebuffer: Vulkan's clip space has y down, Metal's up (nvn_mtl.m: CCW when (front == CCW) != lower left), and the
    // lower-left origin mode flips the viewport again, so CCW when (front == CCW) == lower left. Taken as given, upper-left games (NEO) culled their ground and walls
    k.front = g_front_ovr == 0 || g_front_ovr == 1 ? (uint32_t)g_front_ovr : (((S.poly.front == NVN_FRONT_FACE_CCW) == (g_lower_left != 0)) != (g_flipy != 0)) ^ (g_front_ovr == 2);
    static int plog = -1; if (plog < 0) plog = getenv("DM_VK_PIPE_LOG") ? atoi(getenv("DM_VK_PIPE_LOG")) > 0 : 0;   // the polygon-state lines come with DM_VK_PIPE_LOG: unset costs one branch per draw
    if (plog) { static int n; if (n < 16) { static int seen[16]; int b = (S.poly.cull & 3) << 2 | (S.poly.front & 1) << 1 | (g_lower_left != 0); if (!seen[b]) { seen[b] = 1; n++;
        VKLOG("polygon state: cull %d, front %d, lower-left origin %d, flipy %d → Vulkan cull %u front %u (DM_VK_FRONT=%d, DM_VK_NOCULL=%d)",
              (int)S.poly.cull, (int)S.poly.front, g_lower_left != 0, g_flipy != 0, k.cull, k.front, g_front_ovr, g_no_cull); } } }
    k.mode = S.poly.mode; k.depth_clamp = S.depth_clamp;
    k.bias_en = (S.poly.offset_en & (S.poly.mode == 0 ? 1 : S.poly.mode == 1 ? 2 : 4)) != 0;
    if (var == 2) k.bias_en = 0;
    if (var == 8) k.raster_discard = 1;   // no fragment shader in the pipeline
    int has_depth = (S.daspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, has_stencil = (S.daspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    k.dt = has_depth && S.ds.dt; k.dw = S.ds.dw; k.df = S.ds.df; k.st = has_stencil && S.ds.st;
    static int npl; if (g_x_logfs && S.sh[1] && S.sh[1]->key == g_x_logfs && npl++ < 60) VKLOG("logfs: present %llu pass %d (%ux%u, %llu draws before): depth attachment %d test %d (state %d) func %u write %u; depth range %.9g %.9g; viewport %.1f %.1f %.1f %.1f",
        (unsigned long long)g_tex_frame, g_sub_pass - 1, S.pass_w, S.pass_h, (unsigned long long)g_pass_draws, has_depth, k.dt, S.ds.dt, k.df, k.dw, S.dnear, S.dfar, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3]);
    // A depth buffer that a fragment shader wrote through gl_FragDepth: Adreno's early culling (LRZ) then rejects whole triangles of later draws that test it, in some frames
    // and not others, with the same draws, state and depth contents. Hades II downsamples its scene depth into a 640x360 D24S8 with such a shader (Blit2DDepth), then
    // draws its lights into it with the depth test on and no depth write: the lights went out in turn every 24-48 frames (whole halves of light quads missing in pass dumps),
    // and Xenoblade Chronicles 3's cloud dome (its depth downsampled the same way) lost whole triangles. The light shader rewritten to write gl_FragDepth = gl_FragCoord.z
    // (the same depth, tested after the shader) brought the pulses to 0 in 5 recordings, against 8-21; neither writing the depth image back by transfer (one texel or the
    // whole image) nor waiting for the GPU after each submission changed them. So draws that test such a buffer (written in the last two presents) use that variant.
    // Not with a depth bias (gl_FragCoord.z need not carry it). DM_VK_FDZ=0 (live): never
    { vkimg_t *dim = has_depth ? P_att[8].im : NULL; vkshader_t *fsh = k.raster_discard ? NULL : S.sh[1];
      if (dim && fsh && fsh->depth_repl && k.dt && k.dw) dim->fdz_frame = g_tex_frame + 1;
      k.fdz = g_fdz && dim && fsh && !fsh->depth_repl && k.dt && !k.bias_en && dim->fdz_frame && g_tex_frame + 1 - dim->fdz_frame <= 2; }
    k.sf[0] = S.ds.sf_f; k.sf[1] = S.ds.sf_b; k.sfail[0] = S.ds.fail_f; k.szfail[0] = S.ds.zfail_f; k.szpass[0] = S.ds.zpass_f; k.sfail[1] = S.ds.fail_b; k.szfail[1] = S.ds.zfail_b; k.szpass[1] = S.ds.zpass_b;
    // vertex input: every location the vertex shader reads
    vkshader_t *vsh = S.sh[0]; uint32_t in_mask = vsh ? vsh->local[0] : 0, int_mask = vsh ? vsh->local[1] | vsh->local[2] : 0, sint_mask = vsh ? vsh->local[1] : 0;
    uint32_t used_streams = 0; int need_zero = 0;
    { static int vl_read, done; static uint64_t vl_key;   // DM_VK_VATTR_LOG=<vertex shader key>: once, the vertex inputs the draw really has. Read once (start-up): one branch per draw when unset
      if (!vl_read) { const char *vl = getenv("DM_VK_VATTR_LOG"); vl_key = vl && *vl ? strtoull(vl, NULL, 16) : 0; vl_read = 1; }
      if (vl_key && !done && S.sh[0] && S.sh[0]->key == vl_key) { done = 1;
          VKLOG("vattr: VS %016llx reads locations %#x", (unsigned long long)S.sh[0]->key, in_mask);
          for (int loc = 0; loc < 16; loc++) { if (!(in_mask >> loc & 1)) continue; va_st a; memcpy(&a, &S.vattr[loc], 4);
              VKLOG("vattr: loc %d fmt %#x stream %d offset %u | stream stride %u divisor %u | buffer %p", loc, a.fmt, a.stream, a.offset,
                    S.vstream[a.stream].stride, S.vstream[a.stream].divisor, (void *)(uintptr_t)S.vbuf[a.stream].address); } } }
    for (int loc = 0; loc < 16; loc++) {
        if (!(in_mask >> loc & 1)) continue;
        va_st a; memcpy(&a, &S.vattr[loc], 4);
        VkFormat f = a.fmt ? vertex_format(a.fmt, int_mask >> loc & 1, sint_mask >> loc & 1) : VK_FORMAT_UNDEFINED;
        if (f == VK_FORMAT_UNDEFINED || !S.vbuf[a.stream].address) { k.attr[k.nattr++] = (typeof(k.attr[0])){(uint32_t)loc, 16, int_mask >> loc & 1 ? (sint_mask >> loc & 1 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT) : VK_FORMAT_R32G32B32A32_SFLOAT, a.fmt ? 16u : 0u}; need_zero = 1; continue; }   // disabled format: (0,0,0,1); invalid/unbacked enabled input: zero fallback in the next 16 bytes
        k.attr[k.nattr++] = (typeof(k.attr[0])){(uint32_t)loc, a.stream, f, a.offset};
        used_streams |= 1u << a.stream;
    }
    for (int s = 0; s < 16; s++) if (used_streams >> s & 1) k.bind[k.nbind++] = (typeof(k.bind[0])){(uint32_t)s, S.vstream[s].stride, S.vstream[s].divisor ? 1u : 0u, S.vstream[s].divisor};
    if (need_zero) k.bind[k.nbind++] = (typeof(k.bind[0])){16, 0, 0, 0};
    // attachments and blending
    k.ncol = (uint32_t)S.ncol; k.dfmt = S.dfmt; k.samples = S.samples ? S.samples : 1; k.a2c = S.ms.a2c;
    k.logic_en = S.color.logic_op != 3 && S.color.logic_op != 0 && 0;   // logic ops: not on mobile render targets in version 1
    k.logic_op = S.color.logic_op;
    uint32_t outputs = S.sh[1] ? ((k.debug_mode & 1) ? 0xffu : S.sh[1]->out_locs) : 0;
    for (int i = 0; i < S.ncol; i++) {
        k.cfmt[i] = S.cfmt[i];
        k.wmask[i] = S.cfmt[i] && S.sh[1] ? (S.chmask.mask >> (4 * i)) & 0xf : 0;   // no fragment shader: no colour written
        // Upstream seedds/nxnative 742318ae color_operations: Maxwell leaves
        // absent outputs untouched, as nvn_mtl.m does for Metal colour attachments.
        if (!(outputs & (1u << i))) { k.wmask[i] = 0; continue; }
        const vkfmt_t *f = S.rt_has_col[i] ? vk_fmt(S.rt_col[i].d.format) : NULL;
        if (((S.color.blend_en >> i) & 1) && f && !f->is_int) {
            k.blend_en |= 1u << i;
            k.bl[i].src = S.blend[i].src_rgb; k.bl[i].dst = S.blend[i].dst_rgb; k.bl[i].srca = S.blend[i].src_a; k.bl[i].dsta = S.blend[i].dst_a; k.bl[i].eq = S.blend[i].eq_rgb; k.bl[i].eqa = S.blend[i].eq_a;
        }
    }
    if (S.ncol > 1 && P_att[0].im == &g_s0_image) { k.wmask[0] = 0; k.blend_en &= ~1u; }   // slot 0's scratch (DM_VK_SLOT0_FILL): nothing written there
    // Dual-source blending: NVN blends colour target 0 with the shader's output 1 as its second source (factors SRC1_*, 0x10-0x13), the translator declares that output at
    // location 1, and Vulkan takes the second source only from location 0, index 1 (with location 1 the blend reads undefined values: Pokémon Legends: Arceus's post effects,
    // shader ha_post_effects variation 4, washed the whole picture out; the validation layer named it). Such a pipeline takes the fragment shader's dual-source variant
    #define SRC1F(x) ((x) >= 0x10 && (x) <= 0x13)
    k.dual = (k.blend_en & 1) && S.sh[1] && !k.raster_discard && (SRC1F(k.bl[0].src) || SRC1F(k.bl[0].dst) || SRC1F(k.bl[0].srca) || SRC1F(k.bl[0].dsta));
    #undef SRC1F
    k.clip_neg1 = !g_depth_zero;
    // layout and pipeline
    VkDescriptorSetLayout dsl[5] = {0}; int nsets = 0;
    for (int st = 0; st < 3; st++) if (S.sh[st] && !(st == 1 && k.raster_discard)) { dsl[S.sh[st]->set] = S.sh[st]->dsl; if (S.sh[st]->set + 1 > nsets) nsets = S.sh[st]->set + 1; }
    // the last draw's set layouts and what vk_pipeline_layout made of them (it fills absent stages with the empty layout): its lock and hash took ~10% of the
    // replay thread on Unicorn Overlord's battle map. Pipeline layouts are never destroyed
    static VkDescriptorSetLayout l_in[5], l_out[5]; static int l_n = -1; static VkPipelineLayout l_lay; VkPipelineLayout layout;
    if (nsets == l_n && !memcmp(dsl, l_in, sizeof l_in)) { memcpy(dsl, l_out, sizeof l_out); layout = l_lay; }
    else { memcpy(l_in, dsl, sizeof l_in); l_n = nsets; layout = l_lay = vk_pipeline_layout(dsl, nsets); memcpy(l_out, dsl, sizeof l_out); }
    // descriptor sets first: the views they are given decide which shader variants the pipeline takes (integer texels, vk_shader_module)
    g_var_now = var;
    VkDescriptorSet sets[5] = {0};
    uint32_t dofs[5][96], ndofs = 0, dall[3 * 96];
    uint64_t t1d = tick();
    for (int st = 0; st < 3; st++) if (S.sh[st] && !(st == 1 && k.raster_discard)) {
        uint64_t m0 = g_set_memo; int n0 = g_stn[st]; g_stn[st] = 0; g_coll_st = st;   // the images it is collected with (vk_watch); a memoized set keeps the last ones
        sets[S.sh[st]->set] = stage_set(st, S.sh[st], dofs[S.sh[st]->set], NULL); k.um[st] = g_num_um; k.im[st] = g_num_im;
        g_coll_st = -1; if (g_set_memo != m0) g_stn[st] = n0; }
    if (g_order_check) {   // the order-only barrier before this draw (see g_order_keep): kept, or the pass ends here with it
        g_order_check = 0; vkshader_t *fs = k.raster_discard ? NULL : S.sh[1];
        if (g_pass_wmem || shader_wmem(S.sh[0]) || shader_wmem(fs) || shader_wmem(S.sh[2]) || order_feedback(3)) {
            g_order_ended++; vk_end_pass(); vk_img_barrier_all(vk_cmd());
            if (!begin_pass() || !pass_commit()) { g_vkst.skip_rt++; return 0; }
            cb = vk_cmd();
        } else g_order_kept++;
    }
    g_pass_wmem |= shader_wmem(S.sh[0]) | shader_wmem(k.raster_discard ? NULL : S.sh[1]) | shader_wmem(S.sh[2]);
    uint64_t tdesc = tick() - t1d; g_vkst.ns_desc += tdesc; if (g_prof_on) g_prof_ns[5] += tdesc;
    if (g_tracing) {
        VKLOG("  pipeline: topology %u restart %u discard %u cull %u front %u mode %u clamp %u bias %u; depth test %u write %u func %u stencil %u; attributes %u bindings %u; colour %u (format %u) samples %u a2c %u; blend enable %#x logic %u/%u; fragment depth variant %u (depth written by a shader at present %llu, now %llu)",
              k.topo, k.restart, k.raster_discard, k.cull, k.front, k.mode, k.depth_clamp, k.bias_en, k.dt, k.dw, k.df, k.st, k.nattr, k.nbind, k.ncol, k.cfmt[0], k.samples, k.a2c, k.blend_en, k.logic_en, k.logic_op,
              k.fdz, has_depth && P_att[8].im ? (unsigned long long)P_att[8].im->fdz_frame : 0ull, (unsigned long long)g_tex_frame + 1);
        if (k.st) VKLOG("  stencil: func %u/%u fail %u/%u zfail %u/%u pass %u/%u; compare mask %#x/%#x write mask %#x/%#x reference %#x/%#x", k.sf[0], k.sf[1], k.sfail[0], k.sfail[1], k.szfail[0], k.szfail[1], k.szpass[0], k.szpass[1],
                        (unsigned)S.smask[0], (unsigned)S.smask[1], (unsigned)S.swmask[0], (unsigned)S.swmask[1], (unsigned)S.sref[0], (unsigned)S.sref[1]);
        for (uint32_t i = 0; i < k.ncol; i++) VKLOG("  blend %u: src %#x dst %#x eq %u, alpha src %#x dst %#x eq %u, write mask %#x", i, k.bl[i].src, k.bl[i].dst, k.bl[i].eq, k.bl[i].srca, k.bl[i].dsta, k.bl[i].eqa, k.wmask[i]);
        for (uint32_t i = 0; i < k.nattr; i++) VKLOG("  attribute location %u binding %u format %u offset %u", k.attr[i].loc, k.attr[i].binding, k.attr[i].fmt, k.attr[i].off);
        for (uint32_t i = 0; i < k.nbind; i++) VKLOG("  binding %u stride %u rate %u divisor %u", k.bind[i].binding, k.bind[i].stride, k.bind[i].rate, k.bind[i].divisor);
    }
    crumb_draw(k.key[0], k.key[1], k.key[2]);
    VkPipeline p = graphics_pipeline(&k, layout);
    uint64_t t1 = tick(); g_vkst.ns_pipe += t1 - t0 - tdesc;
    if (!p) { g_vkst.skip_pipe++; return 0; }
    texbind(&k);
    if (g_ubolog_key && g_ubolog_n > 0 && S.sh[0] && S.sh[0]->key == g_ubolog_key) {   // DM_VK_UBO_LOG: the pipeline the draw got, and its key's hash
        uint64_t kh = fnv(1469598103934665603ull, &k, sizeof k); VKLOG("ubo: present %llu pipeline %p key %016llx um %llx/%llx im %llx/%llx", (unsigned long long)g_tex_frame, (void *)p, (unsigned long long)kh,
              (unsigned long long)k.um[0], (unsigned long long)k.um[1], (unsigned long long)k.im[0], (unsigned long long)k.im[1]); }
    g_vkst.draws++; if (++g_pass_draws > g_vkst.max_pass_draws) g_vkst.max_pass_draws = g_pass_draws;
    if (p != S.bound_pipe) { uint64_t tb = g_prof_on ? tick() : 0; vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); S.bound_pipe = p; if (g_prof_on) g_prof_ns[4] += tick() - tb; }
    set_dynamic(cb);
    if (var == 1) { vkCmdSetDepthBias(cb, 0, 0, 0); memset(&g_bc.po, 0xff, sizeof g_bc.po); }
    uint32_t at[6] = {0};   // where each set's dynamic offsets start in dall
    for (int i = 0; i < nsets; i++) { at[i] = ndofs; for (int st = 0; st < 3; st++)   // the dynamic offsets: by set, then binding
        if (S.sh[st] && !(st == 1 && k.raster_discard) && S.sh[st]->set == i) { memcpy(dall + ndofs, dofs[i], sizeof(uint32_t) * (size_t)S.sh[st]->ndyn); ndofs += (uint32_t)S.sh[st]->ndyn; } }
    at[nsets] = ndofs;
    for (int i = 0; i < nsets; i++) if (!sets[i]) sets[i] = vk_alloc_set(dsl[i]);   // an absent stage's set: from the empty layout vk_pipeline_layout filled in
    // DM_VK_SETSPLIT=1 (live): under the same layout, only the run of sets whose handle or dynamic offsets changed is bound again. Adreno's vkCmdBindDescriptorSets
    // took ~22% of the replay thread on Unicorn Overlord's battle map (one load in it), and a draw that moves only its vertex stage's offsets sent the fragment set too
    if (!(dbg_draw() & 4) && g_setsplit && !bc_fresh() && g_bc.lay == layout && g_bc.nsets == (uint32_t)nsets && g_bc.ndofs == ndofs) {
        int lo = -1, hi = -1;
        for (int i = 0; i < nsets; i++) if (g_bc.sets[i] != sets[i] || memcmp(g_bc.dofs + at[i], dall + at[i], sizeof *dall * (at[i + 1] - at[i]))) { if (lo < 0) lo = i; hi = i; }
        if (lo >= 0) { vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, (uint32_t)lo, (uint32_t)(hi - lo + 1), sets + lo, at[hi + 1] - at[lo], dall + at[lo]);
            memcpy(g_bc.sets + lo, sets + lo, sizeof *sets * (size_t)(hi - lo + 1)); memcpy(g_bc.dofs + at[lo], dall + at[lo], sizeof *dall * (at[hi + 1] - at[lo])); } }
    else if (!(dbg_draw() & 4)) { int fresh = bc_fresh();
        if (fresh || g_bc.lay != layout || g_bc.nsets != (uint32_t)nsets || g_bc.ndofs != ndofs || memcmp(g_bc.sets, sets, sizeof *sets * (size_t)nsets) || memcmp(g_bc.dofs, dall, sizeof *dall * ndofs)) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, (uint32_t)nsets, sets, ndofs, dall);
            g_bc.lay = layout; g_bc.nsets = (uint32_t)nsets; g_bc.ndofs = ndofs; memcpy(g_bc.sets, sets, sizeof *sets * (size_t)nsets); memcpy(g_bc.dofs, dall, sizeof *dall * ndofs); } }
    // vertex buffers
    for (uint32_t i = 0; i < k.nbind; i++) {
        uint32_t b = k.bind[i].binding; VkBuffer buf; VkDeviceSize off;
        if (b == 16) { buf = vk_default_attribute_buffer(); off = 0; }
        else if (!vk_buf_at(S.vbuf[b].address, 1, &buf, &off)) { buf = vk_zero_buffer(); off = 0; VKRED_ONCE(51, "vertex buffer outside the memory pools"); }
        else if (S.vbuf[b].size) { vkpool_t *vp = vk_pool_of(S.vbuf[b].address, 1); if (vp && S.vbuf[b].address + S.vbuf[b].size > vp->hi && vp->hi < vp->end) vk_cross(2, S.vbuf[b].address, S.vbuf[b].size, vp); }
        if (g_var_now == 5) { buf = vk_zero_buffer(); off = 0; }
        if (!(dbg_draw() & 2) && b < 17) { int fresh = bc_fresh(); if (fresh || g_bc.vb[b] != buf || g_bc.vo[b] != off) { vkCmdBindVertexBuffers(cb, b, 1, &buf, &off); g_bc.vb[b] = buf; g_bc.vo[b] = off; } }
    }
    (void)indexed;
    return 1;
}
// quads without an index buffer: one buffer of 0 1 2 0 2 3, 4 5 6 4 6 7, ... for up to QUAD_MAX quads (more are drawn as the first QUAD_MAX)
#define QUAD_MAX 65536u
static VkBuffer quad_index_buffer(uint32_t *nq) {
    static VkBuffer b; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (*nq > QUAD_MAX) { VKRED_ONCE(58, "quad draw of %u quads: the first %u drawn", *nq, QUAD_MAX); *nq = QUAD_MAX; }
    pthread_mutex_lock(&mu);
    if (!b) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, QUAD_MAX * 6 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &b)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        VkDeviceMemory m; VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &m)); VKCHK(vkBindBufferMemory(V.dev, b, m, 0));
        uint32_t *p; VKCHK(vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&p));
        static const uint32_t pat[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t q = 0; q < QUAD_MAX; q++) for (int k = 0; k < 6; k++) p[q * 6 + k] = q * 4 + pat[k];
    }
    pthread_mutex_unlock(&mu);
    return b;
}
// quads: a triangle list of indices made on the CPU (from the game's index buffer for indexed draws)
static int quad_indices(uint32_t count, const uint8_t *src, int itype, int32_t first, VkBuffer *buf, VkDeviceSize *off) {
    uint32_t nq = count / 4; if (!nq) return 0;
    uint32_t *d = vk_scratch((VkDeviceSize)nq * 6 * 4, 4, buf, off);
    for (uint32_t q = 0; q < nq; q++) for (int k = 0; k < 6; k++) {
        static const int pat[6] = {0, 1, 2, 0, 2, 3}; uint32_t i = q * 4 + (uint32_t)pat[k];
        d[q * 6 + k] = !src ? (uint32_t)first + i : itype == 0 ? src[i] : itype == 1 ? ((const uint16_t *)src)[i] : ((const uint32_t *)src)[i];
    }
    return (int)(nq * 6);
}
