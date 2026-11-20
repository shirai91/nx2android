// ---------------- NVN state shadow ----------------
static struct {
    const prog_t *prog; vkshader_t *sh[6]; int prog_ok;
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][256], samp[6][128], img[6][32], tex_sseq[6][256], samp_sseq[6][128];   // *_sseq: the sampler registrations made before the bind (gfx_vk_tex.c vk_samp_at)
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    uint32_t vattr[16]; vs_st vstream[16]; NVNbufferRange vbuf[16];
    float vp[16][4]; float dnear, dfar; p_scissor sc; int sc_set; uint8_t vp_sw[16][4];   // vp_sw: viewport swizzles XOR the identity (p_vp_swizzle)
    int sref[2], smask[2], swmask[2];
    p_poly_offset pofs; float blendc[4]; int depth_clamp, rast_discard, prim_restart;
    tex_t rt_col[8], rt_dep; view_t rt_cview[8], rt_dview; uint8_t rt_has_col[8], rt_has_cview[8], rt_has_dep, rt_has_dview; int rt_n; uint32_t rt_bad[8];   // rt_bad: the first word of a colour slot that is not a texture object (traces)
    int64_t row_stride, image_stride;
    // the open render pass
    int pass_open; uint32_t pass_w, pass_h; VkFormat cfmt[8], dfmt; VkImageAspectFlags daspect; uint32_t samples; int ncol;
    VkPipeline bound_pipe; int rp_pass;   // rp_pass: the open pass began with vkCmdBeginRenderPass
} S;
static p_tf_bind g_tf[4]; static uint64_t g_tf_off[4]; static int g_tf_on, g_tf_in_pass; static uint32_t g_tf_verts;   // transform feedback: buffers, bytes written into each, capture on, a capture in the open pass (xfb_begin), vertices captured since Begin
static int g_depth_zero = 1, g_lower_left;
void mtl_set_depth_zero(int on) { g_depth_zero = on; }
void mtl_set_origin_lower_left(int on) { g_lower_left = on; }
// NVN's upper-left mode has clip space y up (as Metal and D3D): it is drawn with a flipped viewport over the same top-left rectangle, lower-left mode without
// one; the scissor is then never flipped and the winding follows nvn_mtl.m's. Drawn the other way, every blit flipped the picture (NEO docked: upside down)
// and the winding had to be inverted. DM_VK_FLIPY=0 brings the old way back
#include <dlfcn.h>
static int g_flipy;
static int g_x_depth_clear_color;
// Lower-left mode then stores the window image bottom-up (GL's rows), so the present flips it as nvn_mtl.m does (Celeste's MonoGame keeps NVN's default lower-left origin)
int vk_window_bottom_up(void) { return g_lower_left && g_flipy; }
static void state_reset(void) {
    memset(&S, 0, sizeof S);
    S.dnear = 0; S.dfar = 1;
    S.chmask.mask = 0xffffffffu;
    S.color.logic_op = 3;
    S.poly.front = 1; S.poly.mode = 2;
    S.ds.dw = 1; S.ds.df = 2; S.ds.sf_f = S.ds.sf_b = 8; S.ds.fail_f = S.ds.zfail_f = S.ds.zpass_f = S.ds.fail_b = S.ds.zfail_b = S.ds.zpass_b = 1;
    S.smask[0] = S.smask[1] = S.swmask[0] = S.swmask[1] = 0xff;
    for (int i = 0; i < 8; i++) { S.blend[i].target = i; S.blend[i].src_rgb = S.blend[i].src_a = 2; S.blend[i].dst_rgb = S.blend[i].dst_a = 1; S.blend[i].eq_rgb = S.blend[i].eq_a = 1; }
}
static void prewarm_start(void); static void compute_warm_start(void); static void build_dump(void);
static uint64_t g_set_epoch = 1;   // the submission the descriptor set cache belongs to (stage_set)
static int32_t g_pass_bb[4];   // bounding box of the scissors the open pass's draws used (x0 y0 x1 y1)
static int g_cb_draws;   // draws in the command buffer being recorded (vk_cmd_split after DM_VK_CB_DRAWS draws, 0 = never)
static int g_psplit, g_psplit_depth, g_psplit_now, g_ord_parts, g_setsplit;   // DM_VK_PASS_SPLIT / _DEPTH (prepare_draw); g_psplit_now: the pass being begun goes on with the one just ended (begin_pass cuts before it);
                                                                    // g_ord_parts: ordinary DM_VK_CB_DRAWS cuts in this submission, kept to the old 15 (MAIN_CBS was 16): the extra parts are for pass splits
static int g_var_now;   // the bisection's change for the draw being recorded (see g_bis_var)
static int g_tracing;   // the draw being recorded is the one the bisection found (its descriptors are logged)
// replay counters, printed by gfx_vk_present.c every 600 presents (draws done and skipped by reason, passes, clears folded into load ops, dispatches)
struct { uint64_t draws, skip_prog, skip_rt, skip_pipe, passes, folded, dispatches, pipes_built, rt_kept, ns_exec, ns_desc, ns_pipe, max_pass_draws; } g_vkst; static uint64_t g_pass_draws;   // ns_*: time in the replay, its descriptor sets, its pipeline keys and lookups
static inline uint64_t tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
// The per-draw timers (about 10 a draw, ~3,000 draws a frame): the arm64 counter register read directly, a few ns against clock_gettime's ~20-40, in ns
static inline uint64_t tick(void) {
#if defined(__aarch64__)
    static uint64_t q; if (!q) { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); q = (1000000000ull << 32) / f; }
    uint64_t c; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(c)); return (uint64_t)(((unsigned __int128)c * q) >> 32);
#else
    return tnow();
#endif
}
// Measurement switches, read again at every submission so DM_CTL "env" lines change them while the game runs (never for normal play: they draw wrong):
// DM_VK_X_NOBAR=1 no barrier before a pass, DM_VK_X_DEPTH=1 depth neither loaded nor stored, DM_VK_X_COLOR=1 colour not loaded unless cleared
static int g_x_nobar, g_x_depth, g_x_color, g_x_cbdraws, g_x_tracepass = -1, g_sub_pass;   // DM_VK_TRACE_PASS=n: log every draw of the submission's pass n (once)
static int g_x_listpasses;
static int g_c2fast = 1;   // DM_VK_C2_FAST (x_read; drv_shared)
// c2 by its slots (DM_VK_C2_FAST, live, default on): a c2 segment (bank != 0, not the slot-index form) is S.tex / S.img / S.samp of its stage and nothing else,
// and only bind_handles_rec changes those, so it keeps g_c2h[st], an order-free hash of every non-zero slot (O(1) a bind). drv_shared then finds an equal
// segment by that hash instead of building 4 KB and hashing 2.4 KB of it for every draw whose bindings changed (Sifu's bindless pool shaders all read c2:
// the driver segments went from 4 to 12 ms a frame), and builds a new one straight into the scratch memory
static uint64_t g_c2h[6];
static int g_c2_check; static uint64_t g_c2_checked, g_c2_bad;   // DM_VK_X_SETVERIFY: c2 segments found by g_c2h built again and compared (drv_shared)
static uint64_t g_sseqh[6], g_ssboh[6];   // the same for the slots' sampler registration counts (S.tex_sseq, S.samp_sseq) and the storage buffers (S.ssbo): with g_c2h, a stage's bindings (rmemo_key)
static inline uint64_t c2mix(uint64_t k, uint64_t v) {
    if (!v) return 0;
    uint64_t x = (k + 1) * 0x9E3779B97F4A7C15ull ^ v; x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 29; x *= 0x94D049BB133111EBull; return x ^ (x >> 32);
}
uint64_t vk_replay_ns(void) { return g_vkst.ns_exec; }   // the replay time so far (gfx_vk_present.c: a frame's cost for the adaptive rate)
// flicker experiments (DM_CTL env lines, read every submission): DM_VK_X_NOSETC no descriptor set reuse, DM_VK_X_NODRVC no driver segment reuse, DM_VK_X_LANDCUT deferred
// uniform updates land at each cut, DM_VK_X_NOKEEP no pass kept for the same targets, DM_VK_X_NOSHC no shadow reuse
int g_ubd_heap = 1;   // DM_VK_UBD_HEAP=0 (live): a deferred uniform update keeps no copy in ordinary memory, and the shadows read it back from scratch, as before (gfx_vk_core.c vk_sub_update)
int g_pool_slots = 1;   // DM_VK_POOL_SLOTS=0 (live): gfx_vk_core.c vk_pool_of looks in the last four pools it found alone, without its 512 page slots (as before them)
int g_x_inflkeep;   // DM_VK_UBD_KEEP=1 (live): every landed uniform update stays listed until its submission completes, as before (gfx_vk_core.c vk_ubd_land), for A/B runs
static int g_fdz = 1;  // DM_VK_FDZ (live): fragment-depth variants for draws on depth written by fragment shaders (pipeline key fdz)
static int g_gs_split_mode; // 1: discarded, single-output memory pass; 2: pure rasterization
static int g_gs_split;      // DM_VK_GS_SPLIT=1 (a profile env row; live, read in x_read): the split below on Adreno; off by default
enum { LRZ_READ = 2, LRZ_SWITCH = 4, LRZ_CUT = 8, LRZ_ALL = 14, LRZ_CUT_RD = 16, LRZ_CORNER = 32 };   /* LRZ_CUT_RD: only read at a cut; LRZ_CORNER: the last texel, not the first (tests) */   // DM_VK_LRZ_TOUCH bits: the read before a load pass, the write-back on a depth switch, the write-back at a cut
static int g_x_nosetc, g_x_nodrvc, g_x_landcut, g_x_nokeep, g_x_nobc, g_x_cutany, g_lrz_touch, g_xfb_on = 1, g_x_nogs, g_spv_alt, g_x_nodispbar; int g_x_noshc;   // g_xfb_on: DM_VK_XFB=0 captures no transform feedback (A/B)
static int g_x_nosetmemo, g_x_setverify;   // DM_VK_X_NOSETMEMO=1 (live, A/B): no per-stage set memo; DM_VK_X_SETVERIFY=1 (live): one memo hit in 64 checked (see stage_set)
static int g_vbound = 1;   // DM_VK_VBOUND (see vbuf_bound)
// DM_VK_PROF_SLOW=1 (live): a submission whose replay took over 40 ms logs where it went: pipelines built on the replay thread, textures (vk_tex: images made and
// packaged uploads, the latter also apart), the copied pools' sync before it, shader modules, and the rest
int g_prof_on; uint64_t g_prof_ns[8]; static uint64_t g_prof_t0, g_prof_pkg0, g_prof_npkg0, g_prof_pipes0;
static int g_async_pipe = 1;   // DM_VK_ASYNC_PIPE (see async_pipe)
static int g_lin_back = 1;   // DM_VK_LIN_BACK (see lin_back)
static int g_samp_snap = 1;   // DM_NVN_SAMP_SNAP=0 (live): samplers as registered now, not as the bind saw them (gfx_vk_tex.c vk_samp_at)
static uint64_t g_x_flat[8]; static int g_x_flat_mode[8], g_x_nflat;   // DM_VK_X_FLAT_VS=<vertex shader key>[:mode][,...] (debugging; pipelines made after it is set): mode bits 1 solid magenta pixel shader, 2 no depth test, 4 no blending (7 when left out)
// Additional diagnostic bits: 8 no alpha coverage, 16 no depth bias, 32 no depth test/write, 64 no stencil. The mode is part of the pipeline key so live changes take effect.
static uint64_t g_x_logfs;   // DM_VK_LOG_FS=<fragment shader key> (live): one line per draw with it, with its samplers as the draw sees them
static int g_rd_touch = 1, g_x_rdkeep, g_x_nskipvs; static uint64_t g_x_skipvs[8];   // DM_VK_RD_TOUCH, DM_VK_X_RDKEEP, DM_VK_X_SKIPVS (see prepare_draw)
static int g_x_nskipfs; static uint64_t g_x_skipfs[16];   // DM_VK_X_SKIPFS: bounded live fragment-shader bisection
static uint64_t g_x_cutfs, g_prev_fs;   // DM_VK_X_CUTFS: isolate a fragment shader between pass dumps
static uint64_t g_list_vs, g_list_fs;
static char g_x_tracesize[40], g_x_traceframe[40]; static int g_x_traceframe_on;
// DM_VK_WATCH=<pool offset, hex>[#n] (live): log the next n (20) uses of images at that offset of their pool (attachment, sampled, copy), with the pass and present
// numbers, to find what fills a texture a pass reads
static long long g_watch_off = -1; static int g_watch_n; extern uint64_t g_tex_frame;
// DM_VK_UBO_LOG=<shader key, hex>[#n] (live): log the next n (40) uniform buffers that shader reads (present, stage, register, address, a hash and the first 16 words)
static uint64_t g_ubolog_key; static int g_ubolog_n;
static void ubo_log(const vkshader_t *s, int st, int reg, uint64_t a, uint64_t n) {
    if (!g_ubolog_key || s->key != g_ubolog_key || g_ubolog_n <= 0) return;
    g_ubolog_n--; uint8_t buf[4096]; if (n > sizeof buf) n = sizeof buf; void vk_ubd_read(uint64_t a, uint64_t n, uint8_t *dst); vk_ubd_read(a, n, buf);
    uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < n; i++) h = (h ^ buf[i]) * 1099511628211ull; float f[16] = {0}; memcpy(f, buf, n < 64 ? n : 64);
    // every word of 512 bytes, as floats: from byte 0, or from DM_VK_UBO_LOG_FROM=<byte offset> (live, a multiple of 16) for fields further in
    const char *fe = getenv("DM_VK_UBO_LOG_FROM"); uint32_t from = fe ? (uint32_t)atoi(fe) & ~15u : 0; if (from >= n) from = 0;
    char t[4096]; int k = from ? snprintf(t, sizeof t, "(from +%u) ", from) : 0; uint32_t nw = (uint32_t)(n - from < 512 ? n - from : 512) / 4; (void)f;
    for (uint32_t i = 0; i < nw && k < (int)sizeof t - 24; i++) { float v; memcpy(&v, buf + from + 4 * i, 4); k += snprintf(t + k, sizeof t - k, "%s%g", i ? (i % 4 ? " " : " | ") : "", v); }
    if (st == 0 && reg == 0 && S.vbuf[0].address) {   // the vertex stream 0 the draw reads, hashed the same way (CPU view at replay)
        uint64_t vn = S.vbuf[0].size > sizeof buf ? sizeof buf : S.vbuf[0].size, vh = 1469598103934665603ull; memcpy(buf, (const void *)(uintptr_t)S.vbuf[0].address, vn);
        for (uint64_t i = 0; i < vn; i++) vh = (vh ^ buf[i]) * 1099511628211ull;
        float v[6] = {0}; memcpy(v, buf, vn < 24 ? vn : 24);
        if (S.rt_has_dep) VKLOG("ubo: present %llu pass %d depth target pool %#llx+%#llx serial %llu; depth test %d func %d", (unsigned long long)g_tex_frame, g_sub_pass - 1,
                                (unsigned long long)(uintptr_t)S.rt_dep.pool, (unsigned long long)S.rt_dep.off, (unsigned long long)S.rt_dep.serial, S.ds.dt, S.ds.df);
        VKLOG("ubo: present %llu vertex stream 0 %#llx +%llu hash %016llx: %g %g %g %g %g %g", (unsigned long long)g_tex_frame, (unsigned long long)S.vbuf[0].address, (unsigned long long)S.vbuf[0].size,
              (unsigned long long)vh, v[0], v[1], v[2], v[3], v[4], v[5]);
        void vk_ubd_read(uint64_t a, uint64_t n, uint8_t *dst); vk_ubd_read(a, n, buf); }
    VKLOG("ubo: present %llu pass %d stage %d c%d %#llx +%llu hash %016llx: %s", (unsigned long long)g_tex_frame, g_sub_pass - 1, st, reg, (unsigned long long)a, (unsigned long long)n, (unsigned long long)h, t);
}
// DM_VK_TEXDUMP=<pool hex>+<offset hex> (read when a pass dump starts; the trace prints both): during the dump, the first sampled image there is blitted (level 0, up to 6 layers)
// into RGBA16F images and dumped with the passes, so a compressed texture (BC6H cube maps...) can be looked at decoded by the GPU
static long long g_td_off = -1; static unsigned long long g_td_pool; static vkimg_t *g_td_im; static int g_td_done;
void vk_texdump_note(vkimg_t *im) { if (g_td_off >= 0 && !g_td_done && !g_td_im && im && im->off == g_td_off && im->pool == g_td_pool) g_td_im = im; }
// the pass list (DM_VK_TRACE_PASS=-2, DM_VK_LIST_AT): the images each pass or dispatch reads, by pool offset, printed at its end
static uint64_t g_lst_rd[48]; static int g_lst_nrd;
// the images each stage's set was last collected with (prepare_draw: a memoized set keeps its last collection's), for the order barrier check below
typedef struct { uint64_t pool; int64_t off, size; } stimg_t;
static stimg_t g_stimg[3][32]; static int g_stn[3], g_coll_st = -1;
// Order-only barriers inside a pass (profile key order_keep; DM_VK_ORDER_KEEP=0/1, live, overrides it). Every barrier ends the pass (OP_BARRIER), and on a tiled GPU each
// pass begun again on the same attachments stores and loads all of them: Monster Hunter Rise orders its fragments (NVN barrier 0x2, sometimes 0x1) between almost every
// draw of its decal, transparent and effect passes on the 960x540 scene (B10G11R11 + R8 + D24S8), 98 of its 182 passes a frame begun again that way. With the key a barrier
// of only ORDER_PRIMITIVES / ORDER_FRAGMENTS / ORDER_FRAGMENTS_TILED leaves the pass open and is decided at the next draw (prepare_draw): the pass ends there, with the
// barrier, when that draw samples one of the pass's attachments, or a shader of it or of an earlier draw of the pass writes memory (shader_wmem). Attachment writes
// themselves keep their order inside a pass (primitive order). Off by default: Xenoblade Chronicles 3's characters tore with such barriers kept
static int g_order_keep, g_order_pending, g_order_check, g_pass_wmem; uint64_t g_order_kept, g_order_ended;   // (kept / ended: gfx_vk_present.c reports them)
static void list_reads(const char *what) {
    if (!g_lst_nrd) return;
    char t[1024]; int k = 0; for (int i = 0; i < g_lst_nrd && k < (int)sizeof t - 24; i++) k += snprintf(t + k, sizeof t - k, " %llx", (unsigned long long)g_lst_rd[i]);
    VKLOG("    %s reads%s", what, t); g_lst_nrd = 0;
}
void vk_watch(const vkimg_t *im, const char *what) {
    if (g_x_listpasses && im && what[0] == 's') { uint64_t o = (uint64_t)im->off; int i = 0; while (i < g_lst_nrd && g_lst_rd[i] != o) i++; if (i == g_lst_nrd && i < 48) g_lst_rd[g_lst_nrd++] = o; }
    if (g_coll_st >= 0 && im && what[0] == 's') { if (g_stn[g_coll_st] < 32) g_stimg[g_coll_st][g_stn[g_coll_st]++] = (stimg_t){im->pool, im->off, im->size}; else g_stn[g_coll_st] = 33; }   // 33: too many to keep
    if (g_watch_off < 0 || !im || im->off != g_watch_off || g_watch_n <= 0) return;
    g_watch_n--; VKLOG("watch: present %llu pass %d (%ux%u): %s %ux%u fmt %d pool %#llx+%#llx (image %p, nvn flags %#x, %d overlapping)", (unsigned long long)g_tex_frame, g_sub_pass - 1, S.pass_w, S.pass_h, what, im->w, im->h, im->fmt,
                       (unsigned long long)im->pool, (unsigned long long)im->off, (const void *)im, im->flags, im->novl);
}
static void cpu_ready_config(void) {
    const char *e = getenv("DM_VK_CPU_READY");
    g_vk_cpu_ready = !(e && !atoi(e));   // live A/B: =0 leaves CPU reads unsynchronized
}
static int g_dbg_draw_live; static uint64_t g_skip_ops_live;   // DM_VK_DBG_DRAW, DM_VK_SKIP_OPS: read with the other live switches, once a submission (bisection at a chosen moment)
static void x_read(void) { cpu_ready_config(); { const char *e = getenv("DM_VK_DBG_DRAW"); g_dbg_draw_live = e ? atoi(e) : 0; } { uint64_t s = 0; const char *e = getenv("DM_VK_SKIP_OPS"); for (const char *q = e; q && *q;) { char *n; long v = strtol(q, &n, 10); if (n == q) break; if (v > 0 && v < 64) s |= 1ull << v; q = *n ? n + 1 : n; } g_skip_ops_live = s; } const char *c = getenv("DM_VK_CB_DRAWS"); g_x_cbdraws = c ? atoi(c) : 128; g_sub_pass = 0;
    const char *fy = getenv("DM_VK_FLIPY"); g_flipy = !(fy && !atoi(fy));   // on unless DM_VK_FLIPY=0
    { const char *e = getenv("DM_VK_C2_FAST"); g_c2fast = !(e && !atoi(e)); }   // c2 segments found by their slots' hash (drv_shared); on unless DM_VK_C2_FAST=0
    if (g_x_listpasses) { g_x_listpasses = 0; setenv("DM_VK_TRACE_PASS", "-1", 1); }
    const char *tp = getenv("DM_VK_TRACE_PASS"); g_x_tracepass = tp ? atoi(tp) : -1;
    { const char *e = getenv("DM_VK_ORDER_KEEP"); g_order_keep = e ? atoi(e) != 0 : DM_GAME_ORDER_KEEP; g_order_pending = g_order_check = 0; }
    { static uint64_t until; static int done; const char *la = getenv("DM_VK_LIST_AT");   // DM_VK_LIST_AT=<present>[#n]: the pass list of every submission over n (2) presents from there; now[#n] from the current one
      if (la && !done && (!strncmp(la, "now", 3) || g_tex_frame >= (uint64_t)atoll(la))) { const char *h = strchr(la, '#'); done = 1; until = g_tex_frame + (h && atoi(h + 1) > 0 ? (uint64_t)atoi(h + 1) : 2); }
      if (until && g_tex_frame < until) { g_x_tracepass = -2; g_x_listpasses = 1; g_lst_nrd = 0; VKLOG("--- submission at present %llu", (unsigned long long)g_tex_frame); } }
    { const char *w = getenv("DM_VK_WATCH"); static char last[40]; if (!w || !*w || !strcmp(w, "0")) g_watch_off = -1;
      else if (strcmp(w, last)) { g_watch_off = strtoll(w, NULL, 16); const char *h = strchr(w, '#'); g_watch_n = h ? atoi(h + 1) : 20; }
      snprintf(last, sizeof last, "%s", w ? w : ""); }
    { const char *w = getenv("DM_VK_UBO_LOG"); static char last[40]; if (!w || !*w || !strcmp(w, "0")) g_ubolog_key = 0;
      else if (strcmp(w, last)) { g_ubolog_key = strtoull(w, NULL, 16); const char *h = strchr(w, '#'); g_ubolog_n = h ? atoi(h + 1) : 40; }
      snprintf(last, sizeof last, "%s", w ? w : ""); }
    if (g_x_traceframe_on) { g_x_traceframe_on = 0; setenv("DM_VK_TRACE_FRAME", "", 1); }   // one submission traced
    { const char *tf = getenv("DM_VK_TRACE_FRAME"); snprintf(g_x_traceframe, sizeof g_x_traceframe, "%s", tf && strcmp(tf, "0") ? tf : ""); }
    { const char *tz = getenv("DM_VK_TRACE_SIZE"); snprintf(g_x_tracesize, sizeof g_x_tracesize, "%s", tz ? tz : ""); } const char *e; g_x_nobar = (e = getenv("DM_VK_X_NOBAR")) && atoi(e); g_x_depth = (e = getenv("DM_VK_X_DEPTH")) && atoi(e); g_x_color = (e = getenv("DM_VK_X_COLOR")) && atoi(e);
    g_x_nosetc = (e = getenv("DM_VK_X_NOSETC")) && atoi(e); g_x_nodrvc = (e = getenv("DM_VK_X_NODRVC")) && atoi(e); g_x_landcut = (e = getenv("DM_VK_X_LANDCUT")) && atoi(e);
    g_x_nodispbar = (e = getenv("DM_VK_X_NODISPBAR")) && atoi(e);
    g_x_depth_clear_color = (e = getenv("DM_VK_X_DEPTH_CLEAR_COLOR")) && atoi(e);
    g_x_nokeep = (e = getenv("DM_VK_X_NOKEEP")) && atoi(e); g_x_nobc = (e = getenv("DM_VK_X_NOBC")) && atoi(e); g_x_noshc = (e = getenv("DM_VK_X_NOSHC")) && atoi(e); g_x_cutany = (e = getenv("DM_VK_X_CUTANY")) && atoi(e); g_x_nogs = (e = getenv("DM_VK_X_NOGS")) && atoi(e); g_spv_alt = (e = getenv("DM_VK_SPV_ALT")) && atoi(e); g_rd_touch = !((e = getenv("DM_VK_RD_TOUCH")) && !atoi(e));
    g_lrz_touch = (e = getenv("DM_VK_LRZ_TOUCH")) ? atoi(e) : vk_quirk(VQ_LRZ_TOUCH); if (g_lrz_touch == 1) g_lrz_touch = LRZ_ALL;   /* 0 none, 1 all, else LRZ_* bits */ g_fdz = !((e = getenv("DM_VK_FDZ")) && !atoi(e)); g_xfb_on = !((e = getenv("DM_VK_XFB")) && !atoi(e));
    g_psplit = (e = getenv("DM_VK_PASS_SPLIT")) ? atoi(e) : 0; g_setsplit = (e = getenv("DM_VK_SETSPLIT")) && atoi(e); g_x_inflkeep = (e = getenv("DM_VK_UBD_KEEP")) && atoi(e); g_ubd_heap = !((e = getenv("DM_VK_UBD_HEAP")) && !atoi(e)); g_pool_slots = !((e = getenv("DM_VK_POOL_SLOTS")) && !atoi(e)); g_psplit_depth = (e = getenv("DM_VK_PASS_SPLIT_DEPTH")) && atoi(e);
    g_x_rdkeep = (e = getenv("DM_VK_X_RDKEEP")) && atoi(e); g_vbound = !((e = getenv("DM_VK_VBOUND")) && !atoi(e)); g_x_logfs = (e = getenv("DM_VK_LOG_FS")) ? strtoull(e, NULL, 16) : 0; g_x_nflat = 0; if ((e = getenv("DM_VK_X_FLAT_VS"))) for (char *q = (char *)e; *q && g_x_nflat < 8; ) { char *nx; uint64_t v = strtoull(q, &nx, 16); if (nx == q) break; int m = 7; if (*nx == ':') m = (int)strtol(nx + 1, &nx, 10); g_x_flat_mode[g_x_nflat] = m; g_x_flat[g_x_nflat++] = v; q = *nx ? nx + 1 : nx; } g_samp_snap = !((e = getenv("DM_NVN_SAMP_SNAP")) && !atoi(e)); g_lin_back = !((e = getenv("DM_VK_LIN_BACK")) && !atoi(e)); g_async_pipe = !((e = getenv("DM_VK_ASYNC_PIPE")) && !atoi(e)); g_prof_on = (e = getenv("DM_VK_PROF_SLOW")) && atoi(e); { extern volatile int g_vk_samp_log, g_vk_samp_log_boot; static int first = 1;   // the first read sees the start-up value, which mtl_reg_samp (game threads, no setenv there) may have taken already: not armed twice
      if ((e = getenv("DM_VK_SAMP_LOG")) && atoi(e) > 0) { if (!(first && atoi(e) == g_vk_samp_log_boot)) g_vk_samp_log = atoi(e); setenv("DM_VK_SAMP_LOG", "0", 1); } first = 0; } g_x_nosetmemo = (e = getenv("DM_VK_X_NOSETMEMO")) && atoi(e); g_x_setverify = (e = getenv("DM_VK_X_SETVERIFY")) && atoi(e); g_gs_split = (e = getenv("DM_VK_GS_SPLIT")) && atoi(e);
    g_x_nskipvs = 0; for (const char *q = getenv("DM_VK_X_SKIPVS"); q && *q && g_x_nskipvs < 8;) { char *n; uint64_t v = strtoull(q, &n, 16); if (n == q) break; if (v) g_x_skipvs[g_x_nskipvs++] = v; q = *n ? n + 1 : n;  }
    g_x_nskipfs = 0; for (const char *q = getenv("DM_VK_X_SKIPFS"); q && *q && g_x_nskipfs < 16;) { char *n; uint64_t v = strtoull(q, &n, 16); if (n == q) break; if (v) g_x_skipfs[g_x_nskipfs++] = v; q = *n ? n + 1 : n; }
    g_x_cutfs = (e = getenv("DM_VK_X_CUTFS")) ? strtoull(e, NULL, 16) : 0; }
