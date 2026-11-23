// ---------------- conditional rendering (profile render_cond) ----------------
// As nvn_mtl.m (OP_RENDER_COND): SetRenderEnableConditional draws while the 64-bit counters of the reports at addr and addr + 16 are equal / differ, until SetRenderEnable(true).
// Evaluated on the CPU at replay from the records in memory: a report replayed earlier in this submission is not written yet (that happens when the GPU completes), so a timestamp
// one counts with its known counter 0 (vk_sub_complete writes 0 there under the key) and any other one is unknown, which keeps drawing. Inside a block evaluated false, draws and
// clears are dropped. Mario vs. Donkey Kong (gsys, as Tears of the Kingdom and Animal Crossing) clears its HDR scene target under such a condition after lighting it: drawn
// anyway, its levels were black under the HUD
static int g_rc_skip;   // a render-enable block evaluated false is active
#define RC_PEND 256
static struct { uint64_t addr; int kind; } g_rc_pend[RC_PEND]; static int g_rc_npend;   // reports replayed in this submission: kind 1 = timestamp (counter 0), 2 = another counter (unknown)
// Addresses ever written by a non-timestamp report: conditions on them keep drawing. A set that grows: the fixed 4096-slot table this was filled up in The Legend of
// Zelda: Ocarina of Time (its reports move through their buffer as the camera turns), after which every report and every condition walked all 4096 slots (0.9 s of the
// replay thread's 4.4 s over 8 s of camera turns, Odin 3) and new addresses were no longer recorded. Replay thread only
static nvn_hmap g_rc_occ = {.klen = 8};
static void rc_occ_add(uint64_t a) {
    if (nvn_hm_get(&g_rc_occ, &a)) return;
    nvn_hm_put(&g_rc_occ, &a, (void *)1);
    if (g_rc_occ.n >= 4096 && !(g_rc_occ.n & (g_rc_occ.n - 1))) VKLOG("render conditions: %u addresses written by occlusion reports so far", g_rc_occ.n);
}
static int rc_occ_has(uint64_t a) { return nvn_hm_get(&g_rc_occ, &a) != NULL; }
static void rc_note(uint64_t a, int kind) {
    if (kind == 2) rc_occ_add(a);
    for (int i = 0; i < g_rc_npend; i++) if (g_rc_pend[i].addr == a) { g_rc_pend[i].kind = kind; return; }
    if (kind == 2) return;   // (g_rc_occ holds it. Appended here too, a submission's occlusion reports filled the table and a timestamp report after them was not noted, so a
                             // condition on it read memory, not yet written, instead of its known counter 0. Ocarina of Time (DeathKhan/nx2android) clears its 768x432 target under
                             // such a condition at each bind and reports once: the later clears wiped the scene copy, opaque water)
    if (g_rc_npend < RC_PEND) { g_rc_pend[g_rc_npend].addr = a; g_rc_pend[g_rc_npend++].kind = kind; }
    else { static int said; if (!said) { said = 1; VKLOG("render conditions: more than %d timestamp reports in a submission, the rest read memory", RC_PEND); } }
}
static int rc_value(uint64_t a, uint64_t *v) {   // 0 = unknown at replay time
    if (rc_occ_has(a)) return 0;
    for (int i = g_rc_npend - 1; i >= 0; i--) if (g_rc_pend[i].addr == a) { if (g_rc_pend[i].kind != 1) return 0; *v = 0; return 1; }
    *v = *(const volatile uint64_t *)(uintptr_t)a; return 1;
}
static void render_cond(const p_render_cond *rc) {
    if (rc->mode >= 2) { g_rc_skip = rc->mode == 3; return; }
    uint64_t x = 0, y = 0; int known = rc_value(rc->addr, &x) & rc_value(rc->addr + 16, &y);
    g_rc_skip = known && (rc->mode ? x == y : x != y);
    if (g_x_tracepass == -2) { int k0 = -1, k1 = -1; for (int i = 0; i < g_rc_npend; i++) { if (g_rc_pend[i].addr == rc->addr) k0 = g_rc_pend[i].kind; if (g_rc_pend[i].addr == rc->addr + 16) k1 = g_rc_pend[i].kind; }
        VKLOG("--- render condition mode %d at %#llx: %s (%s), memory %llu / %llu, occlusion-written %d / %d, pending kind %d / %d", rc->mode, (unsigned long long)rc->addr, g_rc_skip ? "skip" : "draw", known ? "known" : "unknown",
            (unsigned long long)*(const volatile uint64_t *)(uintptr_t)rc->addr, (unsigned long long)*(const volatile uint64_t *)(uintptr_t)(rc->addr + 16), rc_occ_has(rc->addr), rc_occ_has(rc->addr + 16), k0, k1); }
}

static void exec_begin(int continuing) {   // internal segments retain the current logical NVN and conditional state
    static int inited; if (!inited) { inited = 1; state_reset(); crash_add_note_hook(build_dump); prewarm_start(); compute_warm_start(); }
    if (!continuing) { g_rc_npend = 0; g_rc_skip = 0; }
    { void vk_tex_release_drain(void); vk_tex_release_drain(); }   // Finalized textures' CPU-written marks that no queued job needs any more (gfx_vk_tex.c)
    S.pass_open = 0; S.bound_pipe = VK_NULL_HANDLE; g_cb_draws = 0; g_ord_parts = 0; g_psplit_now = 0; g_set_epoch++; x_read();
    if (g_prof_on) { extern uint64_t g_pkg_ns, g_pkg_n; memset(g_prof_ns, 0, sizeof g_prof_ns); g_prof_t0 = tick(); g_prof_pkg0 = g_pkg_ns; g_prof_npkg0 = g_pkg_n; g_prof_pipes0 = g_vkst.pipes_built; }
    { extern volatile int g_vk_samp_log; if (g_vk_samp_log > 0) VKLOG("submission begins (present %llu, thread %d)", (unsigned long long)g_tex_frame, (int)gettid()); }
}

void vk_exec_reset(void) { exec_begin(0); }
void vk_exec_resume(void) { exec_begin(1); }

