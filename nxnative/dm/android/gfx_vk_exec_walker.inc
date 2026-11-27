// ---------------- the record walker ----------------
static int rt_same(const p_set_rt *r) {   // the targets the open pass draws into already
    int n = r->n > 8 ? 8 : r->n; if (n != S.rt_n) return 0;
    for (int i = 0; i < n; i++) {
        int has = r->color[i] != NULL && r->color[i]->magic == MAGIC('T', 'E', 'X', 0);
        if (has != S.rt_has_col[i] || (has && memcmp(r->color[i], &S.rt_col[i], sizeof S.rt_col[i]))) return 0;
        if (r->has_cview[i] != S.rt_has_cview[i] || (r->has_cview[i] && memcmp(&r->cview[i], &S.rt_cview[i], sizeof S.rt_cview[i]))) return 0;
    }
    int hd = r->depth != NULL && r->depth->magic == MAGIC('T', 'E', 'X', 0);
    if (hd != S.rt_has_dep || (hd && memcmp(r->depth, &S.rt_dep, sizeof S.rt_dep))) return 0;
    if (r->has_dview != S.rt_has_dview || (r->has_dview && memcmp(&r->dview, &S.rt_dview, sizeof S.rt_dview))) return 0;
    return 1;
}
static void set_rt(const p_set_rt *r) {
    if (g_x_tracepass == -2) {   // (DM_VK_TRACE_PASS=-2 lists the render target changes too: each target's serial and memory)
        char t[512]; int o = 0; for (int i = 0; i < r->n && i < 8 && o < 400; i++) { const tex_t *c = r->color[i]; o += snprintf(t + o, sizeof t - (size_t)o, " c%d %s", i, c && c->magic == MAGIC('T', 'E', 'X', 0) ? "" : "-");
            if (c && c->magic == MAGIC('T', 'E', 'X', 0)) o += snprintf(t + o, sizeof t - (size_t)o, "#%llu@%p", (unsigned long long)c->serial, (void *)c->mem); }
        const tex_t *d = r->depth; VKLOG("--- set targets:%s depth %s#%llu@%p%s", t, d && d->magic == MAGIC('T', 'E', 'X', 0) ? "" : "(none) ", d ? (unsigned long long)d->serial : 0ull, d ? (void *)d->mem : NULL,
                                         S.pass_open && !g_x_nokeep && rt_same(r) ? " (the same: the pass goes on)" : ""); }
    if (S.pass_open && !g_x_nokeep && rt_same(r)) { g_vkst.rt_kept++; return; }   // the same targets again (Unity sets them before many draws): the pass goes on
    vk_end_pass();
    S.rt_n = r->n > 8 ? 8 : r->n;
    for (int i = 0; i < S.rt_n; i++) {
        S.rt_has_col[i] = r->color[i] != NULL && r->color[i]->magic == MAGIC('T', 'E', 'X', 0); if (S.rt_has_col[i]) S.rt_col[i] = *r->color[i];
        S.rt_bad[i] = r->color[i] && !S.rt_has_col[i] ? r->color[i]->magic | 1u : 0;
        S.rt_has_cview[i] = r->has_cview[i]; if (r->has_cview[i]) S.rt_cview[i] = r->cview[i];
    }
    S.rt_has_dep = r->depth != NULL && r->depth->magic == MAGIC('T', 'E', 'X', 0); if (S.rt_has_dep) S.rt_dep = *r->depth;
    S.rt_has_dview = r->has_dview; if (r->has_dview) S.rt_dview = r->dview;
}
static void bind_handles_rec(uint32_t op, const void *a) {
    p_bind_handles tmp; const p_bind_handles *b = bind_rec(op, a, &tmp);
    if (b->defer && b->src && b->n > 0 && b->n <= 32) memcpy((void *)b->h, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
    int st = b->stage; if (st < 0 || st > 5) return;
    for (int i = 0; i < b->n; i++) {
        int slot = b->first + i;
        uint64_t *d = NULL;
        uint64_t *q = NULL;
        if (op == OP_BIND_TEX || op == OP_BIND_TEX1) { if (slot >= 0 && slot < 256) { d = &S.tex[st][slot]; q = &S.tex_sseq[st][slot]; } }
        else if (op == OP_BIND_SAMP) { if (slot >= 0 && slot < 128) { d = &S.samp[st][slot]; q = &S.samp_sseq[st][slot]; } }
        else if (op == OP_BIND_IMG) { if (slot >= 0 && slot < 32) d = &S.img[st][slot]; }
        if (d && *d != b->h[i]) { int code = op == OP_BIND_SAMP ? 2 : op == OP_BIND_IMG ? 3 : 1; uint64_t k = (uint64_t)code << 16 | (uint64_t)slot; g_c2h[st] ^= c2mix(k, *d) ^ c2mix(k, b->h[i]); }   // (c2 by its slots)
        if (q && *q != b->sseq) { uint64_t k = (uint64_t)(op == OP_BIND_SAMP ? 5 : 4) << 16 | (uint64_t)slot; g_sseqh[st] ^= c2mix(k, *q) ^ c2mix(k, b->sseq); }
        if (d && (*d != b->h[i] || (q && *q != b->sseq))) { *d = b->h[i]; if (q) *q = b->sseq; g_bind_gen[st]++; }   // only a change counts: games bind the same handles again before many draws (stage_set's memo)
    }
}
uint32_t g_sub_ops[64];   // record ops replayed in the open submission, by op (logged when the driver refuses it)
static void exec_ops(const uint8_t *p);
// DM_VK_PROF=1 (start-up): a 1 ms CPU-time timer on the replay thread samples where it is; every 20 s the most frequent places are logged: offsets in this library,
// time in other libraries charged to the first caller of ours ("via:"), or with =2 the place itself (libc, the driver). Resolve offsets with llvm-symbolizer
// on the debug build's libdmhost.so. Simpleperf is refused on retail devices; this found the command pool resets and render pass begins that took half the replay
#include <signal.h>
#include <sys/syscall.h>
static _Atomic uint64_t g_prof_pc[16384], g_prof_leaf[16384], g_prof_lr[16384], g_prof_st[16384][6]; static _Atomic uint32_t g_prof_n; static uintptr_t g_prof_lo, g_prof_hi;   // this library's text, for the first frame of ours
static void prof_sig(int sig, siginfo_t *si, void *ucv) { (void)sig; (void)si; ucontext_t *u = ucv; uint64_t pc = u->uc_mcontext.pc;
    if (pc < g_prof_lo || pc >= g_prof_hi) {   // outside this library: charge it to our first caller on the frame chain (marked with bit 63)
        uint64_t lr = u->uc_mcontext.regs[30] & 0x7fffffffffull, fp = u->uc_mcontext.regs[29], sp = u->uc_mcontext.sp, c = 0;
        if (lr >= g_prof_lo && lr < g_prof_hi) c = lr;
        for (int k = 0; k < 12 && !c && fp && !(fp & 7) && fp >= sp && fp - sp < 0x400000; k++) { uint64_t r = ((const uint64_t *)(uintptr_t)fp)[1] & 0x7fffffffffull; if (r >= g_prof_lo && r < g_prof_hi) c = r; fp = ((const uint64_t *)(uintptr_t)fp)[0]; }
        if (c) pc = c | (1ull << 63); }
    uint32_t i = atomic_fetch_add(&g_prof_n, 1); if (i < 16384) { atomic_store(&g_prof_pc[i], pc); atomic_store(&g_prof_leaf[i], u->uc_mcontext.pc); atomic_store(&g_prof_lr[i], u->uc_mcontext.regs[30]);
        uint64_t fp = u->uc_mcontext.regs[29], sp = u->uc_mcontext.sp;   // the frame chain's return addresses (DM_VK_PROF_OUT)
        for (int k = 0; k < 6; k++) { uint64_t r = 0; if (fp && !(fp & 7) && fp >= sp && fp - sp < 0x400000) { r = ((const uint64_t *)(uintptr_t)fp)[1] & 0x7fffffffffull; fp = ((const uint64_t *)(uintptr_t)fp)[0]; } else fp = 0; atomic_store(&g_prof_st[i][k], r); } } }
static void prof_tick(void) {
    // armed by the first DM_VK_PROF=1 it sees, read every 64 frames: a ctl.txt env line can start it after loading (Xenoblade Chronicles 3 crashed loading a save with it on from start-up)
    static int on; static unsigned chk; static uint64_t last; if (!on && !(chk++ & 63)) { const char *e = getenv("DM_VK_PROF"); on = e && atoi(e);
        if (on) { Dl_info d0; if (dladdr((void *)prof_tick, &d0)) { g_prof_lo = (uintptr_t)d0.dli_fbase; g_prof_hi = g_prof_lo + 0x4000000; }
            struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = prof_sig; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &sa, NULL);
            // DM_VK_PROF_TID=<tid>: sample that thread of the process instead (its CPU-time clock; e.g. the game's main thread), and DM_VK_PROF_OUT=<file>: append the raw samples (leaf pc, lr) there
            const char *tt = getenv("DM_VK_PROF_TID"); pid_t tid = tt && atoi(tt) > 0 ? (pid_t)atoi(tt) : (pid_t)syscall(SYS_gettid);
            clockid_t clk = tt && atoi(tt) > 0 ? (clockid_t)((~(unsigned)tid << 3) | 6) : CLOCK_THREAD_CPUTIME_ID;   // Linux MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)
            struct sigevent ev; memset(&ev, 0, sizeof ev); ev.sigev_notify = SIGEV_THREAD_ID; ev.sigev_signo = SIGPROF; ev._sigev_un._tid = tid;
            timer_t tm; if (!timer_create(clk, &ev, &tm)) { struct itimerspec it = {{0, 1000000}, {0, 1000000}}; timer_settime(tm, 0, &it, NULL); VKLOG("[prof] sampling thread %d every 1 ms of its CPU time", (int)tid); } else VKLOG("[RED] [prof] timer_create for thread %d failed", (int)tid); } }
    if (!on) return; uint64_t now = tnow(); if (!last) last = now; if (now - last < 20000000000ull) return; last = now;
    uint32_t n = atomic_exchange(&g_prof_n, 0); if (n > 16384) n = 16384;
    { const char *po = getenv("DM_VK_PROF_OUT"); FILE *pf = po ? fopen(po, "ab") : NULL;
      if (pf) { for (uint32_t i = 0; i < n; i++) { uint64_t v[8] = {atomic_load(&g_prof_leaf[i]), atomic_load(&g_prof_lr[i])}; for (int k = 0; k < 6; k++) v[2 + k] = atomic_load(&g_prof_st[i][k]); fwrite(v, sizeof v, 1, pf); } fclose(pf); } }
    Dl_info di; uintptr_t base = dladdr((void *)prof_tick, &di) ? (uintptr_t)di.dli_fbase : 0;
    static uint64_t pcs[16384]; static uint32_t cnt[16384]; int np = 0;
    static int leaves = -1; if (leaves < 0) { const char *e = getenv("DM_VK_PROF"); leaves = e && atoi(e) == 2; }   // DM_VK_PROF=2: where the time is, not who called
    for (uint32_t i = 0; i < n; i++) { uint64_t pc = atomic_load(leaves ? &g_prof_leaf[i] : &g_prof_pc[i]); int k = 0; for (; k < np; k++) if (pcs[k] == pc) break; if (k == np) { pcs[np] = pc; cnt[np++] = 0; } cnt[k]++; }
    char line[4000]; int o = snprintf(line, sizeof line, "[prof] %u samples, top pcs (library offset:count):", n);
    for (int t = 0; t < 40 && o < (int)sizeof line - 40; t++) { int b = -1; for (int k = 0; k < np; k++) if (cnt[k] && (b < 0 || cnt[k] > cnt[b])) b = k; if (b < 0) break;
        uint64_t pc = pcs[b]; Dl_info d2;
        if (pc >> 63) o += snprintf(line + o, sizeof line - o, " via:%llx:%u", (unsigned long long)((pc & ~(1ull << 63)) - base), cnt[b]);
        else if (pc - base < 0x10000000) o += snprintf(line + o, sizeof line - o, " %llx:%u", (unsigned long long)(pc - base), cnt[b]);
        else if (dladdr((void *)(uintptr_t)pc, &d2) && d2.dli_fname) o += snprintf(line + o, sizeof line - o, " %s(%s)+%llx:%u", strrchr(d2.dli_fname, '/') ? strrchr(d2.dli_fname, '/') + 1 : d2.dli_fname, d2.dli_sname ? d2.dli_sname : "?", (unsigned long long)(pc - (uintptr_t)(d2.dli_saddr ? d2.dli_saddr : d2.dli_fbase)), cnt[b]);
        else o += snprintf(line + o, sizeof line - o, " ?%llx:%u", (unsigned long long)pc, cnt[b]);
        cnt[b] = 0; }
    VKLOG("%s", line);
}
// Every record is written by this same library, so a size that is not a whole number of 8-byte words means the stream went out of step (a copied command handle
// that points into a rewritten buffer, or a payload read short). Print the records just before it and stop: going on jumps by a garbage size into unmapped
// memory, which is what crashed inside an inlined texture copy (Pokémon Shining Pearl, 2026-11-15: "unknown record op 3194720768", SEGV in copy_t2t).
// An op outside the enum with a sound size is skipped instead, logged: the size keeps the stream in step whatever the op means, while a size cannot be
// guessed at all.
typedef struct { uint32_t op, size; intptr_t off; } lastrec_t;
static lastrec_t g_lastrec[16];
static unsigned g_lastrec_n;
static const uint8_t *g_rec_base;
static void rec_stream_lost(const uint8_t *p, op_hdr h, const char *why) {
    unsigned n = g_lastrec_n < 16 ? g_lastrec_n : 16;
    VKLOG("[RED] [vk] record stream out of step at %+lld (%s): op %u size %u",
          (long long)(p - g_rec_base), why, h.op, h.size);
    for (unsigned i = 0; i < n; i++) {
        unsigned k = (g_lastrec_n - n + i) & 15;
        VKLOG("[RED] [vk]   %2u. op %u size %u at %+lld", i + 1, g_lastrec[k].op, g_lastrec[k].size,
              (long long)g_lastrec[k].off);
    }
    abort();
}
void mtl_exec(const uint8_t *p) {   // timed at the top level only (OP_CALL nests)
    static int depth; uint64_t t0 = depth ? 0 : tnow(); depth++; if (depth == 1) { prof_tick(); g_rec_base = p; g_lastrec_n = 0; }
    exec_ops(p);
    if (!--depth) g_vkst.ns_exec += tnow() - t0;
}
static void exec_ops(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7)) rec_stream_lost(p, h, "bad size");
        if (h.op >= OP_COUNT_) { static int nred; if (nred++ < 8) VKLOG("[RED] [vk] record op %u outside the enum (size %u) at %+lld: skipped", h.op, h.size, (long long)(p - g_rec_base)); p += h.size; continue; }
        g_lastrec[g_lastrec_n++ & 15] = (lastrec_t){h.op, h.size, (intptr_t)(p - g_rec_base)};
        if (h.op < 64) g_sub_ops[h.op]++;
        uint64_t skip = g_skip_ops_live;   // DM_VK_SKIP_OPS (debugging, live): record ops not replayed
        if (h.op < 64 && h.op > 3 && (skip >> h.op & 1)) { p += h.size; continue; }
        const uint8_t *a = p + 8;
        switch (h.op) {
        case OP_END: return;
        case OP_BEGIN: break;
        case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); exec_ops((const uint8_t *)(uintptr_t)t); break; }
        case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
        case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8);
            int kind = t == 0 || t == 0x10 ? 1 : 3;   // the render condition's: 1 timestamp, 2 a counted samples-passed report, 3 a value that measured nothing
            if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED && vk_oq_enabled()) { if (vk_sub_samples_passed(ad)) kind = 2; }
            else mtl_sub_counter(ad, (uint32_t)t);
            if (DM_GAME_RENDER_COND) rc_note(ad, kind);
            if (g_x_tracepass == -2) VKLOG("--- report counter type %#llx to %#llx", (unsigned long long)t, (unsigned long long)ad); break; }
        case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED && vk_oq_enabled()) vk_sub_samples_reset(); break; }
        case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8);
            if (!sync_replay_fence(s, mtl_last_fence() + 1)) { static int n; if (n++ < 4) VKLOG("fence on sync %p skipped: the game has finalized or freed it (nvn_obj.h sync_replay_fence)", (void *)s); }
            break; }
        // (DM_VK_TRACE_PASS=-2 lists the buffer writes too: copies, clears and uniform updates, by address)
        case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s_, d, n);
            if (g_x_tracepass == -2 || mdi_log_now()) VKLOG("--- copy %#llx → %#llx, %llu bytes, after pass %d", (unsigned long long)s_, (unsigned long long)d, (unsigned long long)n, g_sub_pass - 1); break; }
        case OP_CLEAR_BUF: { const p_clear_buf *c = (const p_clear_buf *)a; mtl_sub_fill(c->addr, c->size, c->value);
            if (g_x_tracepass == -2 || mdi_log_now()) VKLOG("--- buffer clear %#llx, %llu bytes to %#x, after pass %d", (unsigned long long)c->addr, (unsigned long long)c->size, c->value, g_sub_pass - 1); break; }
        case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); vk_sub_update(d, a + 16, n);
            if (g_x_tracepass == -2) VKLOG("--- uniform update %#llx, %llu bytes, after pass %d", (unsigned long long)d, (unsigned long long)n, g_sub_pass - 1); break; }
        case OP_SET_RT: set_rt((const p_set_rt *)a); break;
        case OP_CLEAR_COLOR: if (!g_rc_skip) { clear_color((const p_clear_color *)a); pixel_history_record(1, -1, 0, 0, 0, 0, 0); } break;
        case OP_CLEAR_DS: if (!g_rc_skip) { clear_ds((const p_clear_ds *)a); pixel_history_record(2, -1, 0, 0, 0, 0, 0); } break;
        case OP_VIEWPORTS: { const p_viewports *v = (const p_viewports *)a; for (int i = 0; i < v->n && v->first + i < 16; i++) memcpy(S.vp[v->first + i], v->v[i], 16); break; }
        case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.sc_set = 1; break;
        case OP_DEPTH_RANGE: { const p_depth_range *d = (const p_depth_range *)a; S.dnear = d->n; S.dfar = d->f; break; }
        case OP_BIND_PROGRAM: bind_program((const p_bind_program *)a); break;
        case OP_BIND_STATE: { const p_bind_state *s = (const p_bind_state *)a;
            switch (s->kind) {
            case 0: { blend_st b; memcpy(&b, s->bytes, sizeof b); if (b.target < 8) S.blend[b.target] = b; break; }
            case 1: memcpy(&S.color, s->bytes, sizeof S.color); break;
            case 2: { chmask_st mask;memcpy(&mask,s->bytes,sizeof mask);bind_chmask(mask.mask);break; }
            case 3: memcpy(&S.ms, s->bytes, sizeof S.ms); break;
            case 4: memcpy(&S.poly, s->bytes, sizeof S.poly); break;
            case 5: memcpy(&S.ds, s->bytes, sizeof S.ds); break;
            }
            break; }
        case OP_BIND_VATTR: { const p_bind_vattr *v = (const p_bind_vattr *)a; memset(S.vattr, 0, sizeof S.vattr); for (int i = 0; i < v->n && i < 16; i++) S.vattr[v->first + i] = v->st[i]; break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *v = (const p_bind_vstream *)a; for (int i = 0; i < v->n && v->first + i < 16; i++) S.vstream[v->first + i] = v->st[i]; break; }
        case OP_BIND_VBUF: { const p_bind_vbuf *v = (const p_bind_vbuf *)a; for (int i = 0; i < v->n && v->first + i < 16; i++) S.vbuf[v->first + i] = v->r[i]; break; }
        case OP_BIND_UBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16 && memcmp(&S.ubo[b->stage][b->index], b, sizeof *b)) { S.ubo[b->stage][b->index] = *b; g_ubo_gen[b->stage]++; } break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16 && memcmp(&S.ssbo[b->stage][b->index], b, sizeof *b)) {
            const p_bind_buf *o = &S.ssbo[b->stage][b->index]; uint64_t k = 0x60000ull | (uint64_t)b->index;
            g_ssboh[b->stage] ^= c2mix(k, o->addr) ^ c2mix(k | 0x100, o->size) ^ c2mix(k, b->addr) ^ c2mix(k | 0x100, b->size);
            S.ssbo[b->stage][b->index] = *b; g_bind_gen[b->stage]++; } break; }
        case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_BIND_TEX1: bind_handles_rec(h.op, a); break;
        case OP_STENCIL: { const p_stencil *s = (const p_stencil *)a;
            for (int f = 0; f < 2; f++) if (s->face & (1 << f)) { if (s->which == 0) S.smask[f] = s->value_mask; else if (s->which == 1) S.swmask[f] = s->mask; else S.sref[f] = s->ref; }
            break; }
        case OP_POLY_OFFSET: S.pofs = *(const p_poly_offset *)a; break;
        case OP_BLEND_COLOR: memcpy(S.blendc, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_depth_clamp *)a)->on; break;
        case OP_RAST_DISCARD: S.rast_discard = ((const p_rast_discard *)a)->on; break;
        case OP_PRIM_RESTART: S.prim_restart = ((const p_prim_restart *)a)->on; break;
        case OP_DRAW: { if (g_rc_skip) break; p_draw t; const p_draw *d = draw_rec(a, h.size, &t); uint64_t before = g_vkst.draws; draw(d);
            int drawn = g_vkst.draws != before; if (!drawn) vk_oq_draw_skipped();   // whichever path left the draw out: what is not measured reads as visible
            pixel_history_record(3, g_draw_i - 1, drawn, d->count, d->instances, 0, d->first); break; }
        case OP_DRAW_IDX: { if (g_rc_skip) break; p_draw_idx t; const p_draw_idx *d = draw_idx_rec(a, h.size, &t); uint64_t before = g_vkst.draws; draw_idx(d);
            int drawn = g_vkst.draws != before; if (!drawn) vk_oq_draw_skipped();
            pixel_history_record(4, g_draw_i - 1, drawn, d->count, d->instances, d->ibuf, d->base_vertex); break; }
        case OP_DISPATCH: dispatch((const p_dispatch *)a); break;
        case OP_BARRIER: {   // the pass ends at every barrier: kept across XC3's fragment- and primitive-order barriers (0x1 0x2 0x4) its characters drew torn in some frames
            int32_t bits = *(const int32_t *)a;
            if (g_x_listpasses) VKLOG("    barrier %#x%s", bits,
                S.pass_open == 2 && !(bits & ~0x7) ? " (deferred pass: kept for loadOp clear)" :
                g_order_keep && S.pass_open == 1 && !(bits & ~0x7) ? " (order only: decided at the next draw)" : "");
            // A deferred pass (pass_open == 2) has no draws yet, only folded loadOp=CLEARs; an order-only barrier is already satisfied before the first draw by Vulkan's loadOp ordering.
            if (S.pass_open == 2 && !(bits & ~0x7)) break;
            if (g_order_keep && S.pass_open == 1 && !(bits & ~0x7)) { g_order_pending = 1; break; }   // (g_order_keep)
            vk_end_pass(); vk_img_barrier_all(vk_cmd()); g_order_pending = 0; break; }
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: {   // bound handles were rebased when recorded (hle_nvn.c bind_handles); handles read from memory are rebased by these (mem_handle)
            const p_set_pool *sp = (const p_set_pool *)a; int base = sp->pool ? nvn_dpool_base(sp->pool, h.op == OP_SET_TEX_POOL) : 0;
            if (h.op == OP_SET_TEX_POOL) g_pool_tex_base = base; else g_pool_samp_base = base;
            break; }
        case OP_COPY_STRIDE: { const p_copy_stride *c = (const p_copy_stride *)a; if (c->image) S.image_stride = c->stride; else S.row_stride = c->stride; break; }
        case OP_COPY_B2T: copy_b2t((const p_copy_b2t *)a, 1); break;
        case OP_COPY_T2B: copy_b2t((const p_copy_b2t *)a, 0); break;
        case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
        case OP_CLEAR_TEX: clear_tex((const p_clear_tex *)a); break;
        case OP_RENDER_COND: render_cond((const p_render_cond *)a); break;   // recorded only with the profile key render_cond (hle_nvn.c)
        case OP_DRAW_TF: { if (g_rc_skip) break; uint64_t before = g_vkst.draws; draw_tf((const p_draw_tf *)a); if (g_vkst.draws == before) vk_oq_draw_skipped(); break; }
        case OP_TF_BIND: { const p_tf_bind *b = (const p_tf_bind *)a; if (b->index >= 0 && b->index < 4) g_tf[b->index] = *b; break; }
        case OP_TF_STATE: if (!V.has_xfb) VKRED_ONCE(80, "transform feedback: the device has none, its draws write nothing"); tf_state((const p_tf_state *)a); break;
        case OP_VP_SWIZZLE: { const p_vp_swizzle *b = (const p_vp_swizzle *)a; memcpy(S.vp_sw[b->first], b->sw, 4 * (size_t)b->n); break; }
        default: VKRED_ONCE(82, "unknown record op %u", h.op); break;
        }
        p += h.size;
    }
}
