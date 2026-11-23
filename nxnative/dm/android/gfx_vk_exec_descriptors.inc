// ---------------- descriptors ----------------
static uint64_t c2_handle(int st, int off) {   // the driver's handle table c2, as dmsh_build.py lays it out
    if (off >= 0x20 && off < 0x120) return S.tex[st][(off - 0x20) / 8];
    if (off >= 0x120 && off < 0x160) return S.img[st][(off - 0x120) / 8];
    if (off >= 0x168 && off < 0x568) return S.tex[st][128 + (off - 0x168) / 8];
    if (off >= 0x568 && off < 0x968) return S.samp[st][(off - 0x568) / 8];
    return 0;
}
// nxnative shader_cpu_ready/uniform_ready: finish CPU-decoded descriptor
// dependencies before this command can allocate scratch or descriptor sets.
static void cpu_ready(uint64_t addr, uint64_t size) {
    if (!g_vk_cpu_ready || !size || addr > UINT64_MAX - size) return;
    for (int i = 0; S.pass_open && g_lin_back && (g_pass_draws || P_cleared) && i < S.ncol && i < 8; i++) {
        const tex_t *t = S.rt_has_col[i] ? &S.rt_col[i] : NULL;
        if (!t || !t->mem || !(t->d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) continue;
        uint64_t base = (uintptr_t)t->mem;
        if (addr < base ? base - addr < size : addr - base < t->L.size) { vk_end_pass(); break; }
    }
    vk_cpu_ready(addr, size);
}
static void uniform_ready(int stage, int reg, int64_t off, uint32_t count) {
    if (stage < 0 || stage >= 6 || reg < 0 || reg >= 16 || off < 0 || !count) return;
    const p_bind_buf *u = &S.ubo[stage][reg];
    uint64_t extent = u->size ? u->size : 4;   // target ubo_word treats size zero as one word
    if (!u->addr || (uint64_t)off >= extent || u->addr > UINT64_MAX - (uint64_t)off) return;
    uint64_t size = 8ull * (count - 1) + 4;
    if (size > extent - (uint64_t)off) size = extent - (uint64_t)off;
    cpu_ready(u->addr + off, size);
}
static void shader_cpu_ready(const vkshader_t *s, int stage) {
    if (!s) return;
    // most shaders read no handle or address from uniform buffer memory: found once a shader, not by walking its bindings for every draw (~2% of Sifu's replay)
    if (!s->cpuready) { int any = 0; for (int i = 0; i < s->nbind; i++) { const dmsv_bind *b = &s->b[i];
            any |= b->kind == RK_TEX_UBO || b->kind == RK_TBUF_UBO || b->kind == RK_IMG_UBO || b->kind == RK_SBUF_UBO || b->samp == RS_UBO; }
        ((vkshader_t *)s)->cpuready = any ? 2 : 1; }
    if (s->cpuready == 1) return;
    for (int i = 0; i < s->nbind; i++) {
        const dmsv_bind *b = &s->b[i];
        if (b->kind == RK_TEX_UBO || b->kind == RK_TBUF_UBO || b->kind == RK_IMG_UBO)
            uniform_ready(stage, b->reg, b->off, b->count ? b->count : b->kind == RK_TEX_UBO ? 32 : 1);
        if (b->kind == RK_SBUF_UBO) {
            if (stage < 0 || stage >= 6 || b->reg < 0 || b->reg >= 16 || b->off < 0) continue;
            const p_bind_buf *u = &S.ubo[stage][b->reg];
            uint64_t extent = u->size ? u->size : 4;
            if (u->addr && extent >= 4 && (uint64_t)b->off <= extent - 4 && u->addr <= UINT64_MAX - (uint64_t)b->off - 8)
                cpu_ready(u->addr + b->off, extent - (uint64_t)b->off >= 8 ? 8 : 4);
        }
        if (b->samp == RS_UBO) uniform_ready(stage, b->samp_arg & 0xff, b->samp_arg >> 8, 1);
    }
}
static void graphics_cpu_ready(void) { for (int st = 0; st < 3; st++) shader_cpu_ready(S.sh[st], st); }
static uint32_t ubo_word(int st, int reg, int off) {
    const p_bind_buf *u = reg >= 0 && reg < 16 ? &S.ubo[st][reg] : NULL; uint32_t v = 0;
    if (u && u->addr && off >= 0 && (uint64_t)off + 4 <= (u->size ? u->size : 4) && vk_pool_of(u->addr + off, 4)) vk_ubd_read(u->addr + off, 4, (uint8_t *)&v);   // with the deferred updates
    return v;
}
// The texture and sampler pools the replayed command buffer last bound, as bases in the host's tables (hle_nvn.c dpool_base; 0 for the first pool of each kind). Handles that bind
// calls pass were rebased when recorded (hle_nvn.c bind_handles); handles the replay reads from the game's own memory (bindless: a uniform buffer word) are rebased here. DARK SOULS:
// REMASTERED binds one of 12 sampler pools and reads its scene's texture handles from its uniform buffers: every texture took the sampler of the same ID in the first pool, and its
// tiled walls and floors (texture coordinates of -3.5 and so on) were clamped into streaks
static int g_pool_tex_base, g_pool_samp_base;
int nvn_dpool_base(const void *pool, int tex);
static uint64_t mem_handle(uint64_t h) {
    uint64_t tid = h & 0xfffff, sid = h >> 20 & 0xfff;
    if (tid) tid += (uint64_t)g_pool_tex_base;
    if (sid) sid += (uint64_t)g_pool_samp_base;
    return tid | sid << 20;
}
static uint32_t g_dispatch[3], g_draw_binst;
#define SB_ALIGN 256   // storage buffers are bound from this boundary below their address (dm/android/tools/shader_vk.py SB_ALIGN, which the shaders were translated for)
static void drv_segment(int st, int bank, uint8_t *d, int dyn) {
    memset(d, 0, 0x1000);
    if (bank == 0) {
        // Geometry's index-building shaders read the bound storage addresses at
        // c0[0x410 + 16*r]; leaving them zero also leaves the later indirect draws empty.
        int base = st == 0 ? 0x110 : st == 2 ? 0x410 : st == 5 ? 0x310 : st == 1 ? 0x510 : -1;
        if (base >= 0) for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[st][r].addr; uint32_t n = (uint32_t)S.ssbo[st][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
        if (st != 5) { const float mp = 2047.0f; memcpy(d + 0x610, &mp, 4); memcpy(d + 0x644, &g_draw_binst, 4); }
        else { memcpy(d + 0x200, g_dispatch, 12); for (int r = 0; r < 16; r++) { uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size; memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4); } }
    } else if (!dyn) {
        // c2_handle's layout, a run at a time (d is zeroed above, so 0x160-0x168 stays 0): one call per 8 bytes, 297 a segment, was ~4 % of the replay's samples
        // in Sifu's 15k-draw frames (Fold7)
        memcpy(d + 0x20, S.tex[st], 32 * 8); memcpy(d + 0x120, S.img[st], 8 * 8); memcpy(d + 0x168, S.tex[st] + 128, 128 * 8); memcpy(d + 0x568, S.samp[st], 128 * 8);
    } else {
        // Slot-index handles only for profiles declaring tex_dyn_slots.
        for (int r = 0; r < 8; r++) { uint32_t h = (uint32_t)S.img[st][r]; memcpy(d + 0x120 + 8*r, &h, 4); }
        for (int r = 0; r < 32; r++) { uint64_t h = S.tex[st][r]; uint32_t v = h ? (uint32_t)r | ((h >> 20) ? (uint32_t)r << 20 : 0) : 0; memcpy(d + 0x20 + 8*r, &v, 4); }
        for (int r = 0; r < 128; r++) {
            uint32_t v = S.tex[st][128+r] ? (uint32_t)r : 0; memcpy(d + 0x168 + 8*r, &v, 4);
            v = S.samp[st][r] ? (uint32_t)r << 20 : 0; memcpy(d + 0x568 + 8*r, &v, 4);
        }
    }
}
// vk_samp_by_id without the registry lock while no sampler ID was registered with other parameters (g_samp_gen): a 32-slot texture array took the lock
// per slot, a lock the game thread takes for every texture it registers (DM_NVN_ASYNC). VkSamplers are never destroyed, so a kept handle stays valid
extern uint64_t g_samp_gen;
static VkSampler samp_cached(int sid) {
    enum { SC = 1024 }; static struct { uint64_t gen; int sid; VkSampler s; } c[SC]; unsigned k = (unsigned)sid % SC;
    uint64_t g = __atomic_load_n(&g_samp_gen, __ATOMIC_ACQUIRE);   // read before the lookup: a registration racing it leaves the entry outdated, never wrong and current
    if (c[k].gen == g && c[k].sid == sid) return c[k].s;
    VkSampler s = vk_samp_by_id(sid); c[k].gen = g; c[k].sid = sid; c[k].s = s; return s;
}
static uint64_t g_tex_sseq;   // stage_set: the bind count of the texture slot being collected (0 for handles read from memory)
static VkSampler samp_for(int st, const dmsv_bind *b, uint64_t texh, int nearest) {   // nearest: its unfiltered twin (integer views)
    int sid = 0; uint64_t seq = 0;
    switch (b->samp) {
    case RS_TEX: sid = (int)(texh >> 20); seq = g_tex_sseq; break;
    case RS_SLOT: sid = b->samp_arg >= 0 && b->samp_arg < 128 ? (int)(S.samp[st][b->samp_arg] >> 20) : 0; seq = sid ? S.samp_sseq[st][b->samp_arg] : 0; break;
    case RS_ID: sid = b->samp_arg; break;
    case RS_UBO: sid = (int)(mem_handle(ubo_word(st, b->samp_arg & 0xff, b->samp_arg >> 8) & ~0xfffffull) >> 20); break;
    default: break;
    }
    VkSampler vk_samp_by_id_nearest(int sid), vk_samp_at(int sid, uint64_t seq, int nearest); uint64_t mtl_samp_seq(void);
    if (!g_samp_snap || seq >= mtl_samp_seq()) seq = 0;   // no registration since the bind: the newest (the cached lookups)
    VkSampler s = !sid ? VK_NULL_HANDLE : seq ? vk_samp_at(sid, seq, nearest) : nearest ? vk_samp_by_id_nearest(sid) : samp_cached(sid);
    static int nsl; if (g_tracing || (g_x_logfs && S.sh[1] && S.sh[1]->key == g_x_logfs && nsl++ < 60)) { char d[256]; void vk_samp_describe_at(int sid, uint64_t seq, char *out, size_t n); vk_samp_describe_at(sid, seq, d, sizeof d); VKLOG("  %sbinding %d: %s (texture handle %#llx, present %llu, pass %d, thread %d)", g_tracing ? "" : "logfs ", b->binding, d, (unsigned long long)texh, (unsigned long long)g_tex_frame, g_sub_pass - 1, (int)gettid()); }
    return s ? s : vk_dummy_sampler();
}
// texture views by ID within a submission: a draw's texture arrays (32 slots) looked every slot up again, each through the registry's and the view table's
// locks (NEO: about 2 ms a frame). An entry holds while no texture ID is registered again and no view is dropped (g_tex_gen)
enum { TVC = 4096 };
static struct { uint64_t epoch, gen; int tid, dim, arr, num, sc; VkImageView v; vkimg_t *im; } g_tvc[TVC];
uint64_t g_tvc_hit, g_tvc_miss, g_tvc_cold;   // lookups the cache answered, ones it did not, and of those the ones only an older submission's entry would have answered
// what a view's texels are, as gfx_vk_shader.c's integer-texel variants need it: 0 float, 1 uint (unsigned integer formats, a stencil), 2 int
static int view_num(const tex_t *t, const view_t *v) {
    const vkfmt_t *f = vk_fmt(v && v->set_format ? v->format : t->d.format); if (!f) return 0;
    if (f->depth || f->stencil) return f->stencil && (!f->depth || t->stencil || (v && v->set_dsmode && v->dsmode == 1));   // vk_tex_view's aspect
    return f->is_int ? (f->is_sint ? 2 : 1) : 0;
}
// the integer class a storage image binding's shader declares, from the SPIR-V image format the translator recorded (dmsv_bind.fmt, shader_vk.py): 1 uint, 2 sint,
// 0 float or none declared (the format 0, Unknown, is left alone)
static int img_fmt_cls(uint8_t f) {
    switch (f) {
    case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39: case 40: return 1;   // Rgba32ui .. R64ui
    case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 41: return 2;           // Rgba32i .. R8i, R64i
    }
    return 0;
}
static uint64_t g_num_um, g_num_im;   // stage_set: the sampled bindings of the stage being built that got uint / int views (bit = binding index)
extern uint64_t g_tex_gen;
static int tex_desc_for(int st, const dmsv_bind *b, uint64_t h, VkDescriptorImageInfo *ii, int storage) {   // the view's view_num
    int tid = (int)(h & 0xfffff); const view_t *v = NULL; const tex_t *t = NULL;
    ii->imageLayout = VK_IMAGE_LAYOUT_GENERAL; ii->sampler = storage ? VK_NULL_HANDLE : samp_for(st, b, h, 0); ii->imageView = VK_NULL_HANDLE;
    int sc = storage ? img_fmt_cls(b->fmt) : 0;   // FINAL FANTASY TACTICS' UI composite declares its window image r32ui over an RGBA8 texture: the view is in the shader's class
    unsigned ck = ((unsigned)tid * 2654435761u ^ (unsigned)b->dim * 97u ^ (unsigned)(b->flags & 1) ^ (unsigned)sc * 0x9E37u) % TVC;
    int logfs = g_x_logfs && S.sh[1] && S.sh[1]->key == g_x_logfs && st == 1; static int nlogfs; if (logfs && nlogfs >= 60) logfs = 0;
    if (tid && g_var_now != 3 && !g_tracing && !logfs && g_tvc[ck].epoch == g_set_epoch && g_tvc[ck].gen == g_tex_gen && g_tvc[ck].tid == tid && g_tvc[ck].dim == b->dim && g_tvc[ck].arr == (b->flags & 1) && g_tvc[ck].sc == sc) {
        // (no last_use here: the entry was made in this submission, whose fence the miss below already gave the image; the fence changes only at the commit.
        // Writing it again touched a scattered image record per binding, ~77,000 a frame in Sifu)
        ii->imageView = g_tvc[ck].v; vk_texdump_note(g_tvc[ck].im); g_tvc_hit++;
        if (g_tvc[ck].num && !storage) ii->sampler = samp_for(st, b, h, 1);
        return g_tvc[ck].num;
    }
    int num = 0; g_tvc_miss++;
    if (tid && g_tvc[ck].epoch != g_set_epoch && g_tvc[ck].gen == g_tex_gen && g_tvc[ck].tid == tid && g_tvc[ck].dim == b->dim && g_tvc[ck].arr == (b->flags & 1) && g_tvc[ck].sc == sc) g_tvc_cold++;
    if (tid && g_var_now != 3 && vk_tex_by_id(tid, &v, &t) && t) {
        // a storage image the shader declares as integers over a float or unorm texture of the same texel size: a view of the same image in that class, so the shader
        // reads the texels' bits (FINAL FANTASY TACTICS' UI composite loads its RGBA8 window as r32ui; through a float view its blend came out yellow and white). Integer
        // textures keep their own views and the shader variants of gfx_vk_shader.c
        extern __thread VkFormat vk_view_fmt;
        VkFormat vf = 0; const vkfmt_t *tf = sc && !view_num(t, v) ? vk_fmt(v && v->set_format ? v->format : t->d.format) : NULL;
        if (tf && !tf->compressed && !tf->dec && !tf->depth && !tf->stencil) vf = vk_int_fmt(tf->bpb, sc == 2);
        vkimg_t *im = NULL; ii->imageView = vf ? vk_tex_view_as(t, v, b->dim, b->flags & 1, 0, &im, vf) : vk_tex_view(t, v, b->dim, b->flags & 1, 0, &im); if (storage && im) im->gpu_w = 1;
        if (vf && ii->imageView && vk_view_fmt == vf) { static int once; if (!once++) VKLOG("storage image: nvn format %#x viewed as vk %d for a %s image binding", t->d.format, vf, sc == 1 ? "uint" : "int"); }
        // DM_VK_BIND_DUMP=<pixel shader key hex>:<binding>: write the storage of the texture that binding really resolved to (slots alone are polluted by stale
        // entries from earlier draws and say nothing about which binding reads which); deswizzle it offline with the dimensions in the line
        { static int bd_read; static const char *bd; if (!bd_read) { bd = getenv("DM_VK_BIND_DUMP"); bd_read = 1; }   // read once: this runs for every texture binding of every game
          if (bd && S.sh[1] && t->mem && t->L.size) {
              char want[64]; snprintf(want, sizeof want, "%016llx:%d", (unsigned long long)S.sh[1]->key, (int)b->binding);
              size_t wl = strlen(want);
              if (!strncmp(bd, want, wl) && (bd[wl] == 0 || bd[wl] == ':' || bd[wl] == ',')) {
                  static char done[8][64]; static int nd; int seen = 0;
                  for (int i = 0; i < nd; i++) if (!strcmp(done[i], want)) seen = 1;
                  const char *root = getenv("DM_ROOT"); char path[1024];
                  if (!seen && nd < 8) {
                      snprintf(path, sizeof path, "%s/bind_%016llx_b%d_%ux%u_f%x_s%llu.bin", root ? root : ".", (unsigned long long)S.sh[1]->key, (int)b->binding,
                               t->d.width, t->d.height, t->d.format, (unsigned long long)t->serial);
                      FILE *f = fopen(path, "wb");
                      if (f) { fwrite(t->mem, 1, t->L.size, f); fclose(f); snprintf(done[nd++], sizeof done[0], "%s", want);
                               VKLOG("bind dump: %s: %ux%u nvn format %#x levels %u, %llu bytes", path, t->d.width, t->d.height, t->d.format, t->d.levels, (unsigned long long)t->L.size); }
                  }
              }
          } }
        if (logfs) nlogfs++;
        if (g_tracing || logfs) VKLOG("  %stexture binding %d handle %#llx id %d: %ux%u nvn format %#x target %d serial %llu pool %p+%#llx size %#llx (mem %p, flags %#x, pool flags %#x, packaged layout %d) → image %p (%ux%u fmt %d layers %u levels %u, gpu-written %d) view %p", logfs ? "logfs " : "", b->binding, (unsigned long long)h, tid, t->d.width, t->d.height, t->d.format, t->d.target,
                             (unsigned long long)t->serial, (const void *)t->pool, (unsigned long long)t->off, (unsigned long long)t->L.size, (const void *)t->mem, (unsigned)t->d.flags, t->pool ? (unsigned)t->pool->flags : 0u, (int)t->has_layout, (void *)im, im ? im->w : 0, im ? im->h : 0, im ? im->fmt : 0, im ? im->layers : 0, im ? im->levels : 0, im ? (int)im->gpu_w : -1, (void *)ii->imageView);
        if (im) im->last_use = mtl_last_fence() + 1;
        if (ii->imageView) num = view_num(t, v);
        if (ii->imageView) { g_tvc[ck].epoch = g_set_epoch; g_tvc[ck].gen = g_tex_gen; g_tvc[ck].tid = tid; g_tvc[ck].dim = b->dim; g_tvc[ck].arr = b->flags & 1; g_tvc[ck].v = ii->imageView; g_tvc[ck].im = im; g_tvc[ck].num = num; g_tvc[ck].sc = sc; }
    }
    if (num && !storage) ii->sampler = samp_for(st, b, h, 1);
    if (!ii->imageView) {
        if (g_tracing) VKLOG("  texture binding %d handle %#llx id %d: no texture (%s) → a dummy view", b->binding, (unsigned long long)h, tid, !tid ? "ID 0" : g_var_now == 3 ? "variant 3" : !t ? "the ID is not registered" : "no view");
        // A texture buffer in a slot the translator read as 2D rows of 16384 texels (a 1D integer fetch with no type answer, Program.cs RecordTexelFetch1D; flag 8,
        // shader_vk.py): the Apple host binds the buffer as such rows (nvn_mtl.m tbuf_as_2d), this backend has no image over a buffer, so the shader reads the dummy.
        // Fire Emblem: Three Houses' eye layer read its lighting tables (slots 18-20) so: white, noisy eyes. The profile's answer table fixes it (the slot becomes a texel buffer)
        if (t && t->d.target == NVN_TEXTURE_TARGET_BUFFER && (b->flags & 8)) {
            static uint64_t seen[64]; static int ns; uint64_t key = S.sh[st] ? S.sh[st]->key : 0, id = key ^ (uint64_t)(uint16_t)b->reg << 48; int dup = 0;
            for (int i = 0; i < ns; i++) dup |= seen[i] == id;
            if (!dup && ns < 64) { seen[ns++] = id;
                LOG("[RED] [vk] shader %016llx reads texture slot %d as 2D rows (a 1D fetch with no type answer) but a texture buffer is bound there: it reads a dummy. "
                    "Answer it in the profile's shader_facts_manual.tsv: %016llx {\"tex\":{\"%d\":\"buffer\"}}", (unsigned long long)key, (int)b->reg, (unsigned long long)key, (int)b->reg); }
        }
        const vkfmt_t *f = t ? vk_fmt(t->d.format) : NULL;
        ii->imageView = vk_dummy_view(b->dim == 0xff ? 1 : b->dim, b->flags & 1, (b->flags & 2) != 0, storage, f && f->is_int); num = f && f->is_int ? (f->is_sint ? 2 : 1) : 0;
    }
    return num;
}
// Reuse within a submission (the cache is emptied when it begins, vk_exec_reset): descriptor sets by their whole content (layout, and every buffer range,
// view, sampler and texel buffer view written), and driver segments by their bytes. Unity draws many times with the same bindings, and every new set
// is an allocation and a vkUpdateDescriptorSets; the driver segment's copy is shared so that a set using it can match.
enum { SETC = 8192, DRVC = 1024 };
static struct { uint64_t h, epoch; VkDescriptorSetLayout l; VkDescriptorSet set; size_t off, n; } g_setc[SETC];
// A hash collision must not reuse different descriptors (#2). Keys own explicit Vulkan fields, never scratch pointers or C padding.
// The CPU arena is bounded at 16 MiB and reused at the next submission. When full, new sets simply stay uncached.
enum { SET_KEY_WORDS = 2048, SET_KEY_ARENA_WORDS = 2 * 1024 * 1024 };
static uint64_t g_set_keys[SET_KEY_ARENA_WORDS], g_set_key_epoch;
static size_t g_set_key_used;
// Keep equality bytes in ordinary CPU memory: mapped scratch may be uncached.
// The maximum live prefix is c2's 0x968 bytes; bytes beyond each prefix are zero.
// GPU-written segments must stay outside this immutable cache.
static struct { uint64_t h, epoch; VkBuffer b; VkDeviceSize o; size_t n; uint8_t data[0x968]; } g_drvc[DRVC];
uint64_t g_set_key_bypass;   // cacheable misses left uncached because the per-submission key arena is full
uint64_t g_set_hits, g_set_new, g_ns_set_build, g_ns_set_hash, g_ns_set_new;   // descriptor sets: reused, new; time collecting their descriptors, hashing them, allocating and writing new ones
// Content hash only indexes a cache: set_key/drv_shared still compare every byte before reuse.
// Sifu on Fold7: ~20% of replay CPU samples were in the serial multiply/xor chain.
// Four independent lanes let the CPU overlap long descriptor/driver-segment hashes.
static inline uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p; size_t i = 0;
    if (n >= 32) {
        uint64_t a = h, c = h ^ 0xC2B2AE3D27D4EB4Full, d = h ^ 0x165667B19E3779F9ull, e = h ^ 0x85EBCA77C2B2AE63ull;
        for (; i + 32 <= n; i += 32) {
            uint64_t w[4]; memcpy(w, b + i, sizeof w);
            a = (a ^ w[0]) * 0x9E3779B97F4A7C15ull; a ^= a >> 29;
            c = (c ^ w[1]) * 0x9E3779B97F4A7C15ull; c ^= c >> 29;
            d = (d ^ w[2]) * 0x9E3779B97F4A7C15ull; d ^= d >> 29;
            e = (e ^ w[3]) * 0x9E3779B97F4A7C15ull; e ^= e >> 29;
        }
        h = a ^ ((c << 13) | (c >> 51)) ^ ((d << 29) | (d >> 35)) ^ ((e << 47) | (e >> 17));
        h = (h ^ n) * 0x9E3779B97F4A7C15ull; h ^= h >> 29;
    }
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
// Static descriptor content only: stage_set has already moved dynamic buffer offsets to the bind.
// 2048 words cover its 96 writes, 384 image entries and 96 buffer/texel entries; larger keys bypass caching.
static size_t set_key(uint64_t key[SET_KEY_WORDS], VkDescriptorSetLayout layout, const VkWriteDescriptorSet *w, uint32_t nw) {
    size_t n = 0; key[n++] = (uint64_t)layout; key[n++] = nw;
    for (uint32_t i = 0; i < nw; i++) {
        const VkWriteDescriptorSet *x = &w[i]; unsigned kind = x->pBufferInfo ? 1 : x->pImageInfo ? 2 : x->pTexelBufferView ? 3 : 0;
        size_t stride = kind == 3 ? 1 : kind ? 3 : 0;
        if (!kind || SET_KEY_WORDS - n < 5 || x->descriptorCount > (SET_KEY_WORDS - n - 5) / stride) return 0;
        key[n++] = x->dstBinding; key[n++] = x->dstArrayElement; key[n++] = (uint64_t)x->descriptorType; key[n++] = x->descriptorCount; key[n++] = kind;
        for (uint32_t j = 0; j < x->descriptorCount; j++) {
            if (kind == 1) { const VkDescriptorBufferInfo *b = &x->pBufferInfo[j]; key[n++] = (uint64_t)b->buffer; key[n++] = b->offset; key[n++] = b->range; }
            else if (kind == 2) { const VkDescriptorImageInfo *im = &x->pImageInfo[j]; key[n++] = (uint64_t)im->sampler; key[n++] = (uint64_t)im->imageView; key[n++] = (uint64_t)im->imageLayout; }
            else key[n++] = (uint64_t)x->pTexelBufferView[j];
        }
    }
    return n;
}
uint64_t g_c2_hit, g_c2_put, g_drv_content;   // driver segments found by the slots' hash, built for that path, and built for the content-hashed path (DM_LOG_FPS line)
static void drv_shared(int st, int bank, VkBuffer *buf, VkDeviceSize *off, int dyn) {   // the driver segment: built, then one copy per distinct content in the submission
    static struct { uint64_t h, epoch; VkBuffer b; VkDeviceSize o; const uint8_t *p; } g_c2c[1024];   // (c2 by its slots: segments by g_c2h)
    if (bank && !dyn && g_c2fast && !g_x_nodrvc) {
        // A hit trusts the 64-bit g_c2h: the slots are not compared. With DM_VK_X_SETVERIFY every hit (g_c2_check) builds the segment again and
        // compares its live bytes with the one found ([RED] on a difference, then the fresh one is used); DM_VK_C2_FAST=0 takes the content-hashed path below
        uint64_t h = g_c2h[st] ^ 0x632c632c632c632cull; unsigned k = (unsigned)(h % 1024);
        if (g_c2c[k].epoch == g_set_epoch && g_c2c[k].h == h) { g_c2_hit++;
            if (!g_c2_check) { *buf = g_c2c[k].b; *off = g_c2c[k].o; return; }
            uint8_t d[0x1000], c[0x968]; drv_segment(st, bank, d, 0); g_c2_checked++;
            memcpy(c, g_c2c[k].p, sizeof c);   // (one read of the mapped copy, in the verify mode only; compares stay in ordinary memory)
            if (!memcmp(d, c, sizeof c)) { *buf = g_c2c[k].b; *off = g_c2c[k].o; return; }
            g_c2_bad++; static int n; if (n++ < 20) VKLOG("[RED] c2 segment verify: stage %d: the segment found by the slots' hash differs from a fresh one (%llu of %llu checks)",
                                                            st, (unsigned long long)g_c2_bad, (unsigned long long)g_c2_checked);
        }
        uint8_t *p = vk_scratch(0x1000, V.props.limits.minUniformBufferOffsetAlignment, buf, off); drv_segment(st, bank, p, 0);
        g_c2c[k].h = h; g_c2c[k].epoch = g_set_epoch; g_c2c[k].b = *buf; g_c2c[k].o = *off; g_c2c[k].p = p; g_c2_put++; return;
    }
    uint8_t d[0x1000]; drv_segment(st, bank, d, dyn); g_drv_content++;
    // only the bytes drv_segment can write are hashed (bank 0 ends at 0x648, the c2 table at 0x968; the rest is always zero): 4 KB was 512 hash steps a segment
    size_t n = bank ? 0x968 : 0x648;
    uint64_t h = fnv(0xcbf29ce484222325ull, d, n); unsigned k = (unsigned)(h % DRVC);
    // If different banks hash alike, include the longer live prefix so its tail
    // cannot be mistaken for the shorter bank's implicit zeros.
    size_t compare = n > g_drvc[k].n ? n : g_drvc[k].n;
    if (g_drvc[k].epoch == g_set_epoch && g_drvc[k].h == h && !g_x_nodrvc && !memcmp(g_drvc[k].data, d, compare)) { *buf = g_drvc[k].b; *off = g_drvc[k].o; return; }
    uint8_t *p = vk_scratch(0x1000, V.props.limits.minUniformBufferOffsetAlignment, buf, off); memcpy(p, d, sizeof d);
    g_drvc[k].h = h; g_drvc[k].epoch = g_set_epoch; g_drvc[k].b = *buf; g_drvc[k].o = *off; g_drvc[k].n = n;
    memcpy(g_drvc[k].data, d, n); memset(g_drvc[k].data + n, 0, sizeof g_drvc[k].data - n);
}
// A binding that runs past its chunk's end into the pool's next chunk (pools over ZC_CHUNK come in chunks, each its own buffer, and the replay looks a binding up by its first byte):
// the GPU would read past the buffer, and with robustBufferAccess off it saw zeros. Found with smaller chunks in a fork: Pokemon Pokopia's per-frame constants, a 64 MB ring, crossed
// a 16 MB chunk's end about 2.5 times a second on an AYN Thor, one frame of black objects with a white bloom halo or a model's bones collapsed each time a draw read past it (7 in 3
// minutes at one spot, 0 with this). A constant buffer is bound from a copy of the whole range out of CPU memory (vk_ubd_read, as a deferred update's shadow); a storage or index
// range is memory the GPU may write, so it is counted and named, not copied
extern uint64_t g_ubd_gen;
static uint64_t g_cross[4];   // ranges across a chunk's end: constant buffers copied, storage buffers, vertex buffers (their bound range), index buffers (the draw's indices)
static void vk_cross(int k, uint64_t a, uint64_t n, const vkpool_t *p) {
    // vertex buffers: the stream's bound range, which a draw may read only part of (Pokopia, 16 MB chunks: 32,768 such bindings in 3 minutes of ~1 MB ranges, no wrong frame)
    static const char *const what[4] = {"constant buffer ranges (bound from a copy)", "storage buffer ranges", "vertex buffer bound ranges (a draw may read less)", "index ranges"};
    uint64_t c = ++g_cross[k]; if (!(c & (c - 1))) VKLOG("%s%llu %s across a memory pool chunk's end (this one %#llx +%#llx, the chunk ends at %#llx)", k == 1 || k == 3 ? "[RED] " : "", (unsigned long long)c, what[k],
                                                           (unsigned long long)a, (unsigned long long)n, (unsigned long long)p->hi);
}
// a constant buffer binding's descriptor (stage_set, set_memo_get): its pool's buffer from the alignment boundary below it, or the shadow of the deferred updates over it
extern uint64_t g_cbpath[3];
static void cbuf_desc(int st, const vkshader_t *s, const dmsv_bind *b, VkDescriptorBufferInfo *bi) {
    const p_bind_buf *u = b->reg >= 0 && b->reg < 16 ? &S.ubo[st][b->reg] : NULL;
    uint64_t n = u && u->size ? (u->size > 65536 ? 65536 : u->size) : 65536; VkBuffer buf; VkDeviceSize off;
    // the pool this slot's buffer was last found in, while no pool came or went (vk_pool_gen; a lazy pool given its buffer counts): the memo's check of the
    // constant buffers (set_memo_get) looks each one up again for most draws, and pool_find's own cache (thread-local, by page) took ~8% of Sifu's replay thread
    // (the last four, most recent first: Sifu's game scene binds a slot's constant buffers from several pools in turn). As pool_find's own caches, a pool with pools inside
    // it (has_inner) is never answered from here: its range holds the inner pool's addresses, whose pages its buffer no longer has (Fire Emblem: Three Houses' device lost, 7a6eba18)
    static struct { uint32_t gen; vkpool_t *p[4]; } pc[6][16];
    vkpool_t *p = NULL;
    if (g_var_now != 9 && u && u->addr) {
        uint32_t vk_pool_gen(void); uint32_t g = vk_pool_gen() + 1; typeof(pc[0][0]) *e = &pc[st][b->reg];
        if (e->gen == g) for (int k = 0; k < 4; k++) { vkpool_t *c = e->p[k]; if (c && u->addr >= c->lo && u->addr < c->hi && !__atomic_load_n(&c->has_inner, __ATOMIC_ACQUIRE)) { p = c; for (; k > 0; k--) e->p[k] = e->p[k - 1]; e->p[0] = c; break; } }
        if (!p) { p = vk_pool_of(u->addr, 1); if (e->gen != g) { memset(e->p, 0, sizeof e->p); e->gen = g; } if (p) { memmove(&e->p[1], &e->p[0], 3 * sizeof *e->p); e->p[0] = p; } }
    }
    if (!p) {
        if (g_ubolog_key && s->key == g_ubolog_key && g_ubolog_n > 0) { g_ubolog_n--; VKLOG("ubo: present %llu pass %d stage %d c%d %#llx +%llu: in no memory pool, zeros bound", (unsigned long long)g_tex_frame, g_sub_pass - 1, st, b->reg, (unsigned long long)(u ? u->addr : 0), (unsigned long long)(u ? u->size : 0)); }
        *bi = (VkDescriptorBufferInfo){vk_zero_buffer(), 0, 65536}; return; }
    buf = p->buf; off = u->addr - p->lo;
    if (off + n > p->hi - p->lo) {
        if (p->hi < p->end && u->addr + n <= p->end) {   // into the pool's next chunk (vk_cross): the whole range from CPU memory, one copy per submission
            static struct { uint64_t a, n, ep, gen; VkBuffer b; VkDeviceSize o; } cc[16]; unsigned k = (unsigned)(u->addr >> 8) % 16;
            if (cc[k].a != u->addr || cc[k].n != n || cc[k].ep != g_set_epoch || cc[k].gen != g_ubd_gen) {
                uint8_t *d = vk_scratch(n, V.props.limits.minUniformBufferOffsetAlignment, &buf, &off); vk_ubd_read(u->addr, n, d); vk_cross(0, u->addr, n, p);
                cc[k].a = u->addr; cc[k].n = n; cc[k].ep = g_set_epoch; cc[k].gen = g_ubd_gen; cc[k].b = buf; cc[k].o = off;
            } else { buf = cc[k].b; off = cc[k].o; }
            ubo_log(s, st, b->reg, u->addr, n); *bi = (VkDescriptorBufferInfo){buf, off, n}; g_cbpath[1]++; return;
        }
        n = p->hi - p->lo - off;
    }
    ubo_log(s, st, b->reg, u->addr, n);
    if (vk_ubd_shadow(u->addr, n, &buf, &off)) { *bi = (VkDescriptorBufferInfo){buf, off, n}; g_cbpath[2]++; return; }   // deferred updates in it: their shadow
    g_cbpath[0]++;
    VkDeviceSize am = V.props.limits.minUniformBufferOffsetAlignment - 1, mis = off & am;   // (a power of two: a mask, not a division)
    if (mis) VKRED_ONCE(40, "uniform buffer at an offset not aligned to %llu", (unsigned long long)am + 1);
    *bi = (VkDescriptorBufferInfo){buf, p->boff + off - mis, n + mis};   // (off is from the pool's start, its buffer starts boff before it)
}
// The set a stage's last draw got, and what it was built from. Between two draws most stages change no texture, sampler, image or storage buffer, only
// the offsets of their constant buffers, which go to the bind (dynamic uniform buffers). Then nothing is collected or hashed: the constant buffers alone are
// looked at again, and their buffer and range must be the same (the offset too for one that is not dynamic). It holds within one submission (the sets and
// the driver segments' copies are its own) while the stage's shader and bindings (g_bind_gen, g_ubo_gen; only real changes count), the texture IDs (g_tex_gen),
// sampler IDs (g_samp_gen), memory pools (vk_pool_gen) and the base instance (the driver segment's c0 + 0x644) are unchanged. Images keep the last_use the
// first build gave them: the fence it names is the same for the whole submission. Not for compute (its driver segment holds the dispatch size and its storage
// buffers are marked written), nor for a shader that reads handles or addresses from uniform buffer memory (that memory changes under the same bindings).
// DM_VK_X_NOSETMEMO=1 (live): always collected. DM_VK_X_SETVERIFY=1 (live): one hit in 64 is collected anyway and compared, [RED] on a difference
enum { MEMO_CB = 16 };
typedef struct { vkshader_t *s; uint64_t epoch, bind, ubo, ubd, tex, samp, h, h2, um, im; uint32_t pool, binst, dofs[96]; int ncb; uint8_t ci[MEMO_CB], cpos[MEMO_CB]; VkDescriptorBufferInfo cd[MEMO_CB]; VkDescriptorSet set; } set_memo_t;
static set_memo_t g_smemo_own[3], *g_smemo[3] = {&g_smemo_own[0], &g_smemo_own[1], &g_smemo_own[2]};   // each stage's last set: an entry of g_rmemo (rmemo_entry; its own without the table)
static uint64_t g_bind_gen[6], g_ubo_gen[6];   // a stage's handles or storage buffers changed (bind_handles_rec, OP_BIND_SSBO), its uniform buffers (OP_BIND_UBO)
uint64_t g_set_memo, g_set_memo_bad;   // memo hits, and differences the verify mode found
extern uint64_t g_ubd_gen; uint32_t vk_pool_gen(void);
// Where stage_set's time goes and why its memo misses (the Android overlay's descriptor rows, gfx_vk_present.c): collecting by binding kind (0 textures,
// images, samplers and texel buffers; 1 uniform and storage buffers; 2 the driver segments, c1 and the support buffer), and per call 0 kept by the memo,
// 1 another shader on the stage, 2 a binding of the stage changed, 3 the texture, sampler or pool tables changed, 4 its constant buffers moved, 5 other
// (another submission, base instance, a stage or shader the memo does not take)
uint64_t g_memo_why[6], g_cbmiss[3], g_cbpath[3];   // g_cbpath: cbuf_desc's answers from the pool's buffer, a copy across chunks, a deferred update's shadow   // g_cbmiss: constant buffer misses by cause (another buffer, another range, a static one's offset)
static inline int ss_kind(int k) { return k == RK_CBUF || k == RK_SBUF || k == RK_UBO_PTR || k == RK_SBUF_UBO ? 1 : k == RK_SUPPORT || k == RK_C1 || k == RK_DRV ? 2 : 0; }
static int set_memo_ok(vkshader_t *s) {
    if (!s->setmemo) { int ok = 1, ncb = 0;
        for (int i = 0; i < s->nbind; i++) { const dmsv_bind *b = &s->b[i]; ncb += b->kind == RK_CBUF;
            ok &= b->kind != RK_TEXD && b->kind != RK_SAMPD && b->kind != RK_SBUF_UBO && b->kind != RK_UBO_PTR && b->kind != RK_TEX_UBO && b->kind != RK_IMG_UBO && b->kind != RK_TBUF_UBO && b->samp != RS_UBO && i < 256; }
        s->setmemo = ok && ncb <= MEMO_CB ? 1 : 2; }
    return s->setmemo == 1;
}
static int shader_wmem(vkshader_t *s);
enum { SPARSE_SBUF_CACHE = 32, SPARSE_SBUF_COPY_CAP = 32 * 1024 * 1024 };   // AA456's exhibit shader reads a 24 MiB sparse SSBO; larger snapshots fall back to the zero buffer
static struct { uint64_t a, n, epoch, gen; VkBuffer buf; VkDeviceSize off; } g_sparse_sbuf[SPARSE_SBUF_CACHE];
static int sparse_sbuf_desc(uint64_t a, uint64_t n, VkDescriptorBufferInfo *bi) {
    if (!a || !n || n > UINT64_MAX - a) return 0;
    uint64_t base = a & ~((uint64_t)SB_ALIGN - 1), size = a + n - base;
    if (!nvn_sparse_address_pool(a, n)) return 0;
    if (size > SPARSE_SBUF_COPY_CAP) {
        static int noted; if (!noted++) VKLOG("[RED] sparse storage buffer snapshot %#llx + %#llx exceeds the %d-byte cap; using the zero buffer",
            (unsigned long long)base, (unsigned long long)size, SPARSE_SBUF_COPY_CAP);
        return 0;
    }
    if (size > V.props.limits.maxStorageBufferRange) return 0;
    uint64_t gen = nvn_sparse_gen(); unsigned k = (unsigned)((a >> 8) ^ (n >> 4)) % SPARSE_SBUF_CACHE;
    if (g_sparse_sbuf[k].a == base && g_sparse_sbuf[k].n == size && g_sparse_sbuf[k].epoch == g_set_epoch && g_sparse_sbuf[k].gen == gen) {
        *bi = (VkDescriptorBufferInfo){g_sparse_sbuf[k].buf, g_sparse_sbuf[k].off, size}; return 1;
    }
    VkBuffer buf; VkDeviceSize off; uint8_t *dst = vk_scratch(size, SB_ALIGN, &buf, &off);
    uint64_t miss = nvn_sparse_address_copy(base, dst, size);
    static unsigned notes; if (notes++ < 8) VKLOG("sparse storage buffer snapshot: %#llx + %#llx -> %llu bytes (%llu unmapped), generation %llu",
        (unsigned long long)base, (unsigned long long)size, (unsigned long long)size, (unsigned long long)miss, (unsigned long long)gen);
    g_sparse_sbuf[k] = (typeof(g_sparse_sbuf[0])){base, size, g_set_epoch, gen, buf, off};
    *bi = (VkDescriptorBufferInfo){buf, off, size}; return 1;
}
static uint64_t g_verify_n, g_verify_slot_bad, g_verify_bad;   // DM_VK_X_SETVERIFY checks: made, slot descriptors differing, anything differing
static uint64_t g_verify_dofs[8];   // DM_VK_X_SETVERIFY: dynamic offsets that differ from the memo's, by binding kind (1 constant buffer, 3 driver segment)
static int set_memo_get(int st, vkshader_t *s, uint32_t *dofs, const VkDescriptorBufferInfo *cbd) {   // cbd: the stage's constant buffer descriptors by binding (stage_set), or NULL
    set_memo_t *m = g_smemo[st];
    if (m->s != s) { g_memo_why[1]++; return 0; }
    if (m->epoch != g_set_epoch || m->binst != g_draw_binst) { g_memo_why[5]++; return 0; }
    if (m->bind != g_bind_gen[st]) { g_memo_why[2]++; return 0; }
    if (m->tex != g_tex_gen || m->samp != __atomic_load_n(&g_samp_gen, __ATOMIC_ACQUIRE) || m->pool != vk_pool_gen()) { g_memo_why[3]++; return 0; }
    if (m->ubo != g_ubo_gen[st] || m->ubd != g_ubd_gen) {   // the constant buffers again: most draws move only their offsets
        for (int c = 0; c < m->ncb; c++) { VkDescriptorBufferInfo d; if (cbd) d = cbd[m->ci[c]]; else cbuf_desc(st, s, &s->b[m->ci[c]], &d);
            if (d.buffer != m->cd[c].buffer || d.range != m->cd[c].range || (m->cpos[c] == 0xff && d.offset != m->cd[c].offset)) { g_memo_why[4]++;
                g_cbmiss[d.buffer != m->cd[c].buffer ? 0 : d.range != m->cd[c].range ? 1 : 2]++; return 0; }
            if (m->cpos[c] != 0xff) m->dofs[m->cpos[c]] = (uint32_t)d.offset; }
        m->ubo = g_ubo_gen[st]; m->ubd = g_ubd_gen; }
    memcpy(dofs, m->dofs, sizeof *dofs * (size_t)s->ndyn); g_num_um = m->um; g_num_im = m->im;
    g_memo_why[0]++; return 1;
}
// Several sets a stage had in this submission, by its bindings' content (g_c2h, g_sseqh, g_ssboh: order-free hashes of every slot of the stage, kept by the
// binds in O(1)): a stage whose bindings come back to an earlier draw's gets that draw's set without collecting or hashing anything, set_memo_get checking
// the rest as for the last set (shader, submission, base instance, tables, constant buffers). The last-set memo alone missed 9,500 stage sets a frame in
// Sifu's title scene (a binding changed), and 6,200 of the sets they collected were found again in the submission. A hit trusts the 64-bit rmemo_key for the
// slots: nothing compares them (the entry keeps no copy of them; set_memo_get still compares the constant buffers). DM_VK_X_SETVERIFY collects one hit in 64
// anyway, with the c2 segment built again rather than found by its hash (drv_shared), and logs [RED] on a difference. A stage that made a per-bind texel
// buffer copy while it was collected (vk_tbuf_view's fallback) is not kept here: its set holds that bind's bytes (stage_set)
enum { RMEMO = 2048 };
typedef struct { uint64_t key; int st; set_memo_t m; } rmemo_t;
static rmemo_t *g_rmemo; uint64_t g_rmemo_hit, g_rmemo_put;
// The buffers of the stage's constant buffers go into the key too: Sifu's game scene keeps them in many pools (a VkBuffer each), and one entry per bindings
// was overwritten each time a draw's constant buffers sat in other pools (7,700 of its stage sets a frame missed for another buffer)
static void cbuf_desc(int st, const vkshader_t *s, const dmsv_bind *b, VkDescriptorBufferInfo *bi);
// The stage's constant buffer descriptors, once a stage_set (the memo's check, its key and the collection all take them)
enum { CBD_MAX = 256 };
static void cbd_fill(int st, const vkshader_t *s, VkDescriptorBufferInfo cbd[CBD_MAX]) {
    for (int i = 0; i < s->nbind && i < CBD_MAX; i++) if (s->b[i].kind == RK_CBUF) cbuf_desc(st, s, &s->b[i], &cbd[i]);
}
static uint64_t cb_sig(const vkshader_t *s, const VkDescriptorBufferInfo *cbd) {
    uint64_t h = 0;
    for (int i = 0; i < s->nbind && i < CBD_MAX; i++) if (s->b[i].kind == RK_CBUF) { const VkDescriptorBufferInfo *d = &cbd[i];
        h = (h ^ (uint64_t)(uintptr_t)d->buffer ^ (d->range << 40)) * 0x9E3779B97F4A7C15ull; h ^= h >> 31; }
    return h;
}
static uint64_t rmemo_key(int st, const vkshader_t *s, uint64_t cbs) {
    uint64_t k = ((uint64_t)(uintptr_t)s + (uint64_t)st) * 0x9E3779B97F4A7C15ull ^ cbs; k ^= k >> 29;
    k ^= g_c2h[st] * 0xC2B2AE3D27D4EB4Full; k ^= k >> 31; k ^= g_sseqh[st] * 0x165667B19E3779F9ull; k ^= k >> 29; k ^= g_ssboh[st] * 0x85EBCA77C2B2AE63ull;
    return k ^ (k >> 32);
}
static int rmemo_take(int st, vkshader_t *s, uint32_t *dofs, uint64_t cbs, const VkDescriptorBufferInfo *cbd) {
    if (!g_rmemo) return 0;
    uint64_t k = rmemo_key(st, s, cbs); rmemo_t *e = &g_rmemo[k % RMEMO];
    if (e->key != k || e->st != st || e->m.s != s || e->m.epoch != g_set_epoch) return 0;
    // the stage's last set from now. Its bindings are taken to be those it was made with (an equal rmemo_key), so the generation is today's: set_memo_get's
    // binding check then holds by construction, and the hit rests on the key (see above)
    g_smemo[st] = &e->m; e->m.bind = g_bind_gen[st];
    return set_memo_get(st, s, dofs, cbd);
}
// The entry a stage's new set is written into, in place (a ~1.7 KB copy each way took 7% of Sifu's replay thread): another stage's memo left pointing at
// it only misses, as the entry then names another shader
static set_memo_t *rmemo_entry(int st, const vkshader_t *s, uint64_t cbs) {
    if (!g_rmemo && !(g_rmemo = calloc(RMEMO, sizeof *g_rmemo))) return &g_smemo_own[st];
    uint64_t k = rmemo_key(st, s, cbs); rmemo_t *e = &g_rmemo[k % RMEMO]; e->key = k; e->st = st; g_rmemo_put++;
    return &e->m;
}
// A memo entry's slot descriptors one by one, for the verify mode's [RED] line naming the binding that differs: a side table made on the first verify
// fill, one row per g_rmemo entry and per stage's own entry, so the entries themselves stay small (768 B of each ~1.7 KB, 1.5 MB of g_rmemo, when they were in it)
typedef struct { int nwh; uint32_t wbind[32], wtype[32]; uint64_t wh[32], wa[32]; } memo_slots_t;
static memo_slots_t *g_memo_slots;
static memo_slots_t *memo_slots(const set_memo_t *m, int make) {
    if (!g_memo_slots && (!make || !(g_memo_slots = calloc(RMEMO + 3, sizeof *g_memo_slots)))) return NULL;
    for (int st = 0; st < 3; st++) if (m == &g_smemo_own[st]) return &g_memo_slots[RMEMO + st];
    uintptr_t off = (uintptr_t)m - (uintptr_t)g_rmemo;   // (addresses as integers: m is an entry of g_rmemo or of g_smemo_own, two objects)
    if (g_rmemo && off < sizeof *g_rmemo * RMEMO) return &g_memo_slots[off / sizeof *g_rmemo];
    return NULL;
}
// The descriptors that come from the stage's slots (images, samplers, texel buffers, storage buffers), hashed apart from the uniform buffers: a memo hit must give these
// exactly, while a uniform buffer or driver segment may be an equal copy in another buffer (the verify mode, DM_VK_X_SETVERIFY, tells the two apart)
static uint64_t slot_desc_hash(const VkWriteDescriptorSet *w, uint32_t nw) {
    uint64_t h = 0x84222325cbf29ce4ull;
    for (uint32_t i = 0; i < nw; i++) { const VkWriteDescriptorSet *x = &w[i];
        if (x->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || x->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
        h = (h ^ x->dstBinding) * 0x100000001b3ull;
        for (uint32_t k = 0; k < x->descriptorCount; k++) {
            if (x->pImageInfo) { h = (h ^ (uint64_t)(uintptr_t)x->pImageInfo[k].imageView) * 0x9E3779B97F4A7C15ull; h = (h ^ (uint64_t)(uintptr_t)x->pImageInfo[k].sampler) * 0x9E3779B97F4A7C15ull; }
            else if (x->pTexelBufferView) h = (h ^ (uint64_t)(uintptr_t)x->pTexelBufferView[k]) * 0x9E3779B97F4A7C15ull;
            else if (x->pBufferInfo) { h = (h ^ (uint64_t)(uintptr_t)x->pBufferInfo[k].buffer) * 0x9E3779B97F4A7C15ull; h = (h ^ x->pBufferInfo[k].offset ^ (x->pBufferInfo[k].range << 32)) * 0x9E3779B97F4A7C15ull; } } }
    return h;
}
// The same descriptors one write at a time (the verify mode names the first that differs): binding, type, a hash and the first element's handle
static int slot_desc_list(const VkWriteDescriptorSet *w, uint32_t nw, uint32_t bind[32], uint32_t type[32], uint64_t hv[32], uint64_t first[32]) {
    int n = 0;
    for (uint32_t i = 0; i < nw && n < 32; i++) { const VkWriteDescriptorSet *x = &w[i];
        if (x->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || x->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
        bind[n] = x->dstBinding; type[n] = x->descriptorType; hv[n] = slot_desc_hash(x, 1);
        first[n] = x->pImageInfo ? (uint64_t)(uintptr_t)x->pImageInfo[0].imageView ^ ((uint64_t)(uintptr_t)x->pImageInfo[0].sampler << 1) :
                   x->pTexelBufferView ? (uint64_t)(uintptr_t)x->pTexelBufferView[0] : x->pBufferInfo ? (uint64_t)(uintptr_t)x->pBufferInfo[0].buffer + x->pBufferInfo[0].offset : 0;
        n++; }
    return n;
}
// dofs (at least s->ndyn entries): the dynamic offsets of the set's dynamic uniform buffers, in binding order as vkCmdBindDescriptorSets takes them
// indirect_c0 (compute only): a fresh segment for this dispatch's GPU-written counts; never put mutable bytes in drv_shared's content cache.
static VkDescriptorSet stage_set(int st, vkshader_t *s, uint32_t *dofs, VkDescriptorBufferInfo *indirect_c0) {
    // Check before descriptor memo hits: storage/unknown descriptors can write beyond tracked transfer ranges.
    if (vk_readback_ranges()) for (int i = 0; i < s->nbind; i++) {
        int d = s->b[i].desc;
        int k = s->b[i].kind;
        if (k == RK_SBUF || k == RK_SBUF_UBO || k == RK_IMG || k == RK_IMG_UBO || k == RK_IMG_TBUF) { vk_readback_unknown(); break; }
        if (d != RD_UBO && d != RD_SAMPLED && d != RD_TBUF && d != RD_IMAGE && d != RD_SAMPLER) { vk_readback_unknown(); break; }
    }
    int memo = st < 3 && !g_x_nosetmemo && !g_tracing && !g_var_now && !g_x_nosetc && !g_x_nodrvc && !g_ubolog_key && g_td_off < 0 && set_memo_ok(s), verify = 0;
    uint32_t vd[96]; uint64_t vum = 0, vim = 0;   // verify: what the memo gave
    if (!memo) g_memo_why[5]++;
    // the constant buffer descriptors now, unless the last set holds without looking at them (nothing moved since)
    VkDescriptorBufferInfo cbd[CBD_MAX]; int have_cbd = 0;
    if (memo) { const set_memo_t *m0 = g_smemo[st]; if (!(m0->s == s && m0->ubo == g_ubo_gen[st] && m0->ubd == g_ubd_gen)) { cbd_fill(st, s, cbd); have_cbd = 1; } }
    int got = memo && set_memo_get(st, s, dofs, have_cbd ? cbd : NULL); uint64_t cbs = 0; int have_cbs = 0;
    if (!got && memo) { if (!have_cbd) { cbd_fill(st, s, cbd); have_cbd = 1; }
        cbs = cb_sig(s, cbd); have_cbs = 1; if (rmemo_take(st, s, dofs, cbs, cbd)) { got = 1; g_rmemo_hit++; } }
    if (got) { static unsigned nv; if (!g_x_setverify || ++nv % 64) { g_set_memo++; return g_smemo[st]->set; }
        verify = 1; memcpy(vd, dofs, sizeof *dofs * (size_t)s->ndyn); vum = g_num_um; vim = g_num_im; }
    uint64_t gtex = g_tex_gen, gsamp = __atomic_load_n(&g_samp_gen, __ATOMIC_ACQUIRE); uint32_t gpool = vk_pool_gen();   // before collecting: a change racing it (game thread) leaves the memo outdated
    extern unsigned g_tbuf_copies; unsigned tc0 = g_tbuf_copies;   // (gfx_vk_tex.c: a per-bind texel buffer copy made while collecting)
    g_c2_check = g_x_setverify;   // (DM_VK_X_SETVERIFY: every c2 segment found by its slots' hash built again and compared, drv_shared; not only in the
    // 1-in-64 memo checks, whose collections found none by hash in Super Mario Odyssey: 111,500 of them, 0 c2 checks)
    VkDescriptorSet set = VK_NULL_HANDLE; struct { uint32_t b, o; int i; } dp[96]; int nd = 0; uint64_t tb0 = tick();
    VkWriteDescriptorSet w[96]; VkDescriptorBufferInfo bi[96]; VkDescriptorImageInfo ii[96 * 4]; VkBufferView bv[128]; uint32_t nw = 0, nii = 0, nbv = 0;
    int ncb = 0; uint8_t mci[MEMO_CB]; VkDescriptorBufferInfo mcd[MEMO_CB];   // the constant buffers, for the memo
    g_num_um = g_num_im = 0;
    int dynslots = 0;
    if (DM_GAME_TEX_DYN_SLOTS) for (int j = 0; j < s->nbind; j++)
        if (s->b[j].kind == RK_TEXD || s->b[j].kind == RK_SAMPD) dynslots = 1;
    for (int i = 0; i < s->nbind && nw < 96; i++) {
        const dmsv_bind *b = &s->b[i];
        VkWriteDescriptorSet *wr = &w[nw]; memset(wr, 0, sizeof *wr);
        wr->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr->dstSet = set; wr->dstBinding = b->binding; wr->descriptorCount = 1;
        VkBuffer buf; VkDeviceSize off, len;
        switch (b->kind) {
        case RK_SUPPORT: bi[nw] = (VkDescriptorBufferInfo){vk_support_buffer(), 0, 65536}; break;
        case RK_C1: vk_shader_c1(s, &buf, &off, &len); bi[nw] = (VkDescriptorBufferInfo){buf, off, len}; break;
        case RK_CBUF: if (have_cbd && i < CBD_MAX) bi[nw] = cbd[i]; else cbuf_desc(st, s, b, &bi[nw]); if (ncb < MEMO_CB) { mci[ncb] = (uint8_t)i; mcd[ncb++] = bi[nw]; } break;
        case RK_DRV: {
            if (indirect_c0 && st == 5 && b->reg == 0) {
                if (!indirect_c0->buffer) {   // several bindings of c0 in one shader share this dispatch's segment
                    uint8_t *d = vk_scratch(0x1000, V.props.limits.minUniformBufferOffsetAlignment, &indirect_c0->buffer, &indirect_c0->offset);
                    drv_segment(st, 0, d, dynslots); indirect_c0->range = 0x1000;
                }
                bi[nw] = *indirect_c0;   // retain the real offset before dynamic descriptors move it into dofs
            } else { VkBuffer sb; VkDeviceSize so; drv_shared(st, b->reg, &sb, &so, dynslots); bi[nw] = (VkDescriptorBufferInfo){sb, so, 0x1000}; }
            break; }
        case RK_SBUF: case RK_UBO_PTR: case RK_SBUF_UBO: {
            uint64_t a = 0, n = 0;
            if (b->kind == RK_SBUF && b->reg >= 0 && b->reg < 16) { a = S.ssbo[st][b->reg].addr; n = S.ssbo[st][b->reg].size; }
            else if (b->kind == RK_UBO_PTR && b->reg >= 0 && b->reg < 16) { a = S.ubo[st][b->reg].addr; n = S.ubo[st][b->reg].size; }
            else if (b->kind == RK_SBUF_UBO) { a = (uint64_t)ubo_word(st, b->reg, b->off) | (uint64_t)ubo_word(st, b->reg, b->off + 4) << 32; }
            uint64_t guest_a = a, physical = 0; vkpool_t *p = a ? vk_pool_of(a, 1) : NULL;
            if (!p && a && n && nvn_sparse_address_translate(a, n, &physical)) { p = vk_pool_of(physical, n); if (p) a = physical; }
            if (p) { buf = p->buf; off = a - p->lo; if (n && off + n > p->hi - p->lo && p->hi < p->end) vk_cross(1, a, n, p); if (!n || off + n > p->hi - p->lo) n = p->hi - p->lo - off;
                // bound from the 256-byte boundary below it, the boundary the translator offsets every access from (shader_vk.py SB_ALIGN; a multiple of any device's minimum alignment)
                const VkDeviceSize al = SB_ALIGN; if (V.props.limits.minStorageBufferOffsetAlignment > al) VKRED_ONCE(41, "storage buffer alignment %llu above %llu", (unsigned long long)V.props.limits.minStorageBufferOffsetAlignment, (unsigned long long)al);
                bi[nw] = (VkDescriptorBufferInfo){buf, p->boff + off - off % al, n + off % al};
                if (st == 5 && b->kind != RK_UBO_PTR) { void vk_gpu_wrote(uint64_t a, uint64_t n); vk_gpu_wrote(a, n); }   // a dispatch's storage buffers: taken as written
            } else if (b->kind == RK_SBUF && st != 5 && !shader_wmem(s) && sparse_sbuf_desc(guest_a, n, &bi[nw])) { /* read-only sparse storage: snapshot into this submission */ }
            else bi[nw] = (VkDescriptorBufferInfo){vk_zero_buffer(), 0, 65536};
            break; }
        case RK_TEX: case RK_TEX_UBO: case RK_TEX_C2A: {
            int cnt = b->count ? b->count : 32;
            // Twelve 32-entry arrays fill ii; even a scalar after them needs a capacity check, never a truncated write.
            if (nii + (uint32_t)cnt > sizeof ii / sizeof *ii) die("[RED] [vk] image descriptor array exceeds capacity");
            for (int k = 0; k < cnt; k++) {
                uint64_t h = b->kind == RK_TEX ? vk_tex_slot_handle(S.tex[st], S.img[st], (unsigned)b->reg) :
                             b->kind == RK_TEX_UBO ? mem_handle(ubo_word(st, b->reg, b->off + 8 * k)) : (b->off >= 0 ? c2_handle(st, b->off + 8 * k) : 0);
                g_tex_sseq = b->kind == RK_TEX && b->reg >= 0 && b->reg < 256 ? S.tex_sseq[st][b->reg] : 0;
                int num = tex_desc_for(st, b, h, &ii[nii + k], 0);
                if (num && i < 64) { if (num == 1) g_num_um |= 1ull << i; else g_num_im |= 1ull << i; }
            }
            wr->pImageInfo = &ii[nii]; nii += (uint32_t)cnt; wr->descriptorCount = (uint32_t)cnt; wr->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; nw++;
            continue; }
        case RK_TEXD: case RK_SAMPD: {
            int cnt = b->count ? b->count : 32;
            if (b->desc == RD_TBUF) {
                if (cnt > 32 || nbv + (uint32_t)cnt > sizeof bv / sizeof *bv) die("[RED] [vk] dynamic texel buffer array exceeds capacity");
                for (int k = 0; k < cnt; k++) {
                    uint64_t h = dynslots ? (S.tex[st][128+k] ? S.tex[st][128+k] : S.tex[st][k]) : 0;
                    const view_t *v = NULL; const tex_t *t = NULL;
                    int tid = (int)(h & 0xfffff);
                    bv[nbv + k] = tid && vk_tex_by_id(tid, &v, &t) && t && t->d.target == NVN_TEXTURE_TARGET_BUFFER && !view_num(t, NULL) ? vk_tbuf_view(t, 0) : VK_NULL_HANDLE;   // these arrays declare float texels: an integer-format buffer takes the dummy, as the texture array below
                    if (!bv[nbv + k]) bv[nbv + k] = vk_dummy_tbuf(0);
                }
                if (dynslots) { static int noted; if (!noted++) VKLOG("dynamic texel buffer slot arrays enabled (32 entries)"); }
                wr->pTexelBufferView = &bv[nbv]; nbv += (uint32_t)cnt; wr->descriptorCount = (uint32_t)cnt;
                wr->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                nw++; continue;
            }
            if (cnt > 32 || nii + (uint32_t)cnt > sizeof ii / sizeof *ii) die("[RED] [vk] dynamic texture array exceeds capacity");
            for (int k = 0; k < cnt; k++) {
                VkDescriptorImageInfo *di = &ii[nii + k]; memset(di, 0, sizeof *di);
                if (b->kind == RK_SAMPD) {
                    int sid = dynslots ? (int)(S.samp[st][k] >> 20) : 0;
                    if (!sid && dynslots) sid = (int)(S.tex[st][k] >> 20);
                    di->sampler = sid ? vk_samp_by_id(sid) : VK_NULL_HANDLE;
                    if (!di->sampler) di->sampler = vk_dummy_sampler();
                } else {
                    uint64_t h = dynslots ? (S.tex[st][k] ? S.tex[st][k] : S.tex[st][128+k]) : 0;
                    const view_t *v = NULL; const tex_t *t = NULL;
                    int tid = (int)(h & 0xfffff), fits = 0;
                    if (tid && vk_tex_by_id(tid, &v, &t) && t) {
                        int target = v && v->set_target ? v->target : t->d.target;
                        fits = b->dim == 3 ? (t->d.width == t->d.height && (target == 8 || target == 9 || (target == 4 && t->d.depth >= 6))) :
                               b->dim == 2 ? target == 2 : b->dim == 1 ? (target == 1 || target == 4 || target == 8 || target == 9) : (target == 0 || target == 3);
                        if (view_num(t, v)) fits = 0; // these arrays declare floating-point texels
                    }
                    dmsv_bind vb = *b; vb.samp = RS_TEX;
                    tex_desc_for(st, &vb, fits ? h : 0, di, 0);
                    di->sampler = VK_NULL_HANDLE; // separate sampled image, not combined
                }
            }
            if (dynslots) { static int noted; if (!noted++) VKLOG("dynamic texture slot arrays enabled (32 entries)"); }
            wr->pImageInfo = &ii[nii]; nii += (uint32_t)cnt; wr->descriptorCount = (uint32_t)cnt;
            wr->descriptorType = b->kind == RK_TEXD ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            nw++; continue;
        }
        case RK_IMG: case RK_IMG_UBO: {
            if (nii >= sizeof ii / sizeof *ii) die("[RED] [vk] storage image descriptor exceeds capacity");
            uint64_t h = b->kind == RK_IMG ? (b->reg >= 0 && b->reg < 32 ? S.img[st][b->reg] : 0) : mem_handle(ubo_word(st, b->reg, b->off));
            tex_desc_for(st, b, h, &ii[nii], 1); wr->pImageInfo = &ii[nii++]; wr->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; nw++;
            continue; }
        case RK_TBUF: case RK_TBUF_UBO: case RK_IMG_TBUF: {
            if (nbv >= sizeof bv / sizeof *bv) die("[RED] [vk] texel buffer descriptor exceeds capacity");
            uint64_t h = b->kind == RK_TBUF ? vk_tex_slot_handle(S.tex[st], S.img[st], (unsigned)b->reg) : b->kind == RK_IMG_TBUF ? (b->reg >= 0 && b->reg < 32 ? S.img[st][b->reg] : 0) : mem_handle(ubo_word(st, b->reg, b->off));
            const view_t *v; const tex_t *t = NULL; int tid = (int)(h & 0xfffff);
            bv[nbv] = tid && vk_tex_by_id(tid, &v, &t) && t && t->d.target == NVN_TEXTURE_TARGET_BUFFER ? vk_tbuf_view(t, b->kind == RK_IMG_TBUF) : VK_NULL_HANDLE;
            if ((g_tracing && g_x_tracepass >= 0) || (st == 5 && g_x_tracepass == -2)) VKLOG("  texel buffer binding %d (kind %d, cbuf %d +%d): handle %#llx → %s format %#x, %u elements at %#llx%s", b->binding, b->kind, b->reg, b->off,
                (unsigned long long)h, t ? "texture" : "no texture", t ? t->d.format : 0, t ? t->d.width : 0, t ? (unsigned long long)(uintptr_t)t->mem : 0ull, bv[nbv] ? "" : " (the dummy: zeros)");
            if (!bv[nbv] && t && t->d.target != NVN_TEXTURE_TARGET_BUFFER && b->kind == RK_TBUF) {   // a profile's "buffer" answer (shader_facts_manual.tsv) that met an image: the answer is wrong
                static uint64_t seen[64]; static int ns; uint64_t key = S.sh[st] ? S.sh[st]->key : 0, id = key ^ (uint64_t)(uint16_t)b->reg << 48; int dup = 0;
                for (int j = 0; j < ns; j++) dup |= seen[j] == id;
                if (!dup && ns < 64) { seen[ns++] = id;
                    LOG("[RED] [vk] shader %016llx reads texture slot %d as a texel buffer (the answer table says buffer) but a texture of target %d is bound there: it reads zeros",
                        (unsigned long long)key, (int)b->reg, (int)t->d.target); }
            }
            if (!bv[nbv]) bv[nbv] = vk_dummy_tbuf(b->kind == RK_IMG_TBUF);
            else if (i < 64) { int num = view_num(t, NULL); if (num == 1) g_num_um |= 1ull << i; else if (num == 2) g_num_im |= 1ull << i; }   // texel buffers read and write their words alike (storage ones too: gfx_vk_shader.c spv_int_images)
            wr->pTexelBufferView = &bv[nbv++]; wr->descriptorType = b->kind == RK_IMG_TBUF ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; nw++;
            continue; }
        default: {   // unknown: the descriptor type it was declared with, filled with what reads zero
            if (b->desc == RD_UBO || b->desc == RD_SSBO) bi[nw] = (VkDescriptorBufferInfo){vk_zero_buffer(), 0, 65536};
            else if (b->desc == RD_SAMPLED || b->desc == RD_STORAGE) {
                int cnt = b->count ? b->count : 32;
                if (nii + (uint32_t)cnt > sizeof ii / sizeof *ii) die("[RED] [vk] fallback image descriptor array exceeds capacity");
                ii[nii] = (VkDescriptorImageInfo){b->desc == RD_SAMPLED ? vk_dummy_sampler() : VK_NULL_HANDLE, vk_dummy_view(b->dim == 0xff ? 1 : b->dim, b->flags & 1, (b->flags & 2) != 0, b->desc == RD_STORAGE, 0), VK_IMAGE_LAYOUT_GENERAL};
                for (int k = 1; k < cnt; k++) ii[nii + k] = ii[nii];
                wr->pImageInfo = &ii[nii]; nii += (uint32_t)cnt; wr->descriptorCount = (uint32_t)cnt;
                wr->descriptorType = b->desc == RD_SAMPLED ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; nw++; continue;
            } else if (b->desc == RD_TBUF || b->desc == RD_STORAGE_TBUF) {
                if (nbv >= sizeof bv / sizeof *bv) die("[RED] [vk] texel buffer descriptor exceeds capacity");
                bv[nbv] = vk_dummy_tbuf(b->desc == RD_STORAGE_TBUF); wr->pTexelBufferView = &bv[nbv++];
                wr->descriptorType = b->desc == RD_TBUF ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER; nw++; continue; }
            else continue;
        }
        }
        wr->pBufferInfo = &bi[nw]; wr->descriptorType = b->desc == RD_SSBO ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (s->dyn[i]) {   // the offset goes to the bind, so the set's content (and its reuse) does not depend on it
            if (bi[nw].offset > UINT32_MAX) VKRED_ONCE(42, "dynamic uniform buffer offset past 4 GB");
            int j = nd++; while (j > 0 && dp[j - 1].b > b->binding) { dp[j] = dp[j - 1]; j--; }
            dp[j].b = b->binding; dp[j].o = (uint32_t)bi[nw].offset; dp[j].i = i; bi[nw].offset = 0; wr->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        }
        nw++;
    }
    for (int j = 0; j < nd; j++) dofs[j] = dp[j].o;
    if (g_tracing) for (int j = 0; j < nd; j++) VKLOG("  stage %d binding %u: dynamic offset %u", st, dp[j].b, dp[j].o);
    if (g_tracing) for (uint32_t i = 0; i < nw; i++) {   // the refused draw (bisection): every descriptor it is given
        const VkWriteDescriptorSet *x = &w[i];
        if (x->pBufferInfo) VKLOG("  stage %d binding %u type %d: buffer %p +%llu size %llu", st, x->dstBinding, x->descriptorType, (void *)x->pBufferInfo->buffer, (unsigned long long)x->pBufferInfo->offset, (unsigned long long)x->pBufferInfo->range);
        else if (x->pImageInfo) for (uint32_t k = 0; k < x->descriptorCount && k < 4; k++) VKLOG("  stage %d binding %u[%u] type %d: view %p sampler %p", st, x->dstBinding, k, x->descriptorType, (void *)x->pImageInfo[k].imageView, (void *)x->pImageInfo[k].sampler);
        else if (x->pTexelBufferView) VKLOG("  stage %d binding %u type %d: texel buffer view %p", st, x->dstBinding, x->descriptorType, (void *)x->pTexelBufferView[0]);
    }
    g_c2_check = 0;
    // the set's content, then an earlier identical set of this submission or a new one
    uint64_t tb1 = tick(); g_ns_set_build += tb1 - tb0;
    uint64_t key[SET_KEY_WORDS]; size_t nk = set_key(key, s->dsl, w, nw);
    uint64_t h = fnv(0xcbf29ce484222325ull, key, nk * sizeof *key);
    if (g_set_key_epoch != g_set_epoch) { g_set_key_used = 0; g_set_key_epoch = g_set_epoch; }
    unsigned k = (unsigned)(h % SETC); uint64_t tb2 = tick(); g_ns_set_hash += tb2 - tb1;
    int cacheable = nk && !g_tracing && !g_x_nosetc;
    if (cacheable && g_setc[k].epoch == g_set_epoch && g_setc[k].h == h && g_setc[k].l == s->dsl && g_setc[k].n == nk &&
        !memcmp(g_set_keys + g_setc[k].off, key, nk * sizeof *key)) { g_set_hits++; set = g_setc[k].set; }
    else { set = vk_alloc_set(s->dsl); g_set_new++;
        for (uint32_t i = 0; i < nw; i++) w[i].dstSet = set;
        vkUpdateDescriptorSets(V.dev, nw, w, 0, NULL);
        if (cacheable && nk <= SET_KEY_ARENA_WORDS - g_set_key_used) {
            memcpy(g_set_keys + g_set_key_used, key, nk * sizeof *key);
            g_setc[k].h = h; g_setc[k].epoch = g_set_epoch; g_setc[k].l = s->dsl; g_setc[k].set = set; g_setc[k].off = g_set_key_used; g_setc[k].n = nk;
            g_set_key_used += nk;
        } else if (cacheable) g_set_key_bypass++;
        g_ns_set_new += tick() - tb2; }
    // verify: the same content (hash), integer-texel bindings and dynamic offsets as the memo gave. A lone difference in dofs alone can be a driver segment
    // copied again after its g_drvc entry was taken by another (the memo's copy is as good); anything repeated is a dependency the memo misses
    uint64_t h2 = g_x_setverify && (verify || memo) ? slot_desc_hash(w, nw) : 0;   // (only with DM_VK_X_SETVERIFY)
    // (DM_VK_X_SETVERIFY) a slot descriptor that differs is either a dependency the memo misses (named in the [RED] line below) or an equal copy made again,
    // such as a per-bind texel buffer copy (vk_tbuf_view's fallback), whose stage the memo keeps only for the next draw (rmemo takes none, see above)
    int dofs_bad = 0, first_dof = -1;
    if (verify) { g_verify_n++; if (h2 != g_smemo[st]->h2) g_verify_slot_bad++;
        dofs_bad = memcmp(vd, dofs, sizeof *dofs * (size_t)s->ndyn) != 0;
        if (dofs_bad) for (int j = 0; j < nd && j < (int)s->ndyn; j++) if (vd[j] != dofs[j]) { int kind = s->b[dp[j].i].kind;
            g_verify_dofs[kind < 0 ? 0 : kind > 7 ? 7 : kind]++; if (first_dof < 0) first_dof = j; }
        if (h != g_smemo[st]->h || g_num_um != vum || g_num_im != vim || dofs_bad) g_verify_bad++;
        if (g_verify_n % 500 == 0) { uint64_t other = g_verify_dofs[0] + g_verify_dofs[2] + g_verify_dofs[4] + g_verify_dofs[5] + g_verify_dofs[6] + g_verify_dofs[7];
            VKLOG("memo verify: %llu hits checked, slot descriptors differ %llu, anything %llu; c2 segments found by hash checked %llu, differ %llu, dynamic offsets differ by kind: 1 %llu, 3 %llu, other %llu", (unsigned long long)g_verify_n, (unsigned long long)g_verify_slot_bad, (unsigned long long)g_verify_bad, (unsigned long long)g_c2_checked, (unsigned long long)g_c2_bad, (unsigned long long)g_verify_dofs[1], (unsigned long long)g_verify_dofs[3], (unsigned long long)other); }
        const memo_slots_t *mm = h2 != g_smemo[st]->h2 ? memo_slots(g_smemo[st], 0) : NULL;
        if (mm) { static int nl; uint32_t fb[32], ft[32]; uint64_t fh[32], fa[32]; int fn = slot_desc_list(w, nw, fb, ft, fh, fa);
            for (int q = 0; q < fn && q < mm->nwh && nl < 40; q++) if (fb[q] != mm->wbind[q] || fh[q] != mm->wh[q]) { int kind = -1, reg = -1;
                for (int j = 0; j < s->nbind; j++) if (s->b[j].binding == fb[q]) { kind = s->b[j].kind; reg = s->b[j].reg; }
                nl++; VKLOG("[RED] memo verify: stage %d shader %016llx binding %u (kind %d reg %d, Vulkan type %u) differs: memo %#llx, fresh %#llx", st, (unsigned long long)s->key, fb[q], kind, reg, ft[q], (unsigned long long)mm->wa[q], (unsigned long long)fa[q]); break; } } }
    if (verify && (h != g_smemo[st]->h || g_num_um != vum || g_num_im != vim || dofs_bad)) { g_set_memo_bad++; static int n, n2;
        if (h2 != g_smemo[st]->h2 && n2++ < 40) VKLOG("[RED] descriptor set memo: stage %d shader %016llx: slot descriptors differ from the memo's", st, (unsigned long long)s->key);
        if (n < 80) { char ks[256]; int ko = 0; for (int i = 0; i < s->nbind && ko < 240; i++) ko += snprintf(ks + ko, sizeof ks - (size_t)ko, " %d.%d.%d", s->b[i].kind, s->b[i].reg, s->b[i].samp); ks[ko] = 0;
            VKLOG("[RED] descriptor set memo: stage %d shader %016llx: bindings (kind.reg.samp):%s", st, (unsigned long long)s->key, ks); }
        if (n++ < 80) { char first[96] = "";
            if (first_dof >= 0) { const dmsv_bind *b = &s->b[dp[first_dof].i];
                snprintf(first, sizeof first, " first: binding %u kind %d reg %d, memo %#x fresh %#x", dp[first_dof].b, (int)b->kind, (int)b->reg, vd[first_dof], dofs[first_dof]); }
            VKLOG("[RED] descriptor set memo: stage %d shader %016llx: a fresh set differs from the memo's (hash %016llx / %016llx, um %#llx / %#llx, im %#llx / %#llx, dynamic offsets %s)%s", st, (unsigned long long)s->key,
                  (unsigned long long)h, (unsigned long long)g_smemo[st]->h, (unsigned long long)g_num_um, (unsigned long long)vum, (unsigned long long)g_num_im, (unsigned long long)vim, dofs_bad ? "differ" : "same", first); } }
    if (memo) { if (!have_cbs) { if (!have_cbd) { cbd_fill(st, s, cbd); have_cbd = 1; } cbs = cb_sig(s, cbd); } set_memo_t *m = g_smemo[st] = g_tbuf_copies != tc0 ? &g_smemo_own[st] : rmemo_entry(st, s, cbs);   // (set_memo_ok: at most MEMO_CB constant buffers)
        m->s = s; m->epoch = g_set_epoch; m->bind = g_bind_gen[st]; m->ubo = g_ubo_gen[st]; m->ubd = g_ubd_gen; m->tex = gtex; m->samp = gsamp; m->pool = gpool; m->binst = g_draw_binst;
        { memo_slots_t *ms = memo_slots(m, g_x_setverify); if (ms) ms->nwh = g_x_setverify ? slot_desc_list(w, nw, ms->wbind, ms->wtype, ms->wh, ms->wa) : 0; }   // (verify only)
        m->h = h; m->h2 = h2; m->um = g_num_um; m->im = g_num_im; m->set = set; memcpy(m->dofs, dofs, sizeof *dofs * (size_t)s->ndyn); m->ncb = ncb; memcpy(m->ci, mci, (size_t)ncb); memcpy(m->cd, mcd, sizeof *mcd * (size_t)ncb);
        for (int c = 0; c < ncb; c++) { m->cpos[c] = 0xff; for (int j = 0; j < nd; j++) if (dp[j].i == mci[c]) m->cpos[c] = (uint8_t)j; } }
    return set;
}

