// ---------------- programs and pipelines ----------------
static uint64_t key64(const uint8_t k[8]) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | k[i]; return v; }
// DM_NVN_SHDUMP (debugging; hle_nvn.c then keeps each program's control sections): a shader missing from the bundle is written to <files>/shader_<key>.bin as its 0x880-byte
// control section followed by its code area (control +0x704 bytes), so it can be looked for in the game files (Xenoblade Chronicles 3's title had one such program)
static void shader_dump(const prog_t *p, unsigned i, uint64_t kk) {
    static int on = -1; if (on < 0) on = getenv("DM_NVN_SHDUMP") != NULL;
    if (!on || i >= p->nstage || !p->sd[i].control || !p->sd[i].data) return;
    uint32_t ncode; memcpy(&ncode, (const uint8_t *)p->sd[i].control + 0x704, 4);
    const char *dir = getenv("DM_ROOT"); char path[1024]; snprintf(path, sizeof path, "%s/shader_%016llx.bin", dir ? dir : ".", (unsigned long long)kk);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fwrite(p->sd[i].control, 1, 0x880, f); if (ncode < (1u << 24)) fwrite((const void *)(uintptr_t)p->sd[i].data, 1, ncode, f); fclose(f);
    VKLOG("shader %016llx written to %s", (unsigned long long)kk, path);
}
static void bind_program(const p_bind_program *bp) {
    // NVN binds by stage mask (vertex 1, fragment 2, geometry 4, tessellation 8 / 0x10, compute 0x20): only the masked stages change, so a game that keeps
    // its vertex and fragment shaders in programs of their own (NEO, AI: THE SOMNIUM FILES) binds them one after the other and draws with both.
    // prog_key's index is the position in the program's shader array (as the game passed them to SetShaders), not the stage: the stage is the bundle's.
    int vk_shader_stage(uint64_t key);
    uint32_t mask = bp->stages ? (uint32_t)bp->stages & 0x3f : 0x3f;
    for (int st = 0; st < 6; st++) if (mask >> st & 1) S.sh[st] = NULL;
    S.prog = bp->prog;
    if (S.prog) for (unsigned i = 0; i < 6; i++) {
        uint8_t k[8]; if (!prog_key(S.prog, i, k)) break;
        uint64_t kk = key64(k); int st = kk ? vk_shader_stage(kk) : -1;
        if (kk && st < 0) {   // the draw or dispatch is skipped. Logged once per shader (the first 16 binds were 4 shaders of Xenoblade Chronicles 3 over and over, which hid others)
            static uint64_t seen[256]; static int nseen; int known = 0; for (int j = 0; j < nseen; j++) if (seen[j] == kk) { known = 1; break; }
            if (!known && nseen < 256) { seen[nseen++] = kk; void vk_shader_missing(uint64_t key); vk_shader_missing(kk); shader_dump(S.prog, i, kk); } }
        if (st < 0 || st > 5 || !(mask >> st & 1)) continue;
        S.sh[st] = vk_shader(kk, st);
    }
    S.prog_ok = S.sh[0] || S.sh[5];   // a vertex-only program draws depth only (prepare_draw)
}
typedef struct {
    uint64_t key[3]; uint32_t topo, restart, raster_discard, cull, front, mode, depth_clamp, bias_en, debug_mode;
    uint32_t dt, dw, df, st, sf[2], sfail[2], szfail[2], szpass[2];
    uint32_t nattr; struct { uint32_t loc, binding, fmt, off; } attr[16];
    uint32_t nbind; struct { uint32_t binding, stride, rate, divisor; } bind[17];
    uint32_t ncol, cfmt[8], dfmt, samples, a2c, blend_en, logic_en, logic_op, wmask[8];
    struct { uint32_t src, dst, srca, dsta, eq, eqa; } bl[8];
    uint32_t clip_neg1, alt, fdz, dual, gs_split;   // fdz: fragment depth; dual: dual-source blending; gs_split: separated geometry effects/rasterization
    uint64_t um[3], im[3];   // each stage's sampled bindings given integer views (vk_shader_module)
} pkey;
static nvn_hmap g_pipes = {.klen = sizeof(pkey)}; static pthread_mutex_t g_pipe_mu = PTHREAD_MUTEX_INITIALIZER;   // the replay thread and the prewarm threads
static VkPipeline build_pipeline(const pkey *k, VkPipelineLayout layout, vkshader_t *const stg[3]);
static void pipe_record(const pkey *k);
// The last key and its pipeline (the replay thread's only; pipelines are never dropped): runs of draws with one pipeline skip the 992-byte hash and the lock
static pkey g_last_k; static VkPipeline g_last_p;
// Pipelines built in the background. A pipeline met for the first time costs the driver's compile on the replay thread, 20-35 ms each on Adreno 830 (no pipeline
// libraries there): in Super Mario Odyssey the first cap throw at a breakable object, the first swim, made 2-7 new pipelines a frame (175 ms frames, seen as 40, 30
// fps or lower for a moment), then never again in that run; the prewarm (below) covers what an earlier run met. A draw whose pipeline does not exist yet is queued for
// a background thread and left out until it is built, a few frames, when every target of its pass was drawn in each of the two presents before: a target drawn every
// frame shows it a few frames late. Not two presents at any time: an image kept from the title screen and drawn once at a scene's load lost SMO's fog for the run. A target drawn for the first time (an environment map made once) builds at once, as a draw left out there would stay missing, and so
// do rasterizer-discard and transform-feedback draws (they write memory later draws read). Age goes by the image: counted by size and format instead, SMO's first scene
// after Resume lost its coin counter and tutorial movie for good and showed the Cap Kingdom fog unparted (targets drawn once, in classes the title screen had aged).
// So a capture's new full-screen targets still build at once (SMO's first frog capture: 33 pipelines, two 300 ms frames, once). So do passes into a linear render target:
// the CPU reads it (lin_back), and SMO's fog wave solver feeds what it reads into the next step, so one step left out lost the Cap Kingdom fog for the run.
// DM_VK_ASYNC_PIPE=0 (live): every pipeline is built at once
#define PIPE_PENDING ((VkPipeline)(uintptr_t)2)
enum { APQ = 256 };
static pkey g_apq[APQ]; static int g_apq_h, g_apq_t, g_apq_on;   // queued keys (a draw left out counts as skipped for no pipeline)
static pthread_mutex_t g_apq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_apq_cv = PTHREAD_COND_INITIALIZER;
static void *async_pipe_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk async pipe");
    for (;;) {
        pthread_mutex_lock(&g_apq_mu); while (g_apq_h == g_apq_t) pthread_cond_wait(&g_apq_cv, &g_apq_mu);
        pkey k = g_apq[g_apq_t % APQ]; g_apq_t++; pthread_mutex_unlock(&g_apq_mu);
        vkshader_t *stg[3] = {0}; VkPipeline p = (VkPipeline)1;
        for (int s = 0; s < 3; s++) if (k.key[s] && !(s == 1 && k.raster_discard)) stg[s] = vk_shader(k.key[s], s);
        if (stg[0]) { VkDescriptorSetLayout dsl[5] = {0}; int nsets = 0;
            for (int s = 0; s < 3; s++) if (stg[s]) { dsl[stg[s]->set] = stg[s]->dsl; if (stg[s]->set + 1 > nsets) nsets = stg[s]->set + 1; }
            uint64_t t0 = tick(); p = build_pipeline(&k, vk_pipeline_layout(dsl, nsets), stg);
            { static int n; if (n++ < 6) VKLOG("pipeline built in the background in %.1f ms (shaders %016llx %016llx)", (tick() - t0) / 1e6, (unsigned long long)k.key[0], (unsigned long long)k.key[1]); } }
        pthread_mutex_lock(&g_pipe_mu);
        VkPipeline had = nvn_hm_get(&g_pipes, &k);
        if (had && had != PIPE_PENDING) { if (p != (void *)1) vkDestroyPipeline(V.dev, p, NULL); }   // a prewarm thread made it meanwhile (as graphics_pipeline does: the one in the table may be bound already, the new one goes)
        else { nvn_hm_put(&g_pipes, &k, p); if (p != (void *)1) pipe_record(&k); }
        pthread_mutex_unlock(&g_pipe_mu);
    }
    return NULL;
}
static int async_pipe(const pkey *k) {   // 1: queued (the draw is left out); under nothing (takes g_pipe_mu)
    if (!g_async_pipe || k->raster_discard || (S.sh[0] && S.sh[0]->xfb) || (S.sh[2] && S.sh[2]->xfb)) return 0;
    for (int i = 0; i < S.ncol && i < 8; i++) if (S.rt_has_col[i] && (S.rt_col[i].d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) return 0;   // (the CPU reads it: lin_back)
    int natt = 0; for (int i = 0; i < 9; i++) { const vkimg_t *a = P_att[i].im; if (!a) continue; natt++; if (a->rt_frames < 3) {
            if (g_prof_on) { static int n; if (n++ < 300) VKLOG("pipeline built here: attachment %d %ux%u nvn format %#x drawn in %llu presents in a row (shaders %016llx %016llx)", i, a->w, a->h, a->nvn_format, (unsigned long long)a->rt_frames, (unsigned long long)k->key[0], (unsigned long long)k->key[1]); }
            return 0; } }   // (counted in this present too)
    if (!natt) return 0;
    pthread_mutex_lock(&g_apq_mu);
    if (!g_apq_on) { g_apq_on = 1; for (int i = 0; i < 2; i++) { pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, async_pipe_thread, NULL); pthread_attr_destroy(&a); } }
    int ok = g_apq_h - g_apq_t < APQ; if (ok) { g_apq[g_apq_h % APQ] = *k; g_apq_h++; pthread_cond_signal(&g_apq_cv); }
    pthread_mutex_unlock(&g_apq_mu);
    if (ok) { pthread_mutex_lock(&g_pipe_mu); if (!nvn_hm_get(&g_pipes, k)) nvn_hm_put(&g_pipes, k, PIPE_PENDING); pthread_mutex_unlock(&g_pipe_mu); }
    return ok;
}
static VkPipeline graphics_pipeline(const pkey *k, VkPipelineLayout layout) {
    if (g_last_p && !memcmp(k, &g_last_k, sizeof *k)) return g_last_p == (void *)1 ? VK_NULL_HANDLE : g_last_p;
    pthread_mutex_lock(&g_pipe_mu); VkPipeline p = nvn_hm_get(&g_pipes, k); pthread_mutex_unlock(&g_pipe_mu);
    if (p == PIPE_PENDING) return VK_NULL_HANDLE;
    if (p) { g_last_k = *k; g_last_p = p; return p == (void *)1 ? VK_NULL_HANDLE : p; }
    if (async_pipe(k)) return VK_NULL_HANDLE;
    vkshader_t *stg[3] = {S.sh[0], S.sh[1], S.sh[2]};
    uint64_t tb = g_prof_on ? tick() : 0; p = build_pipeline(k, layout, stg); if (g_prof_on) g_prof_ns[0] += tick() - tb; g_vkst.pipes_built++;
    pthread_mutex_lock(&g_pipe_mu);
    VkPipeline had = nvn_hm_get(&g_pipes, k);
    if (had) { if (p != (void *)1) vkDestroyPipeline(V.dev, p, NULL); p = had; }   // a prewarm thread made it meanwhile
    else { nvn_hm_put(&g_pipes, k, p); if (p != (void *)1) pipe_record(k); }
    pthread_mutex_unlock(&g_pipe_mu);
    return p == (void *)1 ? VK_NULL_HANDLE : p;
}
// ---- pipeline prewarm: every pipeline the game needed is recorded (DM_ROOT/vk_pipelines.bin) and made again at the next boot on background
// threads, before the game reaches it, so a scene seen once never stalls a frame on a pipeline compile again (the driver's compile, not the
// pipeline cache's lookup, is the cost: tens of milliseconds per pipeline on Adreno). DM_VK_NOPREWARM=1 turns it off.
#define PIPE_MAGIC 0x33455050u   // "PPE3"; geometry effects/rasterization now have separate pipeline keys
// debug_mode shifted the key's fields without changing its padded size; PPE1 prewarm records otherwise decoded as 76 colour targets and crashed Adreno at startup.
static FILE *g_prec;
// The records are written by a thread of their own: a flush to the app's files folder (FUSE) took tens of milliseconds, and made under g_pipe_mu it held up the replay's
// pipeline lookups (a 66 ms frame with no pipeline built in it)
enum { PREC_Q = 64 };
static pkey g_precq[PREC_Q]; static int g_precq_n, g_precw_on; static pthread_mutex_t g_precq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_precq_cv = PTHREAD_COND_INITIALIZER;
static void *pipe_record_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk pipe record"); static pkey b[PREC_Q];
    for (;;) {
        pthread_mutex_lock(&g_precq_mu); while (!g_precq_n) pthread_cond_wait(&g_precq_cv, &g_precq_mu);
        int n = g_precq_n; memcpy(b, g_precq, sizeof *b * (size_t)n); g_precq_n = 0; pthread_mutex_unlock(&g_precq_mu);
        fwrite(b, sizeof *b, (size_t)n, g_prec); fflush(g_prec);
    }
    return NULL;
}
static void pipe_record(const pkey *k) {   // (under g_pipe_mu: only queued here)
    if (!g_prec || k->alt) return;   // DM_VK_SPV_ALT's pipelines are a debugging run's own
    pthread_mutex_lock(&g_precq_mu);
    if (!g_precw_on) { g_precw_on = 1; pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, pipe_record_thread, NULL); pthread_attr_destroy(&a); }
    if (g_precq_n < PREC_Q) g_precq[g_precq_n++] = *k;   // (a full queue drops the record: that pipeline is only not prewarmed next time)
    pthread_cond_signal(&g_precq_cv); pthread_mutex_unlock(&g_precq_mu);
}
static pkey *g_pw; static size_t g_pw_n; static volatile size_t g_pw_next, g_pw_done;
static void *prewarm_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk prewarm");
    setpriority(PRIO_PROCESS, 0, 10);   // below the game's threads: prewarm uses idle cores
    for (;;) {
        size_t i = __atomic_fetch_add(&g_pw_next, 1, __ATOMIC_RELAXED); if (i >= g_pw_n) break;
        const pkey *k = &g_pw[i];
        pthread_mutex_lock(&g_pipe_mu); int have = nvn_hm_get(&g_pipes, k) != NULL; pthread_mutex_unlock(&g_pipe_mu);
        if (have) continue;
        vkshader_t *stg[3] = {0}; int ok = 1;
        for (int s = 0; s < 3; s++) if (k->key[s] && !(s == 1 && k->raster_discard)) { stg[s] = vk_shader(k->key[s], s); if (!stg[s]) ok = 0; }
        if (!ok || !stg[0]) continue;
        VkDescriptorSetLayout dsl[5] = {0}; int nsets = 0;
        for (int s = 0; s < 3; s++) if (stg[s]) { dsl[stg[s]->set] = stg[s]->dsl; if (stg[s]->set + 1 > nsets) nsets = stg[s]->set + 1; }
        VkPipeline p = build_pipeline(k, vk_pipeline_layout(dsl, nsets), stg);
        pthread_mutex_lock(&g_pipe_mu);
        if (!nvn_hm_get(&g_pipes, k)) nvn_hm_put(&g_pipes, k, p); else if (p != (void *)1) vkDestroyPipeline(V.dev, p, NULL);
        pthread_mutex_unlock(&g_pipe_mu);
        size_t d = __atomic_add_fetch(&g_pw_done, 1, __ATOMIC_RELAXED);
        if (d == g_pw_n || d % 500 == 0) VKLOG("prewarm: %zu of %zu pipelines", d, g_pw_n);
    }
    return NULL;
}
static void prewarm_start(void) {
    const char *root = getenv("DM_ROOT"); if (!root) return;
    char path[1024]; snprintf(path, sizeof path, "%s/vk_pipelines.bin", root);
    uint32_t hdr[2] = {PIPE_MAGIC, (uint32_t)sizeof(pkey)}, fh[2] = {0};
    FILE *f = fopen(path, "rb");
    if (f && fread(fh, sizeof fh, 1, f) == 1 && fh[0] == hdr[0] && fh[1] == hdr[1]) {
        fseek(f, 0, SEEK_END); long n = (ftell(f) - (long)sizeof fh) / (long)sizeof(pkey); fseek(f, sizeof fh, SEEK_SET);
        if (n > 0) { g_pw = malloc((size_t)n * sizeof(pkey)); g_pw_n = fread(g_pw, sizeof(pkey), (size_t)n, f); }
    }
    if (f) fclose(f);
    // rewrite the file with the records read (dropping a torn last record), then append to it
    g_prec = fopen(path, "wb"); if (g_prec) { fwrite(hdr, sizeof hdr, 1, g_prec); if (g_pw_n) fwrite(g_pw, sizeof(pkey), g_pw_n, g_prec); fflush(g_prec); }
    if (!g_pw_n || getenv("DM_VK_NOPREWARM")) { VKLOG("prewarm: %zu pipelines recorded%s", g_pw_n, g_pw_n ? ", off (DM_VK_NOPREWARM)" : ""); return; }
    long nc = sysconf(_SC_NPROCESSORS_ONLN); int nt = nc >= 8 ? 3 : nc >= 4 ? 2 : 1;
    VKLOG("prewarm: %zu recorded pipelines on %d threads", g_pw_n, nt);
    for (int i = 0; i < nt; i++) { pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, prewarm_thread, NULL); pthread_attr_destroy(&a); }
}
// ---- what each thread is building, for the crash dump. Issue #64: Super Mario Bros. Wonder crashed in the Adreno 750's shader compiler (libllvm-qgl.so, driver 512.762)
// at its title scene, with no log line to say which pipeline or which thread; and a pipeline that crashes is never recorded for the prewarm, so every run met it again.
// A slot per thread in a build: the crash dump prints each (the crashing thread's marked), with the shaders and the state that pick the driver's compile
enum { NBUILD = 16 };
static struct { _Atomic int tid; volatile int phase; char what[400]; } g_build[NBUILD];   // phase 0: claimed, 1: making shader modules, 2: in the driver's pipeline compile
static int build_begin(void) {   // the slot, or -1 when all are taken (nothing noted)
    int me = gettid(); for (int i = 0; i < NBUILD; i++) { int z = 0; if (atomic_compare_exchange_strong(&g_build[i].tid, &z, me)) { g_build[i].phase = 0; return i; } }
    return -1;
}
static void build_end(int i) { if (i >= 0) { g_build[i].phase = 0; atomic_store(&g_build[i].tid, 0); } }
static void build_dump(void) {   // (the crash handler: no locks, no allocation)
    int me = gettid(), n = 0;
    for (int i = 0; i < NBUILD; i++) { int t = atomic_load(&g_build[i].tid); if (!t || !g_build[i].phase) continue; n++;
        char nm[64]; crash_thread_name((unsigned)t, nm, sizeof nm);
        fprintf(stderr, "[dm] [vk] %s thread %#x '%s' was %s: %s\n", t == me ? "this (the crashing)" : "another", (unsigned)t, nm,
                g_build[i].phase == 1 ? "making its shader modules" : "in the driver's pipeline compile", g_build[i].what); }
    if (!n) fprintf(stderr, "[dm] [vk] no pipeline was being built\n");
}
static VkPipeline build_pipeline(const pkey *k, VkPipelineLayout layout, vkshader_t *const stg_in[3]) {
    // DM_VK_PIPE_LOG: one line per pipeline built, with the state that decides how a material is drawn (shaders, cull/front, depth test/write/func,
    // colour formats, blend). For a game where some materials render wrong and others do not, the lines of the wrong material name the state to compare.
    { static int on = -1; if (on < 0) on = getenv("DM_VK_PIPE_LOG") ? atoi(getenv("DM_VK_PIPE_LOG")) : 0;
      if (on > 0) { static int n; if (n++ < 600)
        VKLOG("pipeline %d: shaders %016llx %016llx %016llx topo %u cull %u front %u mode %u depth test %u write %u func %u stencil %u | colour targets %u (formats %u %u %u %u) depth format %u | samples %u a2c %u blend %u logic %u/%u | attrs %u binds %u",
              n, (unsigned long long)k->key[0], (unsigned long long)k->key[1], (unsigned long long)k->key[2], k->topo, k->cull, k->front, k->mode,
              k->dt, k->dw, k->df, k->st, k->ncol, k->cfmt[0], k->cfmt[1], k->cfmt[2], k->cfmt[3], k->dfmt, k->samples, k->a2c, k->blend_en, k->logic_en, k->logic_op, k->nattr, k->nbind); } }
    VkPipeline p; vkshader_t *stg[3] = {stg_in[0], stg_in[1], stg_in[2] ? vk_shader_gs_topo(stg_in[2], (VkPrimitiveTopology)k->topo) : NULL};
    if (k->gs_split && (k->gs_split > 2 || !stg[2] || !vk_shader_gs_split_ready(stg[2]))) return (VkPipeline)1;
    int bs = build_begin();
    if (bs >= 0) { snprintf(g_build[bs].what, sizeof g_build[bs].what, "graphics pipeline, shaders %016llx %016llx %016llx; topology %u, polygon mode %u, samples %u, "
            "%u colour targets (formats %u %u %u %u), depth format %u; raster discard %u, fragment depth variant %u, dual source %u, transform feedback %d, logic op %u, "
            "vertex attributes %u in %u bindings, integer-texel views %#llx/%#llx %#llx/%#llx, alternative code %u",
            (unsigned long long)k->key[0], (unsigned long long)k->key[1], (unsigned long long)k->key[2], k->topo, k->mode, k->samples, k->ncol, k->cfmt[0], k->cfmt[1], k->cfmt[2], k->cfmt[3],
            k->dfmt, k->raster_discard, k->fdz, k->dual, stg[0] ? stg[0]->xfb : 0, k->logic_en ? k->logic_op : 0, k->nattr, k->nbind,
            (unsigned long long)k->um[0], (unsigned long long)k->im[0], (unsigned long long)k->um[1], (unsigned long long)k->im[1], k->alt); g_build[bs].phase = 1; }
    VkPipelineShaderStageCreateInfo ss[3]; uint32_t ns = 0;
    VkShaderStageFlagBits fl[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    // the outputs each stage must declare for the next (vk_shader_module): vertex → geometry or fragment, geometry → fragment
    const vkshader_t *fs = stg[1] && !k->raster_discard ? stg[1] : NULL; uint32_t xo[3] = {0};
    if (stg[0]) xo[0] = (stg[2] ? stg[2]->in_locs : fs ? fs->in_locs : 0) & ~stg[0]->out_locs;
    if (stg[2]) xo[2] = (fs ? fs->in_locs : 0) & ~stg[2]->out_locs;
    uint32_t vi_flip = 0;
    if (stg[0]) for (uint32_t a = 0; a < k->nattr; a++) {
        uint32_t l = k->attr[a].loc; int kind = vertex_integer_kind((VkFormat)k->attr[a].fmt);
        if ((kind == 2 && (stg[0]->local[1] >> l & 1)) || (kind == 1 && (stg[0]->local[2] >> l & 1))) vi_flip |= 1u << l;
    }
    for (int i = 0; i < 3; i++) if (stg[i] && !(i == 1 && k->raster_discard)) {
        VkShaderModule mod = i == 2 && k->gs_split ? stg[i]->gs_mod[k->gs_split - 1] :
            vk_shader_module(stg[i], k->um[i], k->im[i], k->alt && !k->um[i] && !k->im[i] && !xo[i] ? 1u << 31 : xo[i], i == 1 ? (k->fdz ? 1 : 0) | (k->dual ? 2 : 0) : 0, i == 0 ? vi_flip : 0);
        ss[ns++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, vk_sg_pin(fl[i], NULL), 0, fl[i], mod, "main", NULL};   // (pNext: DM_VK_SUBGROUP_PIN)
    }
    int flat = (int)k->debug_mode;
    if (!fs) flat = 0;
    if (flat & 1) {   // DM_VK_X_FLAT_VS: the fragment stage becomes flatdbg.frag
        static VkShaderModule fm; if (!fm) { VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof flatdbg_frag_spv, flatdbg_frag_spv}; VKCHK(vkCreateShaderModule(V.dev, &mi, NULL, &fm)); }
        for (uint32_t i = 0; i < ns; i++) if (ss[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) ss[i].module = fm;
    }
    if (flat) { static int n; if (n++ < 12) VKLOG("debugging: vertex shader %016llx with fragment %016llx: mode %d (1 magenta, 2 no depth test, 4 no blending); %u colour targets, blending %#x (target 0: src %u dst %u op %u, alpha src %u dst %u op %u, write mask %#x), depth test %u write %u func %u",
        (unsigned long long)stg[0]->key, (unsigned long long)fs->key, flat, k->ncol, k->blend_en, k->bl[0].src, k->bl[0].dst, k->bl[0].eq, k->bl[0].srca, k->bl[0].dsta, k->bl[0].eqa, k->wmask[0], k->dt, k->dw, k->df); }
    VkVertexInputAttributeDescription ad[16]; VkVertexInputBindingDescription bd[17]; VkVertexInputBindingDivisorDescriptionEXT dd[17]; uint32_t ndd = 0;
    for (uint32_t i = 0; i < k->nattr; i++) ad[i] = (VkVertexInputAttributeDescription){k->attr[i].loc, k->attr[i].binding, (VkFormat)k->attr[i].fmt, k->attr[i].off};
    for (uint32_t i = 0; i < k->nbind; i++) { bd[i] = (VkVertexInputBindingDescription){k->bind[i].binding, k->bind[i].stride, (VkVertexInputRate)k->bind[i].rate};
        if (k->bind[i].rate && k->bind[i].divisor > 1) {
            if (V.has_divisor) dd[ndd++] = (VkVertexInputBindingDivisorDescriptionEXT){k->bind[i].binding, k->bind[i].divisor};
            else VKRED_ONCE(176, "vertex stream stepping per %u instances on a device without vertexAttributeInstanceRateDivisor: read per instance", k->bind[i].divisor); } }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvs = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, NULL, ndd, dd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, ndd ? &dvs : NULL, 0, k->nbind, bd, k->nattr, ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, (VkPrimitiveTopology)k->topo, k->restart};
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, NULL, k->clip_neg1};
    VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, k->clip_neg1 && V.has_depth_clip_ctl ? &dcc : NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, k->depth_clamp, k->raster_discard,
        k->mode == 0 ? VK_POLYGON_MODE_POINT : k->mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL, k->cull, k->front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        k->bias_en, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)k->samples, 0, 0, NULL, k->a2c, 0};
    if (flat & 8) ms.alphaToCoverageEnable = 0;
    if (flat & 16) rs.depthBiasEnable = 0;
    VkStencilOpState sf = {sop(k->sfail[0]), sop(k->szpass[0]), sop(k->szfail[0]), cmpop(k->sf[0]), 0xff, 0xff, 0}, sb = {sop(k->sfail[1]), sop(k->szpass[1]), sop(k->szfail[1]), cmpop(k->sf[1]), 0xff, 0xff, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, k->dt, k->dt && k->dw, cmpop(k->df), 0, k->st, sf, sb, 0, 1};
    if (flat & 2) { ds.depthTestEnable = ds.depthWriteEnable = ds.stencilTestEnable = 0; }
    if (flat & 32) ds.depthTestEnable = ds.depthWriteEnable = 0;
    if (flat & 64) ds.stencilTestEnable = 0;
    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < k->ncol; i++) cba[i] = (VkPipelineColorBlendAttachmentState){(k->blend_en >> i) & 1, bfac(k->bl[i].src), bfac(k->bl[i].dst), bop(k->bl[i].eq),
        bfac(k->bl[i].srca), bfac(k->bl[i].dsta), bop(k->bl[i].eqa), k->wmask[i]};
    if (flat & 4) for (uint32_t i = 0; i < k->ncol; i++) { cba[i].blendEnable = 0; cba[i].colorWriteMask = 0xf; }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, k->logic_en, (VkLogicOp)k->logic_op, k->ncol, cba, {0, 0, 0, 0}};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof dyn / sizeof *dyn, dyn};
    VkFormat cf[8]; for (uint32_t i = 0; i < k->ncol; i++) cf[i] = (VkFormat)k->cfmt[i];
    VkFormat df = (VkFormat)k->dfmt;
    int has_stencil = df == VK_FORMAT_D24_UNORM_S8_UINT || df == VK_FORMAT_D32_SFLOAT_S8_UINT || df == VK_FORMAT_S8_UINT;
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, k->ncol, cf, df == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : df, has_stencil ? df : VK_FORMAT_UNDEFINED};
    VkRenderPass prp = rp_on() ? rp_compat(k->ncol, k->cfmt, k->dfmt, k->samples) : VK_NULL_HANDLE;   // with render passes, pipelines are made against a compatible one
    if (!prp && !V.has_dynrender) { build_end(bs); return (VkPipeline)1; }   // no compatible render pass and no dynamic rendering: no valid pipeline (pass_commit skips such passes)
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, prp ? NULL : &rci, 0, ns, ss, &vi, &ia, NULL, &vs, &rs, &ms, &ds, &cb, &dy, layout, prp, 0, VK_NULL_HANDLE, -1};
    if (bs >= 0) g_build[bs].phase = 2;
    VkResult r = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &p);
    vk_pipeline_cache_changed();
    build_end(bs);
    static uint64_t n; uint64_t c = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
    if (r) { LOG("[RED] [vk] graphics pipeline failed (%d): shaders %016llx %016llx %016llx", r, (unsigned long long)k->key[0], (unsigned long long)k->key[1], (unsigned long long)k->key[2]); p = (VkPipeline)1; }
    else if (c % 200 == 0) VKLOG("%llu graphics pipelines", (unsigned long long)c);
    __atomic_add_fetch(&g_vkst.pipes_built, 1, __ATOMIC_RELAXED);
    return p;   // (VkPipeline)1 = failed
}

