// ---------------- conditional rendering (profile render_cond) ----------------
// As nvn_mtl.m (OP_RENDER_COND): SetRenderEnableConditional draws while the 64-bit counters of the reports at addr and addr + 16 are equal / differ, until SetRenderEnable(true).
// Evaluated on the CPU at replay from the records in memory: a report replayed earlier in this submission is not written yet (that happens when the GPU completes), so a timestamp
// one counts with its known counter 0 (vk_sub_complete writes 0 there under the key) and any other one is unknown, which keeps drawing. Inside a block evaluated false, draws and
// clears are dropped. Mario vs. Donkey Kong (gsys, as Tears of the Kingdom and Animal Crossing) clears its HDR scene target under such a condition after lighting it: drawn
// anyway, its levels were black under the HUD
static int g_rc_skip;   // a render-enable block evaluated false is active
// Each report address's latest report: its kind and the fence of the submission that recorded it. kind 1 a timestamp report (its counter field is 0 under the
// key, so 0 is known while its submission is not done), kind 2 a samples-passed report counted on the GPU (memory holds the answer once that submission is
// done, and the condition compares it), kind 3 a report that measured nothing -- a samples-passed one written as a constant, or another counter -- which is
// unknown while it is the latest report at that address (a later timestamp or count there decides again). While the submission that recorded a count is being
// recorded or is still on the GPU the condition is unknown and draws: the
// check runs on the CPU at record time, and deciding it on the GPU would take VK_EXT_conditional_rendering over a predicate built from the query results.
// One growing table, not a per-submission array plus a permanent unknown set: the fixed 4096-slot array filled up in The Legend of Zelda: Ocarina of Time
// (its reports move through their buffer as the camera turns), after which every report and every condition walked all 4096 slots (0.9 s of the replay
// thread's 4.4 s over 8 s of camera turns, Odin 3) and new addresses were no longer recorded. With DM_VK_OCCLUSION=0 the entry also keeps the fact that
// a report wrote no timestamp (ever_nonts): counting off, the rule of the builds before this counter applies unchanged. Replay thread only
typedef struct { uint64_t fence; int kind; int ever_nonts; } rc_ent;
static nvn_hmap g_rc = {.klen = 8};
static void rc_note(uint64_t a, int kind) {
    rc_ent *e = nvn_hm_get(&g_rc, &a);
    if (!e) { e = malloc(sizeof *e); if (!e) return; e->ever_nonts = 0; nvn_hm_put(&g_rc, &a, e); }
    e->kind = kind; e->fence = mtl_last_fence() + 1;   // the fence this submission commits with
    if (kind != 1) e->ever_nonts = 1;
    if (g_rc.n >= 4096 && !(g_rc.n & (g_rc.n - 1))) VKLOG("render conditions: %u report addresses so far", g_rc.n);
}
static int rc_value(uint64_t a, uint64_t *v) {   // 0 = unknown at replay time
    rc_ent *e = nvn_hm_get(&g_rc, &a);
    if (!vk_oq_enabled()) {   // the rule before the counter, exactly: a report that wrote no timestamp is unknown for good, a timestamp reads 0 inside its own submission
        if (e && e->ever_nonts) return 0;
        if (e && e->kind == 1 && e->fence == mtl_last_fence() + 1) { *v = 0; return 1; }
        *v = *(const volatile uint64_t *)(uintptr_t)a; return 1;
    }
    if (e && e->kind == 3) return 0;
    if (e && vk_fence_done() < e->fence) { if (e->kind != 1) return 0; *v = 0; return 1; }   // not written yet: a timestamp's 0 is known, a count is not
    *v = *(const volatile uint64_t *)(uintptr_t)a; return 1;
}
static void render_cond(const p_render_cond *rc) {
    if (rc->mode >= 2) { g_rc_skip = rc->mode == 3; return; }
    uint64_t x = 0, y = 0; int known = rc_value(rc->addr, &x) & rc_value(rc->addr + 16, &y);
    g_rc_skip = known && (rc->mode ? x == y : x != y);
    if (g_x_tracepass == -2) { uint64_t a1 = rc->addr + 16; rc_ent *e0 = nvn_hm_get(&g_rc, &rc->addr), *e1 = nvn_hm_get(&g_rc, &a1);
        VKLOG("--- render condition mode %d at %#llx: %s (%s), memory %llu / %llu, report kind %d / %d, fence %llu / %llu, done %llu", rc->mode, (unsigned long long)rc->addr,
              g_rc_skip ? "skip" : "draw", known ? "known" : "unknown",
              (unsigned long long)*(const volatile uint64_t *)(uintptr_t)rc->addr, (unsigned long long)*(const volatile uint64_t *)(uintptr_t)a1,
              e0 ? e0->kind : 0, e1 ? e1->kind : 0, (unsigned long long)(e0 ? e0->fence : 0), (unsigned long long)(e1 ? e1->fence : 0), (unsigned long long)vk_fence_done()); }
}

static void exec_begin(int continuing) {   // internal segments retain the current logical NVN and conditional state
    static int inited; if (!inited) { inited = 1; state_reset(); crash_add_note_hook(build_dump); prewarm_start(); compute_warm_start(); }
    if (!continuing) { g_rc_skip = 0; }   // (g_rc spans submissions: a report's answer arrives with its completion)
    { void vk_tex_release_drain(void); vk_tex_release_drain(); }   // Finalized textures' CPU-written marks that no queued job needs any more (gfx_vk_tex.c)
    S.pass_open = 0; S.bound_pipe = VK_NULL_HANDLE; g_cb_draws = 0; g_ord_parts = 0; g_psplit_now = 0; g_set_epoch++; x_read();
    if (g_prof_on) { extern uint64_t g_pkg_ns, g_pkg_n; memset(g_prof_ns, 0, sizeof g_prof_ns); g_prof_t0 = tick(); g_prof_pkg0 = g_pkg_ns; g_prof_npkg0 = g_pkg_n; g_prof_pipes0 = g_vkst.pipes_built; }
    { extern volatile int g_vk_samp_log; if (g_vk_samp_log > 0) VKLOG("submission begins (present %llu, thread %d)", (unsigned long long)g_tex_frame, (int)gettid()); }
}

void vk_exec_reset(void) { exec_begin(0); }
void vk_exec_resume(void) { exec_begin(1); }

