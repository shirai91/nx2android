// Android port, Vulkan backend: the command replay (mtl_exec, see gfx_vk.h and nvn_obj.h's record format).
// State records update a shadow of the NVN state; a draw or dispatch turns it into a pipeline (cached by everything that shapes it), descriptor sets (one per
// stage, filled from the shader's reflection: uniform slots, the shader's c1, the driver's c0 / c2 segments, textures and samplers by handle, images, storage
// buffers) and the draw. Render passes open lazily at the first draw or clear after SetRenderTargets and close before anything that is not drawing.
// Version 1 orders everything with whole-pipeline barriers around passes, copies and dispatches.
#include "gfx_vk.h"
#include "nvn_hmap.h"
#include <math.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

VkPipelineLayout vk_pipeline_layout(VkDescriptorSetLayout sets[5], int nsets);
void vk_shader_c1(vkshader_t *s, VkBuffer *buf, VkDeviceSize *off, VkDeviceSize *len);
VkBufferView vk_tbuf_view(const tex_t *t);
void vk_sub_update(uint64_t dst, const void *data, uint64_t n);
int prog_key(const prog_t *p, unsigned st, uint8_t key[8]);

// ---------------- NVN state shadow ----------------
static struct {
    const prog_t *prog; vkshader_t *sh[6]; int prog_ok;
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][256], samp[6][128], img[6][32];
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
static void prewarm_start(void);
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
uint64_t vk_replay_ns(void) { return g_vkst.ns_exec; }   // the replay time so far (gfx_vk_present.c: a frame's cost for the adaptive rate)
// flicker experiments (DM_CTL env lines, read every submission): DM_VK_X_NOSETC no descriptor set reuse, DM_VK_X_NODRVC no driver segment reuse, DM_VK_X_LANDCUT deferred
// uniform updates land at each cut, DM_VK_X_NOKEEP no pass kept for the same targets, DM_VK_X_NOSHC no shadow reuse
static int g_fdz = 1;  // DM_VK_FDZ (live): fragment-depth variants for draws on depth written by fragment shaders (pipeline key fdz)
static int g_x_nosetc, g_x_nodrvc, g_x_landcut, g_x_nokeep, g_x_nobc, g_x_cutany, g_lrz_touch, g_xfb_on = 1, g_x_nogs, g_spv_alt; int g_x_noshc;   // g_xfb_on: DM_VK_XFB=0 captures no transform feedback (A/B)
static int g_x_nosetmemo, g_x_setverify;   // DM_VK_X_NOSETMEMO=1 (live, A/B): no per-stage set memo; DM_VK_X_SETVERIFY=1 (live): one memo hit in 64 checked (see stage_set)
static int g_rd_touch = 1, g_x_rdkeep, g_x_nskipvs; static uint64_t g_x_skipvs[8];   // DM_VK_RD_TOUCH, DM_VK_X_RDKEEP, DM_VK_X_SKIPVS (see prepare_draw)
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
    char t[4096]; int k = 0; uint32_t nw = (uint32_t)(n < 512 ? n : 512) / 4; (void)f;   // every word up to 512 bytes, as floats
    for (uint32_t i = 0; i < nw && k < (int)sizeof t - 24; i++) { float v; memcpy(&v, buf + 4 * i, 4); k += snprintf(t + k, sizeof t - k, "%s%g", i ? (i % 4 ? " " : " | ") : "", v); }
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
void vk_watch(const vkimg_t *im, const char *what) {
    if (g_watch_off < 0 || !im || im->off != g_watch_off || g_watch_n <= 0) return;
    g_watch_n--; VKLOG("watch: present %llu pass %d (%ux%u): %s %ux%u fmt %d pool %#llx+%#llx (image %p, nvn flags %#x, %d overlapping)", (unsigned long long)g_tex_frame, g_sub_pass - 1, S.pass_w, S.pass_h, what, im->w, im->h, im->fmt,
                       (unsigned long long)im->pool, (unsigned long long)im->off, (const void *)im, im->flags, im->novl);
}
static void x_read(void) { const char *c = getenv("DM_VK_CB_DRAWS"); g_x_cbdraws = c ? atoi(c) : 128; g_sub_pass = 0;
    const char *fy = getenv("DM_VK_FLIPY"); g_flipy = !(fy && !atoi(fy));   // on unless DM_VK_FLIPY=0
    if (g_x_listpasses) { g_x_listpasses = 0; setenv("DM_VK_TRACE_PASS", "-1", 1); }
    const char *tp = getenv("DM_VK_TRACE_PASS"); g_x_tracepass = tp ? atoi(tp) : -1;
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
    g_x_nokeep = (e = getenv("DM_VK_X_NOKEEP")) && atoi(e); g_x_nobc = (e = getenv("DM_VK_X_NOBC")) && atoi(e); g_x_noshc = (e = getenv("DM_VK_X_NOSHC")) && atoi(e); g_x_cutany = (e = getenv("DM_VK_X_CUTANY")) && atoi(e); g_x_nogs = (e = getenv("DM_VK_X_NOGS")) && atoi(e); g_spv_alt = (e = getenv("DM_VK_SPV_ALT")) && atoi(e); g_rd_touch = !((e = getenv("DM_VK_RD_TOUCH")) && !atoi(e));
    g_lrz_touch = !((e = getenv("DM_VK_LRZ_TOUCH")) && !atoi(e)); g_fdz = !((e = getenv("DM_VK_FDZ")) && !atoi(e)); g_xfb_on = !((e = getenv("DM_VK_XFB")) && !atoi(e));
    g_psplit = (e = getenv("DM_VK_PASS_SPLIT")) ? atoi(e) : 0; g_setsplit = (e = getenv("DM_VK_SETSPLIT")) && atoi(e); g_psplit_depth = (e = getenv("DM_VK_PASS_SPLIT_DEPTH")) && atoi(e);
    g_x_rdkeep = (e = getenv("DM_VK_X_RDKEEP")) && atoi(e); g_x_nosetmemo = (e = getenv("DM_VK_X_NOSETMEMO")) && atoi(e); g_x_setverify = (e = getenv("DM_VK_X_SETVERIFY")) && atoi(e);
    g_x_nskipvs = 0; for (const char *q = getenv("DM_VK_X_SKIPVS"); q && *q && g_x_nskipvs < 8;) { char *n; uint64_t v = strtoull(q, &n, 16); if (n == q) break; if (v) g_x_skipvs[g_x_nskipvs++] = v; q = *n ? n + 1 : n; } }
// ---------------- conditional rendering (profile render_cond) ----------------
// As nvn_mtl.m (OP_RENDER_COND): SetRenderEnableConditional draws while the 64-bit counters of the reports at addr and addr + 16 are equal / differ, until SetRenderEnable(true).
// Evaluated on the CPU at replay from the records in memory: a report replayed earlier in this submission is not written yet (that happens when the GPU completes), so a timestamp
// one counts with its known counter 0 (vk_sub_complete writes 0 there under the key) and any other one is unknown, which keeps drawing. Inside a block evaluated false, draws and
// clears are dropped. Mario vs. Donkey Kong (gsys, as Tears of the Kingdom and Animal Crossing) clears its HDR scene target under such a condition after lighting it: drawn
// anyway, its levels were black under the HUD
static int g_rc_skip;   // a render-enable block evaluated false is active
#define RC_PEND 256
static struct { uint64_t addr; int kind; } g_rc_pend[RC_PEND]; static int g_rc_npend;   // reports replayed in this submission: kind 1 = timestamp (counter 0), 2 = another counter (unknown)
static uint64_t g_rc_occ[4096];   // addresses ever written by a non-timestamp report (open addressing): conditions on them keep drawing
static void rc_occ_add(uint64_t a) { for (uint32_t i = (uint32_t)(a >> 4) & 4095, k = 0; k < 4096; k++, i = (i + 1) & 4095) { if (g_rc_occ[i] == a) return; if (!g_rc_occ[i]) { g_rc_occ[i] = a; return; } } }
static int rc_occ_has(uint64_t a) { for (uint32_t i = (uint32_t)(a >> 4) & 4095, k = 0; k < 4096; k++, i = (i + 1) & 4095) { if (g_rc_occ[i] == a) return 1; if (!g_rc_occ[i]) return 0; } return 0; }
static void rc_note(uint64_t a, int kind) {
    if (kind == 2) rc_occ_add(a);
    for (int i = 0; i < g_rc_npend; i++) if (g_rc_pend[i].addr == a) { g_rc_pend[i].kind = kind; return; }
    if (g_rc_npend < RC_PEND) { g_rc_pend[g_rc_npend].addr = a; g_rc_pend[g_rc_npend++].kind = kind; }
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
}

void vk_exec_reset(void) {   // a new submission: NVN state does not carry over between command buffers, the replay starts from the defaults
    static int inited; if (!inited) { inited = 1; state_reset(); prewarm_start(); }
    g_rc_npend = 0; g_rc_skip = 0;
    S.pass_open = 0; S.bound_pipe = VK_NULL_HANDLE; g_cb_draws = 0; g_ord_parts = 0; g_psplit_now = 0; g_set_epoch++; x_read();
}

// ---------------- render passes ----------------
// A pass is begun lazily: begin_pass() only sets it up (pass_open 2), and the first command that must run inside it begins it (pass_commit, pass_open 1).
// Until then a clear of a whole attachment becomes that attachment's load op, so a tiler clears in tile memory instead of loading the
// attachment from memory and clearing it with a draw; a pass that is set up and never used is dropped.
static VkRenderingAttachmentInfo P_ca[8], P_da, P_sa; static VkRenderingInfo P_ri; static int P_cleared;
// Render passes. Adreno's driver spent about 40 µs in each vkCmdBeginRendering (a quarter of the replay thread in Xenoblade Chronicles 3's battles, 128 passes a
// frame); a VkRenderPass is made once per set of formats and load operations, and an imageless framebuffer once per set of attachment descriptions, and passes
// then begin with vkCmdBeginRenderPass (the battle's replay went from about 20 to 17.5 ms a frame). Pipelines are made against a compatible render pass (the same
// formats, every load operation LOAD). DM_VK_RP=0 (start-up): dynamic rendering
typedef struct { uint32_t ncol, cfmt[8], cload[8], dfmt, dload, sload, dstore, samples; } rpkey;
typedef struct { VkRenderPass rp; uint32_t w, h, layers, n; struct { uint32_t usage, flags, w, h, layers, fmt, fmt2; } a[9]; } fbkey;   // fmt2: a format-listed image's second format (an imageless framebuffer names the image's whole list)
static nvn_hmap g_rps = {.klen = sizeof(rpkey)}, g_fbs = {.klen = sizeof(fbkey)}; static pthread_mutex_t g_rp_mu = PTHREAD_MUTEX_INITIALIZER;
// Without dynamic rendering (a Vulkan 1.1 / 1.2 device, V.has_dynrender) passes must be render pass objects, whatever DM_VK_RP says; without imageless
// framebuffers either (a Vulkan 1.1 driver without the extensions; the emulator's Android 13 image) each pass gets a framebuffer over its own views (fb_get_views)
static int rp_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_RP"); on = !V.has_dynrender || (V.has_imageless && !(e && !atoi(e)));
    VKLOG("passes: %s", !on ? "dynamic rendering" : V.has_imageless ? "render pass objects with imageless framebuffers" : "render pass objects with a framebuffer per set of views"); } return on; }
static int fmt_has_depth(uint32_t f) { return f && f != VK_FORMAT_S8_UINT; }
static int fmt_has_stencil(uint32_t f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT; }
static VkRenderPass rp_get(const rpkey *k) {
    pthread_mutex_lock(&g_rp_mu); VkRenderPass rp = nvn_hm_get(&g_rps, k);
    if (!rp) {
        VkAttachmentDescription2 ad[9]; VkAttachmentReference2 cr[8], dr; uint32_t na = 0;
        for (uint32_t i = 0; i < k->ncol; i++) {
            if (!k->cfmt[i]) { cr[i] = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT}; continue; }
            ad[na] = (VkAttachmentDescription2){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)k->cfmt[i], (VkSampleCountFlagBits)k->samples, (VkAttachmentLoadOp)k->cload[i], VK_ATTACHMENT_STORE_OP_STORE,
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
        if (vk_create_render_pass(&ci, &rp)) { VKRED_ONCE(70, "render pass creation failed"); rp = VK_NULL_HANDLE; } else nvn_hm_put(&g_rps, k, rp);
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
        VkFramebufferAttachmentImageInfo ai[9]; VkFormat vf[9][2];
        for (uint32_t i = 0; i < k->n; i++) { vf[i][0] = (VkFormat)k->a[i].fmt; vf[i][1] = (VkFormat)k->a[i].fmt2;
            ai[i] = (VkFramebufferAttachmentImageInfo){VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO, NULL, k->a[i].flags, k->a[i].usage, k->a[i].w, k->a[i].h, k->a[i].layers, k->a[i].fmt2 ? 2u : 1u, vf[i]}; }
        VkFramebufferAttachmentsCreateInfo aci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO, NULL, k->n, ai};
        VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, &aci, VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT, k->rp, k->n, NULL, k->w, k->h, k->layers};
        if (vkCreateFramebuffer(V.dev, &ci, NULL, &fb)) { VKRED_ONCE(71, "vkCreateFramebuffer failed"); fb = VK_NULL_HANDLE; } else nvn_hm_put(&g_fbs, k, fb);
    }
    pthread_mutex_unlock(&g_rp_mu); return fb;
}
// Without imageless framebuffers: one over the pass's own views. A view's framebuffers are destroyed with it (vk_fb_view_gone, once the GPU has passed the
// submission that freed it), so a later view given the same handle never finds them
typedef struct { VkRenderPass rp; uint32_t w, h, layers, n; VkImageView v[9]; } vfbkey;
static nvn_hmap g_vfbs = {.klen = sizeof(vfbkey)};
static VkFramebuffer fb_get_views(const fbkey *k, const VkImageView *views) {
    vfbkey vk; memset(&vk, 0, sizeof vk); vk.rp = k->rp; vk.w = k->w; vk.h = k->h; vk.layers = k->layers; vk.n = k->n; memcpy(vk.v, views, k->n * sizeof *views);
    pthread_mutex_lock(&g_rp_mu); VkFramebuffer fb = nvn_hm_get(&g_vfbs, &vk);
    if (!fb) {
        VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, k->rp, k->n, views, k->w, k->h, k->layers};
        if (vkCreateFramebuffer(V.dev, &ci, NULL, &fb)) { VKRED_ONCE(77, "vkCreateFramebuffer over views failed"); fb = VK_NULL_HANDLE; } else nvn_hm_put(&g_vfbs, &vk, fb);
    }
    pthread_mutex_unlock(&g_rp_mu); return fb;
}
void vk_fb_view_gone(VkImageView v) {
    if (V.has_imageless || !g_vfbs.n) return;
    pthread_mutex_lock(&g_rp_mu);
    for (;;) {   // one at a time: a deletion moves the entries after it
        vfbkey hit; int found = 0;
        for (uint32_t i = 0; i < g_vfbs.cap && !found; i++) if (g_vfbs.vals[i]) { const vfbkey *e = (const vfbkey *)(g_vfbs.keys + (size_t)i * g_vfbs.klen);
            for (uint32_t j = 0; j < e->n && !found; j++) if (e->v[j] == v) { hit = *e; found = 1; } }
        if (!found) break;
        vkDestroyFramebuffer(V.dev, (VkFramebuffer)nvn_hm_del(&g_vfbs, &hit), NULL);
    }
    pthread_mutex_unlock(&g_rp_mu);
}
// the attachments begin_pass found, for the render pass begin: each colour slot's (and the depth's) image, view, view format and layers, size at the view's level
static struct { vkimg_t *im; VkImageView v; uint32_t fmt, layers, w, h; } P_att[9];

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
    if (g_lrz_touch && dim && g_lrz_prev && g_lrz_prev != dim) lrz_touch(g_lrz_prev, 1);
    if (dim) g_lrz_prev = dim;
    if (g_lrz_touch && dim && P_da.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) lrz_touch(dim, 0);
    void vk_pass_ts(int end, uint32_t w, uint32_t h, uint32_t fmt, uint32_t draws); vk_pass_ts(0, 0, 0, 0, 0);
    if (rp_on()) {
        rpkey k; memset(&k, 0, sizeof k); k.ncol = P_ri.colorAttachmentCount; k.samples = S.samples ? S.samples : 1;
        fbkey fk; memset(&fk, 0, sizeof fk); VkImageView views[9]; VkClearValue cv[9]; uint32_t na = 0;
        for (uint32_t i = 0; i < k.ncol; i++) if (P_ca[i].imageView) {
            k.cfmt[i] = P_att[i].fmt; k.cload[i] = P_ca[i].loadOp; cv[na] = P_ca[i].clearValue; views[na] = P_ca[i].imageView;
            fk.a[na].usage = P_att[i].im->usage; fk.a[na].flags = P_att[i].im->cflags; fk.a[na].w = P_att[i].w; fk.a[na].h = P_att[i].h; fk.a[na].layers = P_att[i].layers;
            fk.a[na].fmt = P_att[i].im->fmt2 ? (uint32_t)P_att[i].im->fmt : P_att[i].fmt; fk.a[na].fmt2 = (uint32_t)P_att[i].im->fmt2; na++; }   // (a format-listed image (DM_VK_UBWC): its whole list, as imageless framebuffers require)
        if (P_ri.pDepthAttachment || P_ri.pStencilAttachment) {
            k.dfmt = P_att[8].fmt; k.dload = P_da.loadOp; k.sload = P_sa.imageView ? P_sa.loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE; k.dstore = P_da.storeOp;
            cv[na] = P_da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? P_da.clearValue : P_sa.clearValue; if (P_sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) cv[na].depthStencil.stencil = P_sa.clearValue.depthStencil.stencil;
            views[na] = P_da.imageView;
            fk.a[na].usage = P_att[8].im->usage; fk.a[na].flags = P_att[8].im->cflags; fk.a[na].w = P_att[8].w; fk.a[na].h = P_att[8].h; fk.a[na].layers = P_att[8].layers; fk.a[na].fmt = P_att[8].fmt; na++; }
        VkRenderPass rp = rp_get(&k);
        fk.rp = rp_compat(k.ncol, k.cfmt, k.dfmt, k.samples); fk.w = P_ri.renderArea.extent.width; fk.h = P_ri.renderArea.extent.height; fk.layers = P_ri.layerCount; fk.n = na;
        VkFramebuffer fb = rp && fk.rp ? (V.has_imageless ? fb_get(&fk) : fb_get_views(&fk, views)) : VK_NULL_HANDLE;
        if (fb) { VkRenderPassAttachmentBeginInfo abi = {VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO, NULL, na, views};
            VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, V.has_imageless ? &abi : NULL, rp, fb, P_ri.renderArea, na, cv};
            vkCmdBeginRenderPass(vk_cmd(), &rbi, VK_SUBPASS_CONTENTS_INLINE); S.rp_pass = 1; }
        else if (V.has_dynrender) { vkCmdBeginRendering(vk_cmd(), &P_ri); S.rp_pass = 0; }
        else { VKRED_ONCE(76, "render pass or framebuffer failed without dynamic rendering: pass skipped"); return 0; }   // (still pass_open 2: vk_end_pass drops it)
    } else { vkCmdBeginRendering(vk_cmd(), &P_ri); S.rp_pass = 0; }
    S.pass_open = 1; g_vkst.passes++; g_sub_pass++;
    { extern uint64_t g_cmd_gen; g_cmd_gen++; }   // the bind cache starts empty in each pass: Adreno does not keep the state a pass's draws set for the next pass (XC3 drew its lighting black)
    if (g_x_tracepass == -2) { g_x_listpasses = 1; VKLOG("--- pass %d: %ux%u, colour formats %d %d, depth format %d", g_sub_pass - 1, S.pass_w, S.pass_h, S.cfmt[0], S.cfmt[1], S.dfmt); }
    if (g_x_traceframe[0]) {   // DM_VK_TRACE_FRAME=WxH[:fmt[:fmt]] (live): trace every pass of the next submission whose targets match (a whole effect chain in one log)
        char b[48]; snprintf(b, sizeof b, "%ux%u:%d:%d", S.pass_w, S.pass_h, S.cfmt[0], S.cfmt[1]);
        g_x_tracepass = !strncmp(b, g_x_traceframe, strlen(g_x_traceframe)) ? g_sub_pass - 1 : -1; g_x_traceframe_on = 1; }
    if (g_x_tracesize[0] && g_x_tracepass < 0) {   // DM_VK_TRACE_SIZE=WxH[:format[:format]] (live): trace the next pass whose first colour target is that (passes are numbered anew each frame)
        char b[48]; snprintf(b, sizeof b, "%ux%u:%d:%d", S.pass_w, S.pass_h, S.cfmt[0], S.cfmt[1]);
        const char *h = strchr(g_x_tracesize, '#'); size_t l = h ? (size_t)(h - g_x_tracesize) : strlen(g_x_tracesize);   // #n: the (n+1)th such pass
        if (!strncmp(b, g_x_tracesize, l)) { if (h && atoi(h + 1) > 0) { snprintf(g_x_tracesize + l, sizeof g_x_tracesize - l, "#%d", atoi(h + 1) - 1); setenv("DM_VK_TRACE_SIZE", g_x_tracesize, 1); }
            else { g_x_tracepass = g_sub_pass - 1; g_x_tracesize[0] = 0; setenv("DM_VK_TRACE_SIZE", "", 1); } } }
    if (g_x_tracepass >= 0 && g_sub_pass - 1 == g_x_tracepass) VKLOG("=== traced pass %d: %ux%u, colour formats %d %d, depth format %d", g_x_tracepass, S.pass_w, S.pass_h, S.cfmt[0], S.cfmt[1], S.dfmt); g_pass_draws = 0; g_pass_bb[0] = g_pass_bb[1] = INT32_MAX; g_pass_bb[2] = g_pass_bb[3] = INT32_MIN;
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
    if (g_pd_state == 1 && g_sub_pass - 1 < g_pd_skip) return;   // (a submission with fewer passes dumps nothing, and the next one is tried)
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
                                                    im->fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && im->fmt != VK_FORMAT_R16_SFLOAT && im->fmt != VK_FORMAT_R8_UNORM && im->fmt != VK_FORMAT_A2B10G10R10_UNORM_PACK32)) return;
    const view_t *cv = S.rt_has_cview[ci] ? &S.rt_cview[ci] : NULL;   // the view's level and layer (a cube face of an array, a mip)
    uint32_t lv = cv && cv->set_levels ? cv->base_level : 0, ly = cv && cv->set_layers ? cv->min_layer : 0, w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1;
    if (lv >= im->levels || ly >= im->layers) lv = ly = 0, w = im->w, h = im->h;
    uint32_t bpp = im->fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : im->fmt == VK_FORMAT_R16_SFLOAT ? 2 : im->fmt == VK_FORMAT_R8_UNORM ? 1 : 4; VkDeviceSize n = (VkDeviceSize)w * h * bpp;
    pdump_t *d = &g_pd[g_npd++]; d->w = w; d->h = h; d->f = im->fmt; d->pass = g_sub_pass - 1; d->present = g_tex_frame;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &d->b)); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, d->b, &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0)};
    VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &d->m)); VKCHK(vkBindBufferMemory(V.dev, d->b, d->m, 0)); VKCHK(vkMapMemory(V.dev, d->m, 0, VK_WHOLE_SIZE, 0, &d->p));
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1}, {0, 0, 0}, {w, h, 1}};
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
void vk_pass_dump_poll(uint64_t done_fence) {   // gfx_vk_present.c, every present: start on the switch, write once the GPU finished
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
        pdump_t *d = &g_pd[i]; char path[1024]; snprintf(path, sizeof path, "%s/pass_%02d.ppm", root ? root : ".", i);
        VKLOG("pass dump %02d is the submission's pass %d (present %llu)", i, d->pass, (unsigned long long)d->present);
        FILE *f = fopen(path, "wb"); if (!f) continue; fprintf(f, "P6\n%u %u\n255\n", d->w, d->h);
        uint8_t *row = malloc((size_t)d->w * 3); uint64_t bad = 0; float mx = 0, mn = 0;
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
        for (uint32_t y = 0; y < d->h; y++) { for (uint32_t x = 0; x < d->w; x++) {
            if (d->f == VK_FORMAT_R16G16B16A16_SFLOAT || d->f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || d->f == VK_FORMAT_R16_SFLOAT) { float v3[3];
                if (d->f == VK_FORMAT_R16G16B16A16_SFLOAT) { const uint16_t *p = (const uint16_t *)d->p + ((size_t)y * d->w + x) * 4; for (int c = 0; c < 3; c++) v3[c] = half2f(p[c]); }
                else if (d->f == VK_FORMAT_R16_SFLOAT) v3[0] = v3[1] = v3[2] = half2f(((const uint16_t *)d->p)[(size_t)y * d->w + x]);
                else { uint32_t w = ((const uint32_t *)d->p)[(size_t)y * d->w + x];   // 11-bit and 10-bit floats: a half's exponent and top mantissa bits
                    v3[0] = half2f((uint16_t)((w & 0x7ff) << 4)); v3[1] = half2f((uint16_t)((w >> 11 & 0x7ff) << 4)); v3[2] = half2f((uint16_t)((w >> 22 & 0x3ff) << 5)); }
                for (int c = 0; c < 3; c++) { float v = v3[c]; if (v != v || v > 65000.f || v < -65000.f) { bad++; v = 1; } if (v > mx) mx = v; if (v < mn) mn = v; row[3 * x + c] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255 + .5f); } }
            else if (d->f == VK_FORMAT_R8_UNORM) row[3 * x] = row[3 * x + 1] = row[3 * x + 2] = ((const uint8_t *)d->p)[(size_t)y * d->w + x];
            else if (d->f == VK_FORMAT_A2B10G10R10_UNORM_PACK32) { uint32_t w = ((const uint32_t *)d->p)[(size_t)y * d->w + x]; for (int c = 0; c < 3; c++) row[3 * x + c] = (uint8_t)((w >> (10 * c) & 0x3ff) >> 2); }
            else { const uint8_t *p = (const uint8_t *)d->p + ((size_t)y * d->w + x) * 4; int bgr = d->f == VK_FORMAT_B8G8R8A8_UNORM; row[3 * x] = p[bgr ? 2 : 0]; row[3 * x + 1] = p[1]; row[3 * x + 2] = p[bgr ? 0 : 2]; } }
            fwrite(row, 1, (size_t)d->w * 3, f); }
        free(row); fclose(f);
        VKLOG("pass dump %02d: %ux%u format %d → %s%s", i, d->w, d->h, d->f, path, d->f == VK_FORMAT_R16G16B16A16_SFLOAT ? "" : "");
        if (d->f == VK_FORMAT_R16G16B16A16_SFLOAT || d->f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || d->f == VK_FORMAT_R16_SFLOAT) VKLOG("   NaN/inf channels %llu, finite values %.3f .. %.3f", (unsigned long long)bad, mn, mx);
        vkDestroyBuffer(V.dev, d->b, NULL); vkFreeMemory(V.dev, d->m, NULL);
    }
    g_npd = 0; g_pd_state = 0;
}
// DM_VK_CRUMBS=1 (start-up), for a device lost (a GPU hang): every pass end and dispatch writes {submission, pass} into host-visible memory after a full barrier, and the host
// keeps the shaders each pass drew with; when the device is lost (vk_crumbs_report), the last work the GPU finished and the passes after it are logged
static int g_crumbs = -1; static VkBuffer g_crumb_buf; static volatile uint32_t *g_crumb;
typedef struct { uint32_t sub; int pass, n; uint64_t k[8][3]; } crumb_t; static crumb_t g_cring[512]; static unsigned g_cn;
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
    for (int i = 0; i < c->n; i++) if (c->k[i][0] == a && c->k[i][1] == b && c->k[i][2] == c3) return; if (c->n < 8) { c->k[c->n][0] = a; c->k[c->n][1] = b; c->k[c->n][2] = c3; c->n++; } }
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
void vk_end_pass(void) {
    if (g_x_listpasses && S.pass_open == 1) { Dl_info di; void *ra = __builtin_return_address(0); uintptr_t base = dladdr((void *)vk_end_pass, &di) ? (uintptr_t)di.dli_fbase : 0; VKLOG("    ended from %#llx (%llu draws)", (unsigned long long)((uintptr_t)ra - base), (unsigned long long)g_pass_draws); }   // (DM_VK_TRACE_PASS=-2: who ends each pass)
    if (S.pass_open == 2 && P_cleared) (void)pass_commit();   // only clears: the pass still runs, for its load ops
    if (S.pass_open == 1) { if (S.rp_pass) vkCmdEndRenderPass(vk_cmd()); else vkCmdEndRendering(vk_cmd()); void vk_pass_ts(int end, uint32_t w, uint32_t h, uint32_t fmt, uint32_t draws); vk_pass_ts(1, S.pass_w, S.pass_h, (uint32_t)S.cfmt[0], (uint32_t)g_pass_draws | (g_pass_draws ? (uint32_t)((g_pass_bb[2] - g_pass_bb[0]) & 0xffff) << 16 : 0));  pass_dump(); if (g_x_tracepass >= 0 && g_sub_pass - 1 == g_x_tracepass) { setenv("DM_VK_TRACE_PASS", "-1", 1); g_x_tracepass = -1; } }
    if (S.pass_open == 1) crumb_mark(0);
    S.pass_open = 0; S.bound_pipe = VK_NULL_HANDLE; g_tf_in_pass = 0;
}
// a pass end before something that reads or writes pool memory other than through a uniform binding (copies, fills, dispatches, buffer ↔ texture
// copies, the submission's end): the deferred uniform updates land first (draws read them through their shadows, so ordinary pass ends keep them)
void vk_end_pass_land(void) { vk_end_pass(); if (vk_sub_open()) vk_ubd_land(vk_cmd()); }
static int pass_whole(VkRect2D r) { return !r.offset.x && !r.offset.y && r.extent.width >= S.pass_w && r.extent.height >= S.pass_h; }
static int begin_pass(void) {
    if (S.pass_open) return 1;
    int psplit = g_psplit_now; g_psplit_now = 0;   // a pass split (prepare_draw): taken before any early return, so the flag never forces a later pass's cut
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo *ca = P_ca; memset(ca, 0, sizeof P_ca);
    for (int i = 0; i < 9; i++) P_att[i].im = NULL;
    VkRenderingAttachmentInfo da = {0}, sa = {0};
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = 1, samples = 1; int ncol = 0, have = 0;
    for (int i = 0; i < S.rt_n; i++) {
        S.cfmt[i] = VK_FORMAT_UNDEFINED;
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (!S.rt_has_col[i]) continue;
        vkimg_t *im; VkImageView v = vk_tex_view(&S.rt_col[i], S.rt_has_cview[i] ? &S.rt_cview[i] : NULL, 0xff, 0, 1, &im);
        if (!v) continue;
        vk_tex_prepare(cb, im);
        uint32_t lv = S.rt_has_cview[i] && S.rt_cview[i].set_levels ? S.rt_cview[i].base_level : 0;
        uint32_t iw = im->w >> lv ? im->w >> lv : 1, ih = im->h >> lv ? im->h >> lv : 1;
        if (iw < w) w = iw; if (ih < h) h = ih; samples = im->samples; have = 1;
        ca[i].imageView = v; S.cfmt[i] = vk_view_fmt; ncol = i + 1; P_att[i].im = im;   // (the view's format: a render pass's attachment and its pipelines take it)
        P_att[i].fmt = vk_view_fmt; P_att[i].layers = vk_view_layers ? vk_view_layers : 1; P_att[i].w = iw; P_att[i].h = ih;
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
            S.dfmt = im->fmt; S.daspect = im->aspect; P_att[8].im = im; P_att[8].fmt = vk_view_fmt; P_att[8].layers = vk_view_layers ? vk_view_layers : 1; P_att[8].w = iw; P_att[8].h = ih;
            if (im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) sa = da;
        }
    }
    if (!have) { VKRED_ONCE(30, "draw with no usable render target: skipped"); return 0; }
    // The cut (DM_VK_CB_DRAWS): only before a pass whose depth attachment no earlier pass of this submission drew with. Adreno keeps a low-resolution copy of a depth
    // attachment (LRZ) that it trusts within a command buffer; a pass that goes on with a depth buffer begun in an earlier command buffer had whole 8x8 blocks of its
    // nearest objects rejected in some frames (NEO: bushes, trees and the phone booth near the camera flickered light blue). DM_VK_X_CUTANY=1 cuts before any pass.
    // The rule looks only at the pass at the cut: a later pass may still go on with a depth buffer drawn before it. Hades II cuts before its depth-less passes and then
    // goes on with the 1280x720 D24S8 of the passes before the cut: its lights went out for single frames while Melinoe walked near them (one-frame dark spikes in
    // 6-23 of 600 recorded frames; 0 with DM_VK_CB_DRAWS=0, also 2-15 with the parts submitted together, 135 without the LRZ touches). Such a pass first gets a texel of its
    // depth written back (lrz_touch), the transfer write that made Adreno drop a stale LRZ for Xenoblade Chronicles 3 (DM_VK_LRZ_TOUCH=0 leaves it out)
    static uint64_t g_cuts; int cut_touch = 0;
    vkimg_t *dim = NULL; if (da.imageView) vk_tex_view(&S.rt_dep, S.rt_has_dview ? &S.rt_dview : NULL, 0xff, 0, 1, &dim);
    if (psplit || (g_x_cbdraws > 0 && g_cb_draws >= g_x_cbdraws && (!dim || dim->depth_sub != g_set_epoch || g_x_cutany))) {   // (a pass split: prepare_draw checked the depth rule and the part budget)
        void vk_cmd_split(void); if (g_x_landcut) vk_ubd_land(vk_cmd());
        if (psplit) vk_cmd_split(); else if (g_ord_parts < 15) { vk_cmd_split(); g_ord_parts++; }   // ordinary cuts as before MAIN_CBS 32 (refused past 15; g_cuts still counts, as it did)
        g_cb_draws = 0; cb = vk_cmd(); g_cuts++;   // no pass is open here; deferred uniform updates stay deferred (draws read their shadows)
    }
    if (dim) { cut_touch = g_lrz_touch && dim->depth_sub == g_set_epoch && dim->depth_cut != g_cuts; dim->depth_sub = g_set_epoch; dim->depth_cut = g_cuts; }
    { static int n; uint32_t cs[9] = {0}; int any = 0, mis = 0; uint32_t s0 = 0;   // multisampled passes: every attachment's sample count (they must agree)
      for (int i = 0; i < ncol; i++) if (ca[i].imageView) { vkimg_t *im2; vk_tex_view(&S.rt_col[i], S.rt_has_cview[i] ? &S.rt_cview[i] : NULL, 0xff, 0, 1, &im2); cs[i] = im2 ? im2->samples : 0; if (cs[i] > 1) any = 1; if (!s0) s0 = cs[i]; else if (cs[i] != s0) mis = 1; }
      if (da.imageView) { vkimg_t *im2; vk_tex_view(&S.rt_dep, S.rt_has_dview ? &S.rt_dview : NULL, 0xff, 0, 1, &im2); cs[8] = im2 ? im2->samples : 0; if (cs[8] > 1) any = 1; if (s0 && cs[8] != s0) mis = 1; }
      if ((any || mis) && n < 10) { n++; VKLOG("pass %ux%u samples: colour %u %u %u %u, depth %u%s", w, h, cs[0], cs[1], cs[2], cs[3], cs[8], mis ? " MISMATCH" : ""); } }
    if (!g_x_nobar) vk_img_barrier_all(cb);
    if (cut_touch) lrz_touch(dim, 1);   // (after the barrier: the earlier command buffer's depth writes come first)
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
    if (c->index < 0 || c->index >= S.ncol || S.cfmt[c->index] == VK_FORMAT_UNDEFINED) return;
    if (!c->mask) return;
    if ((c->mask & 0xf) != 0xf) { VkRect2D r = scissor_rect(); if (!r.extent.width || !r.extent.height) return;
        if (clear_masked(c, r)) return; VKRED_ONCE(31, "partial channel clear mask %#x cleared in full", c->mask); }
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, {{{c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]}}}};
    const vkfmt_t *f = vk_fmt(S.rt_col[c->index].d.format);
    if (f && f->is_int) for (int i = 0; i < 4; i++) { if (f->is_sint) a.clearValue.color.int32[i] = (int32_t)c->rgba[i]; else a.clearValue.color.uint32[i] = c->rgba[i] > 0 ? (uint32_t)c->rgba[i] : 0; }
    VkClearRect r = {scissor_rect(), 0, 1};
    if (S.pass_open == 2 && (c->mask & 0xf) == 0xf && pass_whole(r.rect)) {   // the whole attachment before any draw: its load op
        P_ca[c->index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; P_ca[c->index].clearValue.color = a.clearValue.color; P_cleared = 1; g_vkst.folded++; return;
    }
    if (!pass_commit()) return;
    if (r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(vk_cmd(), 1, &a, 1, &r);
}
static void clear_ds(const p_clear_ds *c) {
    if (!begin_pass() || !S.daspect) return;
    VkImageAspectFlags as = (c->depth_mask ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (c->stencil_mask && (S.daspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    as &= S.daspect; if (!as) return;
    VkClearAttachment a = {as, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect r = {scissor_rect(), 0, 1};
    if (S.pass_open == 2 && pass_whole(r.rect) && (!(as & VK_IMAGE_ASPECT_STENCIL_BIT) || (c->stencil_mask & 0xff) == 0xff)) {   // into the load ops
        if (as & VK_IMAGE_ASPECT_DEPTH_BIT) { P_da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; P_da.clearValue.depthStencil = a.clearValue.depthStencil; }
        if (as & VK_IMAGE_ASPECT_STENCIL_BIT) { P_sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; P_sa.clearValue.depthStencil = a.clearValue.depthStencil; }
        P_cleared = 1; g_vkst.folded++; return;
    }
    if (!pass_commit()) return;
    if (r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(vk_cmd(), 1, &a, 1, &r);
}

// ---------------- formats and enums ----------------
static VkFormat vertex_format(int f, int want_int, int want_sint) {
    static const uint16_t rgb[] = {VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM,
        VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT};
    static const uint16_t rgb_scaled[] = {VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT};
    VkFormat v;
    if (f >= 0x19 && f <= 0x24) v = (VkFormat)rgb[f - 0x19];
    else if (f >= 0x5D && f <= 0x62) v = (VkFormat)rgb_scaled[f - 0x5D];
    else { const vkfmt_t *e = vk_fmt(f); v = e ? e->vk : VK_FORMAT_UNDEFINED; }
    static uint8_t checked[0x100];   // a format the device cannot read as a vertex attribute gives undefined values: named once
    if (v != VK_FORMAT_UNDEFINED && f > 0 && f < 0x100 && !checked[f]) { checked[f] = 1; VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(V.pd, v, &fp);
        if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) VKLOG("[RED] vertex attribute format %#x (vk %d): the device cannot read it as a vertex attribute", f, v); }
    if (want_int) {   // the shader declared an integer input: the attribute must be an integer format of the same layout
        switch (v) {
        case VK_FORMAT_R32_SFLOAT: return want_sint ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
        case VK_FORMAT_R32G32_SFLOAT: return want_sint ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT;
        case VK_FORMAT_R32G32B32_SFLOAT: return want_sint ? VK_FORMAT_R32G32B32_SINT : VK_FORMAT_R32G32B32_UINT;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return want_sint ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_USCALED: return want_sint ? VK_FORMAT_R8G8B8A8_SINT : VK_FORMAT_R8G8B8A8_UINT;
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_USCALED: return want_sint ? VK_FORMAT_R16G16_SINT : VK_FORMAT_R16G16_UINT;
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_USCALED: return want_sint ? VK_FORMAT_R16G16B16A16_SINT : VK_FORMAT_R16G16B16A16_UINT;
        default: return v;
        }
    }
    return v;
}
static const VkBlendFactor BF[0x65] = {
    [1] = VK_BLEND_FACTOR_ZERO, [2] = VK_BLEND_FACTOR_ONE, [3] = VK_BLEND_FACTOR_SRC_COLOR, [4] = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, [5] = VK_BLEND_FACTOR_SRC_ALPHA,
    [6] = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, [7] = VK_BLEND_FACTOR_DST_ALPHA, [8] = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, [9] = VK_BLEND_FACTOR_DST_COLOR,
    [10] = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, [11] = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, [0x10] = VK_BLEND_FACTOR_SRC1_COLOR, [0x11] = VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
    [0x12] = VK_BLEND_FACTOR_SRC1_ALPHA, [0x13] = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, [0x61] = VK_BLEND_FACTOR_CONSTANT_COLOR, [0x62] = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
    [0x63] = VK_BLEND_FACTOR_CONSTANT_ALPHA, [0x64] = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
static VkBlendFactor bfac(unsigned f) { return f < 0x65 && (f == 1 || BF[f]) ? BF[f] : VK_BLEND_FACTOR_ONE; }
static VkBlendOp bop(unsigned e) { return e >= 1 && e <= 5 ? (VkBlendOp)(e - 1) : VK_BLEND_OP_ADD; }
static VkCompareOp cmpop(unsigned f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static VkStencilOp sop(unsigned o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
static VkPrimitiveTopology topo(int p) {
    switch (p) { case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case 2: case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
                 case 5: case 8: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case 6: case 9: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
                 case 0xA: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; case 0xB: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
                 case 0xC: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; case 0xD: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; case 0xE: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
                 default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; }
}

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
    uint64_t key[3]; uint32_t topo, restart, raster_discard, cull, front, mode, depth_clamp, bias_en;
    uint32_t dt, dw, df, st, sf[2], sfail[2], szfail[2], szpass[2];
    uint32_t nattr; struct { uint32_t loc, binding, fmt, off; } attr[16];
    uint32_t nbind; struct { uint32_t binding, stride, rate, divisor; } bind[17];
    uint32_t ncol, cfmt[8], dfmt, samples, a2c, blend_en, logic_en, logic_op, wmask[8];
    struct { uint32_t src, dst, srca, dsta, eq, eqa; } bl[8];
    uint32_t clip_neg1, alt, fdz, dual;   // fdz: the fragment shader's variant that writes gl_FragDepth = gl_FragCoord.z (fdz below); dual: its dual-source variant (below)
    uint64_t um[3], im[3];   // each stage's sampled bindings given integer views (vk_shader_module)
} pkey;
static nvn_hmap g_pipes = {.klen = sizeof(pkey)}; static pthread_mutex_t g_pipe_mu = PTHREAD_MUTEX_INITIALIZER;   // the replay thread and the prewarm threads
static VkPipeline build_pipeline(const pkey *k, VkPipelineLayout layout, vkshader_t *const stg[3]);
static void pipe_record(const pkey *k);
// The last key and its pipeline (the replay thread's only; pipelines are never dropped): runs of draws with one pipeline skip the 992-byte hash and the lock
static pkey g_last_k; static VkPipeline g_last_p;
static VkPipeline graphics_pipeline(const pkey *k, VkPipelineLayout layout) {
    if (g_last_p && !memcmp(k, &g_last_k, sizeof *k)) return g_last_p == (void *)1 ? VK_NULL_HANDLE : g_last_p;
    pthread_mutex_lock(&g_pipe_mu); VkPipeline p = nvn_hm_get(&g_pipes, k); pthread_mutex_unlock(&g_pipe_mu);
    if (p) { g_last_k = *k; g_last_p = p; return p == (void *)1 ? VK_NULL_HANDLE : p; }
    vkshader_t *stg[3] = {S.sh[0], S.sh[1], S.sh[2]};
    p = build_pipeline(k, layout, stg);
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
#define PIPE_MAGIC 0x31455050u   // "PPE1"; the header also holds sizeof(pkey), so a changed key layout starts a new file
static FILE *g_prec;
static void pipe_record(const pkey *k) {   // under g_pipe_mu
    if (!g_prec || k->alt) return;   // DM_VK_SPV_ALT's pipelines are a debugging run's own
    fwrite(k, sizeof *k, 1, g_prec); fflush(g_prec);
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
static VkPipeline build_pipeline(const pkey *k, VkPipelineLayout layout, vkshader_t *const stg[3]) {
    VkPipeline p;
    VkPipelineShaderStageCreateInfo ss[3]; uint32_t ns = 0;
    VkShaderStageFlagBits fl[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    // the outputs each stage must declare for the next (vk_shader_module): vertex → geometry or fragment, geometry → fragment
    const vkshader_t *fs = stg[1] && !k->raster_discard ? stg[1] : NULL; uint32_t xo[3] = {0};
    if (stg[0]) xo[0] = (stg[2] ? stg[2]->in_locs : fs ? fs->in_locs : 0) & ~stg[0]->out_locs;
    if (stg[2]) xo[2] = (fs ? fs->in_locs : 0) & ~stg[2]->out_locs;
    for (int i = 0; i < 3; i++) if (stg[i] && !(i == 1 && k->raster_discard)) ss[ns++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, fl[i], vk_shader_module(stg[i], k->um[i], k->im[i], k->alt && !k->um[i] && !k->im[i] && !xo[i] ? 1u << 31 : xo[i], i == 1 ? (k->fdz ? 1 : 0) | (k->dual ? 2 : 0) : 0), "main", NULL};
    VkVertexInputAttributeDescription ad[16]; VkVertexInputBindingDescription bd[17]; VkVertexInputBindingDivisorDescriptionEXT dd[17]; uint32_t ndd = 0;
    for (uint32_t i = 0; i < k->nattr; i++) ad[i] = (VkVertexInputAttributeDescription){k->attr[i].loc, k->attr[i].binding, (VkFormat)k->attr[i].fmt, k->attr[i].off};
    for (uint32_t i = 0; i < k->nbind; i++) { bd[i] = (VkVertexInputBindingDescription){k->bind[i].binding, k->bind[i].stride, (VkVertexInputRate)k->bind[i].rate};
        if (k->bind[i].rate && k->bind[i].divisor > 1) dd[ndd++] = (VkVertexInputBindingDivisorDescriptionEXT){k->bind[i].binding, k->bind[i].divisor}; }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvs = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, NULL, ndd, dd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, ndd ? &dvs : NULL, 0, k->nbind, bd, k->nattr, ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, (VkPrimitiveTopology)k->topo, k->restart};
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, NULL, k->clip_neg1};
    VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, k->clip_neg1 && V.has_depth_clip_ctl ? &dcc : NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, k->depth_clamp, k->raster_discard,
        k->mode == 0 ? VK_POLYGON_MODE_POINT : k->mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL, k->cull, k->front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        k->bias_en, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)k->samples, 0, 0, NULL, k->a2c, 0};
    VkStencilOpState sf = {sop(k->sfail[0]), sop(k->szpass[0]), sop(k->szfail[0]), cmpop(k->sf[0]), 0xff, 0xff, 0}, sb = {sop(k->sfail[1]), sop(k->szpass[1]), sop(k->szfail[1]), cmpop(k->sf[1]), 0xff, 0xff, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, k->dt, k->dt && k->dw, cmpop(k->df), 0, k->st, sf, sb, 0, 1};
    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < k->ncol; i++) cba[i] = (VkPipelineColorBlendAttachmentState){(k->blend_en >> i) & 1, bfac(k->bl[i].src), bfac(k->bl[i].dst), bop(k->bl[i].eq),
        bfac(k->bl[i].srca), bfac(k->bl[i].dsta), bop(k->bl[i].eqa), k->wmask[i]};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, k->logic_en, (VkLogicOp)k->logic_op, k->ncol, cba, {0, 0, 0, 0}};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof dyn / sizeof *dyn, dyn};
    VkFormat cf[8]; for (uint32_t i = 0; i < k->ncol; i++) cf[i] = (VkFormat)k->cfmt[i];
    VkFormat df = (VkFormat)k->dfmt;
    int has_stencil = df == VK_FORMAT_D24_UNORM_S8_UINT || df == VK_FORMAT_D32_SFLOAT_S8_UINT || df == VK_FORMAT_S8_UINT;
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, k->ncol, cf, df == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : df, has_stencil ? df : VK_FORMAT_UNDEFINED};
    VkRenderPass prp = rp_on() ? rp_compat(k->ncol, k->cfmt, k->dfmt, k->samples) : VK_NULL_HANDLE;   // with render passes, pipelines are made against a compatible one
    if (!prp && !V.has_dynrender) return (VkPipeline)1;   // no compatible render pass and no dynamic rendering: no valid pipeline (pass_commit skips such passes)
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, prp ? NULL : &rci, 0, ns, ss, &vi, &ia, NULL, &vs, &rs, &ms, &ds, &cb, &dy, layout, prp, 0, VK_NULL_HANDLE, -1};
    VkResult r = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &p);
    static uint64_t n; uint64_t c = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
    if (r) { LOG("[RED] [vk] graphics pipeline failed (%d): shaders %016llx %016llx %016llx", r, (unsigned long long)k->key[0], (unsigned long long)k->key[1], (unsigned long long)k->key[2]); p = (VkPipeline)1; }
    else if (c % 200 == 0) VKLOG("%llu graphics pipelines", (unsigned long long)c);
    __atomic_add_fetch(&g_vkst.pipes_built, 1, __ATOMIC_RELAXED);
    return p;   // (VkPipeline)1 = failed
}

// ---------------- descriptors ----------------
static uint64_t c2_handle(int st, int off) {   // the driver's handle table c2, as dmsh_build.py lays it out
    if (off >= 0x20 && off < 0x120) return S.tex[st][(off - 0x20) / 8];
    if (off >= 0x120 && off < 0x160) return S.img[st][(off - 0x120) / 8];
    if (off >= 0x168 && off < 0x568) return S.tex[st][128 + (off - 0x168) / 8];
    if (off >= 0x568 && off < 0x968) return S.samp[st][(off - 0x568) / 8];
    return 0;
}
static uint32_t ubo_word(int st, int reg, int off) {
    const p_bind_buf *u = reg >= 0 && reg < 16 ? &S.ubo[st][reg] : NULL; uint32_t v = 0;
    if (u && u->addr && off >= 0 && (uint64_t)off + 4 <= (u->size ? u->size : 4) && vk_pool_of(u->addr + off, 4)) vk_ubd_read(u->addr + off, 4, (uint8_t *)&v);   // with the deferred updates
    return v;
}
static uint32_t g_dispatch[3], g_draw_binst;
#define SB_ALIGN 256   // storage buffers are bound from this boundary below their address (dm/android/tools/shader_vk.py SB_ALIGN, which the shaders were translated for)
static void drv_segment(int st, int bank, uint8_t *d) {
    memset(d, 0, 0x1000);
    if (bank == 0) {
        int base = st == 0 ? 0x110 : st == 5 ? 0x310 : st == 1 ? 0x510 : -1;
        if (base >= 0) for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[st][r].addr; uint32_t n = (uint32_t)S.ssbo[st][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
        if (st != 5) { const float mp = 2047.0f; memcpy(d + 0x610, &mp, 4); memcpy(d + 0x644, &g_draw_binst, 4); }
        else { memcpy(d + 0x200, g_dispatch, 12); for (int r = 0; r < 16; r++) { uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size; memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4); } }
    } else for (int off = 0x20; off < 0x968; off += 8) { uint64_t h = c2_handle(st, off); memcpy(d + off, &h, 8); }
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
static VkSampler samp_for(int st, const dmsv_bind *b, uint64_t texh, int nearest) {   // nearest: its unfiltered twin (integer views)
    int sid = 0;
    switch (b->samp) {
    case RS_TEX: sid = (int)(texh >> 20); break;
    case RS_SLOT: sid = b->samp_arg >= 0 && b->samp_arg < 128 ? (int)(S.samp[st][b->samp_arg] >> 20) : 0; break;
    case RS_ID: sid = b->samp_arg; break;
    case RS_UBO: sid = (int)(ubo_word(st, b->samp_arg & 0xff, b->samp_arg >> 8) >> 20); break;
    default: break;
    }
    VkSampler vk_samp_by_id_nearest(int sid);
    VkSampler s = sid ? (nearest ? vk_samp_by_id_nearest(sid) : samp_cached(sid)) : VK_NULL_HANDLE;
    if (g_tracing) { char d[256]; void vk_samp_describe(int sid, char *out, size_t n); vk_samp_describe(sid, d, sizeof d); VKLOG("  binding %d: %s", b->binding, d); }
    return s ? s : vk_dummy_sampler();
}
// texture views by ID within a submission: a draw's texture arrays (32 slots) looked every slot up again, each through the registry's and the view table's
// locks (NEO: about 2 ms a frame). An entry holds while no texture ID is registered again and no view is dropped (g_tex_gen)
enum { TVC = 4096 };
static struct { uint64_t epoch, gen; int tid, dim, arr, num; VkImageView v; vkimg_t *im; } g_tvc[TVC];
// what a view's texels are, as gfx_vk_shader.c's integer-texel variants need it: 0 float, 1 uint (unsigned integer formats, a stencil), 2 int
static int view_num(const tex_t *t, const view_t *v) {
    const vkfmt_t *f = vk_fmt(v && v->set_format ? v->format : t->d.format); if (!f) return 0;
    if (f->depth || f->stencil) return f->stencil && (!f->depth || t->stencil || (v && v->set_dsmode && v->dsmode == 1));   // vk_tex_view's aspect
    return f->is_int ? (f->is_sint ? 2 : 1) : 0;
}
static uint64_t g_num_um, g_num_im;   // stage_set: the sampled bindings of the stage being built that got uint / int views (bit = binding index)
extern uint64_t g_tex_gen;
static int tex_desc_for(int st, const dmsv_bind *b, uint64_t h, VkDescriptorImageInfo *ii, int storage) {   // the view's view_num
    int tid = (int)(h & 0xfffff); const view_t *v = NULL; const tex_t *t = NULL;
    ii->imageLayout = VK_IMAGE_LAYOUT_GENERAL; ii->sampler = storage ? VK_NULL_HANDLE : samp_for(st, b, h, 0); ii->imageView = VK_NULL_HANDLE;
    unsigned ck = ((unsigned)tid * 2654435761u ^ (unsigned)b->dim * 97u ^ (unsigned)(b->flags & 1)) % TVC;
    if (tid && g_var_now != 3 && !g_tracing && g_tvc[ck].epoch == g_set_epoch && g_tvc[ck].gen == g_tex_gen && g_tvc[ck].tid == tid && g_tvc[ck].dim == b->dim && g_tvc[ck].arr == (b->flags & 1)) {
        ii->imageView = g_tvc[ck].v; if (g_tvc[ck].im) g_tvc[ck].im->last_use = mtl_last_fence() + 1; vk_texdump_note(g_tvc[ck].im);
        if (g_tvc[ck].num && !storage) ii->sampler = samp_for(st, b, h, 1);
        return g_tvc[ck].num;
    }
    int num = 0;
    if (tid && g_var_now != 3 && vk_tex_by_id(tid, &v, &t) && t) {
        vkimg_t *im = NULL; ii->imageView = vk_tex_view(t, v, b->dim, b->flags & 1, 0, &im); if (storage && im) im->gpu_w = 1;
        if (g_tracing) VKLOG("  texture handle %#llx id %d: %ux%u nvn format %#x target %d serial %llu pool %p+%#llx size %#llx → image %p (%ux%u fmt %d layers %u levels %u) view %p", (unsigned long long)h, tid, t->d.width, t->d.height, t->d.format, t->d.target,
                             (unsigned long long)t->serial, (const void *)t->pool, (unsigned long long)t->off, (unsigned long long)t->L.size, (void *)im, im ? im->w : 0, im ? im->h : 0, im ? im->fmt : 0, im ? im->layers : 0, im ? im->levels : 0, (void *)ii->imageView);
        if (im) im->last_use = mtl_last_fence() + 1;
        if (ii->imageView) num = view_num(t, v);
        if (ii->imageView) { g_tvc[ck].epoch = g_set_epoch; g_tvc[ck].gen = g_tex_gen; g_tvc[ck].tid = tid; g_tvc[ck].dim = b->dim; g_tvc[ck].arr = b->flags & 1; g_tvc[ck].v = ii->imageView; g_tvc[ck].im = im; g_tvc[ck].num = num; }
    }
    if (num && !storage) ii->sampler = samp_for(st, b, h, 1);
    if (!ii->imageView) {
        const vkfmt_t *f = t ? vk_fmt(t->d.format) : NULL;
        ii->imageView = vk_dummy_view(b->dim == 0xff ? 1 : b->dim, b->flags & 1, (b->flags & 2) != 0, storage, f && f->is_int); num = f && f->is_int ? (f->is_sint ? 2 : 1) : 0;
    }
    return num;
}
// Reuse within a submission (the cache is emptied when it begins, vk_exec_reset): descriptor sets by their whole content (layout, and every buffer range,
// view, sampler and texel buffer view written), and driver segments by their bytes. Unity draws many times with the same bindings, and every new set
// is an allocation and a vkUpdateDescriptorSets; the driver segment's copy is shared so that a set using it can match.
enum { SETC = 8192, DRVC = 1024 };
static struct { uint64_t h, epoch; VkDescriptorSetLayout l; VkDescriptorSet set; } g_setc[SETC];
static struct { uint64_t h, epoch; VkBuffer b; VkDeviceSize o; } g_drvc[DRVC];
uint64_t g_set_hits, g_set_new, g_ns_set_build, g_ns_set_hash, g_ns_set_new;   // descriptor sets: reused, new; time collecting their descriptors, hashing them, allocating and writing new ones
// content hash of descriptor sets and driver segments: 8 bytes a step (a set with 32-slot texture arrays is several KB, which byte-wise FNV took milliseconds a frame over)
static inline uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p; size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
static void drv_shared(int st, int bank, VkBuffer *buf, VkDeviceSize *off) {   // the driver segment: built, then one copy per distinct content in the submission
    uint8_t d[0x1000]; drv_segment(st, bank, d);
    // only the bytes drv_segment can write are hashed (bank 0 ends at 0x648, the c2 table at 0x968; the rest is always zero): 4 KB was 512 hash steps a segment
    uint64_t h = fnv(0xcbf29ce484222325ull, d, bank ? 0x968 : 0x648); unsigned k = (unsigned)(h % DRVC);
    if (g_drvc[k].epoch == g_set_epoch && g_drvc[k].h == h && !g_x_nodrvc) { *buf = g_drvc[k].b; *off = g_drvc[k].o; return; }
    uint8_t *p = vk_scratch(0x1000, V.props.limits.minUniformBufferOffsetAlignment, buf, off); memcpy(p, d, sizeof d);
    g_drvc[k].h = h; g_drvc[k].epoch = g_set_epoch; g_drvc[k].b = *buf; g_drvc[k].o = *off;
}
// a constant buffer binding's descriptor (stage_set, set_memo_get): its pool's buffer from the alignment boundary below it, or the shadow of the deferred updates over it
static void cbuf_desc(int st, const vkshader_t *s, const dmsv_bind *b, VkDescriptorBufferInfo *bi) {
    const p_bind_buf *u = b->reg >= 0 && b->reg < 16 ? &S.ubo[st][b->reg] : NULL;
    uint64_t n = u && u->size ? (u->size > 65536 ? 65536 : u->size) : 65536; VkBuffer buf; VkDeviceSize off;
    vkpool_t *p = g_var_now != 9 && u && u->addr ? vk_pool_of(u->addr, 1) : NULL;
    if (!p) { *bi = (VkDescriptorBufferInfo){vk_zero_buffer(), 0, 65536}; return; }
    buf = p->buf; off = u->addr - p->lo; if (off + n > p->hi - p->lo) n = p->hi - p->lo - off;
    ubo_log(s, st, b->reg, u->addr, n);
    if (vk_ubd_shadow(u->addr, n, &buf, &off)) { *bi = (VkDescriptorBufferInfo){buf, off, n}; return; }   // deferred updates in it: their shadow
    VkDeviceSize al = V.props.limits.minUniformBufferOffsetAlignment; if (off % al) VKRED_ONCE(40, "uniform buffer at an offset not aligned to %llu", (unsigned long long)al);
    *bi = (VkDescriptorBufferInfo){buf, off - off % al, n + off % al};
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
typedef struct { vkshader_t *s; uint64_t epoch, bind, ubo, ubd, tex, samp, h, um, im; uint32_t pool, binst, dofs[96]; int ncb; uint8_t ci[MEMO_CB], cpos[MEMO_CB]; VkDescriptorBufferInfo cd[MEMO_CB]; VkDescriptorSet set; } set_memo_t;
static set_memo_t g_smemo[3]; static uint64_t g_bind_gen[6], g_ubo_gen[6];   // a stage's handles or storage buffers changed (bind_handles_rec, OP_BIND_SSBO), its uniform buffers (OP_BIND_UBO)
uint64_t g_set_memo, g_set_memo_bad;   // memo hits, and differences the verify mode found
extern uint64_t g_ubd_gen; uint32_t vk_pool_gen(void);
static int set_memo_ok(vkshader_t *s) {
    if (!s->setmemo) { int ok = 1, ncb = 0;
        for (int i = 0; i < s->nbind; i++) { const dmsv_bind *b = &s->b[i]; ncb += b->kind == RK_CBUF;
            ok &= b->kind != RK_SBUF_UBO && b->kind != RK_UBO_PTR && b->kind != RK_TEX_UBO && b->kind != RK_IMG_UBO && b->kind != RK_TBUF_UBO && b->samp != RS_UBO && i < 256; }
        s->setmemo = ok && ncb <= MEMO_CB ? 1 : 2; }
    return s->setmemo == 1;
}
static int set_memo_get(int st, vkshader_t *s, uint32_t *dofs) {
    set_memo_t *m = &g_smemo[st];
    if (m->s != s || m->epoch != g_set_epoch || m->bind != g_bind_gen[st] || m->tex != g_tex_gen || m->samp != __atomic_load_n(&g_samp_gen, __ATOMIC_ACQUIRE) || m->pool != vk_pool_gen() || m->binst != g_draw_binst) return 0;
    if (m->ubo != g_ubo_gen[st] || m->ubd != g_ubd_gen) {   // the constant buffers again: most draws move only their offsets
        for (int c = 0; c < m->ncb; c++) { VkDescriptorBufferInfo d; cbuf_desc(st, s, &s->b[m->ci[c]], &d);
            if (d.buffer != m->cd[c].buffer || d.range != m->cd[c].range || (m->cpos[c] == 0xff && d.offset != m->cd[c].offset)) return 0;
            if (m->cpos[c] != 0xff) m->dofs[m->cpos[c]] = (uint32_t)d.offset; }
        m->ubo = g_ubo_gen[st]; m->ubd = g_ubd_gen; }
    memcpy(dofs, m->dofs, sizeof *dofs * (size_t)s->ndyn); g_num_um = m->um; g_num_im = m->im;
    return 1;
}
// dofs (at least s->ndyn entries): the dynamic offsets of the set's dynamic uniform buffers, in binding order as vkCmdBindDescriptorSets takes them
static VkDescriptorSet stage_set(int st, vkshader_t *s, uint32_t *dofs) {
    int memo = st < 3 && !g_x_nosetmemo && !g_tracing && !g_var_now && !g_x_nosetc && !g_x_nodrvc && !g_ubolog_key && g_td_off < 0 && set_memo_ok(s), verify = 0;
    uint32_t vd[96]; uint64_t vum = 0, vim = 0;   // verify: what the memo gave
    if (memo && set_memo_get(st, s, dofs)) { static unsigned nv; if (!g_x_setverify || ++nv % 64) { g_set_memo++; return g_smemo[st].set; }
        verify = 1; memcpy(vd, dofs, sizeof *dofs * (size_t)s->ndyn); vum = g_num_um; vim = g_num_im; }
    uint64_t gtex = g_tex_gen, gsamp = __atomic_load_n(&g_samp_gen, __ATOMIC_ACQUIRE); uint32_t gpool = vk_pool_gen();   // before collecting: a change racing it (game thread) leaves the memo outdated
    VkDescriptorSet set = VK_NULL_HANDLE; struct { uint32_t b, o; int i; } dp[96]; int nd = 0; uint64_t tb0 = tick();
    VkWriteDescriptorSet w[96]; VkDescriptorBufferInfo bi[96]; VkDescriptorImageInfo ii[96 * 4]; VkBufferView bv[96]; uint32_t nw = 0, nii = 0;
    int ncb = 0; uint8_t mci[MEMO_CB]; VkDescriptorBufferInfo mcd[MEMO_CB];   // the constant buffers, for the memo
    g_num_um = g_num_im = 0;
    for (int i = 0; i < s->nbind && nw < 96; i++) {
        const dmsv_bind *b = &s->b[i];
        VkWriteDescriptorSet *wr = &w[nw]; memset(wr, 0, sizeof *wr);
        wr->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr->dstSet = set; wr->dstBinding = b->binding; wr->descriptorCount = 1;
        VkBuffer buf; VkDeviceSize off, len;
        switch (b->kind) {
        case RK_SUPPORT: bi[nw] = (VkDescriptorBufferInfo){vk_support_buffer(), 0, 65536}; break;
        case RK_C1: vk_shader_c1(s, &buf, &off, &len); bi[nw] = (VkDescriptorBufferInfo){buf, off, len}; break;
        case RK_CBUF: cbuf_desc(st, s, b, &bi[nw]); if (ncb < MEMO_CB) { mci[ncb] = (uint8_t)i; mcd[ncb++] = bi[nw]; } break;
        case RK_DRV: { VkBuffer sb; VkDeviceSize so; drv_shared(st, b->reg, &sb, &so); bi[nw] = (VkDescriptorBufferInfo){sb, so, 0x1000}; break; }
        case RK_SBUF: case RK_UBO_PTR: case RK_SBUF_UBO: {
            uint64_t a = 0, n = 0;
            if (b->kind == RK_SBUF && b->reg >= 0 && b->reg < 16) { a = S.ssbo[st][b->reg].addr; n = S.ssbo[st][b->reg].size; }
            else if (b->kind == RK_UBO_PTR && b->reg >= 0 && b->reg < 16) { a = S.ubo[st][b->reg].addr; n = S.ubo[st][b->reg].size; }
            else if (b->kind == RK_SBUF_UBO) { a = (uint64_t)ubo_word(st, b->reg, b->off) | (uint64_t)ubo_word(st, b->reg, b->off + 4) << 32; }
            vkpool_t *p = a ? vk_pool_of(a, 1) : NULL;
            if (p) { buf = p->buf; off = a - p->lo; if (!n || off + n > p->hi - p->lo) n = p->hi - p->lo - off;
                // bound from the 256-byte boundary below it, the boundary the translator offsets every access from (shader_vk.py SB_ALIGN; a multiple of any device's minimum alignment)
                const VkDeviceSize al = SB_ALIGN; if (V.props.limits.minStorageBufferOffsetAlignment > al) VKRED_ONCE(41, "storage buffer alignment %llu above %llu", (unsigned long long)V.props.limits.minStorageBufferOffsetAlignment, (unsigned long long)al);
                bi[nw] = (VkDescriptorBufferInfo){buf, off - off % al, n + off % al};
                if (st == 5 && b->kind != RK_UBO_PTR) { void vk_gpu_wrote(uint64_t a, uint64_t n); vk_gpu_wrote(a, n); }   // a dispatch's storage buffers: taken as written
            } else bi[nw] = (VkDescriptorBufferInfo){vk_zero_buffer(), 0, 65536};
            break; }
        case RK_TEX: case RK_TEX_UBO: case RK_TEX_C2A: {
            int cnt = b->count ? b->count : 32; if (nii + (uint32_t)cnt > sizeof ii / sizeof *ii) cnt = 1;
            for (int k = 0; k < cnt; k++) {
                uint64_t h = b->kind == RK_TEX ? (b->reg >= 0 && b->reg < 256 ? S.tex[st][b->reg] : 0) :
                             b->kind == RK_TEX_UBO ? ubo_word(st, b->reg, b->off + 8 * k) : (b->off >= 0 ? c2_handle(st, b->off + 8 * k) : 0);
                int num = tex_desc_for(st, b, h, &ii[nii + k], 0);
                if (num && i < 64) { if (num == 1) g_num_um |= 1ull << i; else g_num_im |= 1ull << i; }
            }
            wr->pImageInfo = &ii[nii]; nii += (uint32_t)cnt; wr->descriptorCount = (uint32_t)cnt; wr->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; nw++;
            continue; }
        case RK_IMG: case RK_IMG_UBO: {
            uint64_t h = b->kind == RK_IMG ? (b->reg >= 0 && b->reg < 32 ? S.img[st][b->reg] : 0) : ubo_word(st, b->reg, b->off);
            tex_desc_for(st, b, h, &ii[nii], 1); wr->pImageInfo = &ii[nii++]; wr->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; nw++;
            continue; }
        case RK_TBUF: case RK_TBUF_UBO: case RK_IMG_TBUF: {
            uint64_t h = b->kind == RK_TBUF ? (b->reg >= 0 && b->reg < 256 ? S.tex[st][b->reg] : 0) : b->kind == RK_IMG_TBUF ? (b->reg >= 0 && b->reg < 32 ? S.img[st][b->reg] : 0) : ubo_word(st, b->reg, b->off);
            const view_t *v; const tex_t *t = NULL; int tid = (int)(h & 0xfffff);
            bv[nw] = tid && vk_tex_by_id(tid, &v, &t) && t && t->d.target == NVN_TEXTURE_TARGET_BUFFER ? vk_tbuf_view(t) : VK_NULL_HANDLE;
            if (!bv[nw]) bv[nw] = vk_dummy_tbuf(b->kind == RK_IMG_TBUF);
            else if (b->kind != RK_IMG_TBUF && i < 64) { int num = view_num(t, NULL); if (num == 1) g_num_um |= 1ull << i; else if (num == 2) g_num_im |= 1ull << i; }   // texel buffers read their words alike
            wr->pTexelBufferView = &bv[nw]; wr->descriptorType = b->kind == RK_IMG_TBUF ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; nw++;
            continue; }
        default: {   // unknown: the descriptor type it was declared with, filled with what reads zero
            if (b->desc == RD_UBO || b->desc == RD_SSBO) bi[nw] = (VkDescriptorBufferInfo){vk_zero_buffer(), 0, 65536};
            else if (b->desc == RD_SAMPLED || b->desc == RD_STORAGE) {
                ii[nii] = (VkDescriptorImageInfo){b->desc == RD_SAMPLED ? vk_dummy_sampler() : VK_NULL_HANDLE, vk_dummy_view(b->dim == 0xff ? 1 : b->dim, b->flags & 1, (b->flags & 2) != 0, b->desc == RD_STORAGE, 0), VK_IMAGE_LAYOUT_GENERAL};
                int cnt = b->count ? b->count : 32; for (int k = 1; k < cnt && nii + k < sizeof ii / sizeof *ii; k++) ii[nii + k] = ii[nii];
                wr->pImageInfo = &ii[nii]; nii += (uint32_t)cnt; wr->descriptorCount = (uint32_t)cnt;
                wr->descriptorType = b->desc == RD_SAMPLED ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; nw++; continue;
            } else if (b->desc == RD_TBUF || b->desc == RD_STORAGE_TBUF) { bv[nw] = vk_dummy_tbuf(b->desc == RD_STORAGE_TBUF); wr->pTexelBufferView = &bv[nw];
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
    // the set's content, then an earlier identical set of this submission or a new one
    uint64_t tb1 = tick(); g_ns_set_build += tb1 - tb0;
    uint64_t h = fnv(0xcbf29ce484222325ull, &s->dsl, sizeof s->dsl);
    for (uint32_t i = 0; i < nw; i++) {
        const VkWriteDescriptorSet *x = &w[i]; h = fnv(h, &x->dstBinding, 4); h = fnv(h, &x->descriptorType, 4); h = fnv(h, &x->descriptorCount, 4);
        if (x->pBufferInfo) h = fnv(h, x->pBufferInfo, sizeof *x->pBufferInfo);
        else if (x->pImageInfo) h = fnv(h, x->pImageInfo, sizeof *x->pImageInfo * x->descriptorCount);
        else if (x->pTexelBufferView) h = fnv(h, x->pTexelBufferView, sizeof *x->pTexelBufferView);
    }
    unsigned k = (unsigned)(h % SETC); uint64_t tb2 = tick(); g_ns_set_hash += tb2 - tb1;
    if (g_setc[k].epoch == g_set_epoch && g_setc[k].h == h && g_setc[k].l == s->dsl && !g_tracing && !g_x_nosetc) { g_set_hits++; set = g_setc[k].set; }
    else { set = vk_alloc_set(s->dsl); g_set_new++;
        for (uint32_t i = 0; i < nw; i++) w[i].dstSet = set;
        vkUpdateDescriptorSets(V.dev, nw, w, 0, NULL);
        g_setc[k].h = h; g_setc[k].epoch = g_set_epoch; g_setc[k].l = s->dsl; g_setc[k].set = set; g_ns_set_new += tick() - tb2; }
    // verify: the same content (hash), integer-texel bindings and dynamic offsets as the memo gave. A lone difference in dofs alone can be a driver segment
    // copied again after its g_drvc entry was taken by another (the memo's copy is as good); anything repeated is a dependency the memo misses
    if (verify && (h != g_smemo[st].h || g_num_um != vum || g_num_im != vim || memcmp(vd, dofs, sizeof *dofs * (size_t)s->ndyn))) { g_set_memo_bad++; static int n;
        if (n++ < 20) VKLOG("[RED] descriptor set memo: stage %d shader %016llx: a fresh set differs from the memo's (hash %016llx / %016llx, um %#llx / %#llx, im %#llx / %#llx, dynamic offsets %s)", st, (unsigned long long)s->key,
                            (unsigned long long)h, (unsigned long long)g_smemo[st].h, (unsigned long long)g_num_um, (unsigned long long)vum, (unsigned long long)g_num_im, (unsigned long long)vim, memcmp(vd, dofs, sizeof *dofs * (size_t)s->ndyn) ? "differ" : "same"); }
    if (memo) { set_memo_t *m = &g_smemo[st];   // (set_memo_ok: at most MEMO_CB constant buffers)
        m->s = s; m->epoch = g_set_epoch; m->bind = g_bind_gen[st]; m->ubo = g_ubo_gen[st]; m->ubd = g_ubd_gen; m->tex = gtex; m->samp = gsamp; m->pool = gpool; m->binst = g_draw_binst;
        m->h = h; m->um = g_num_um; m->im = g_num_im; m->set = set; memcpy(m->dofs, dofs, sizeof *dofs * (size_t)s->ndyn); m->ncb = ncb; memcpy(m->ci, mci, (size_t)ncb); memcpy(m->cd, mcd, sizeof *mcd * (size_t)ncb);
        for (int c = 0; c < ncb; c++) { m->cpos[c] = 0xff; for (int j = 0; j < nd; j++) if (dp[j].i == mci[c]) m->cpos[c] = (uint8_t)j; } }
    return set;
}

// ---------------- draws ----------------
// What the draws of the command buffer being recorded set last: a draw that would set the same again records nothing. Vulkan keeps dynamic state and
// bindings for the whole command buffer, across render passes and across pipelines that declare the same dynamic state (every one of ours, build_pipeline);
// a new command buffer (g_cmd_gen, gfx_vk_core.c) starts empty. Xenoblade Chronicles 3 docked: ~1,900 draws a frame, each with 10 dynamic-state calls and
// its binds, most the same as the draw before; the driver's time grows with what is recorded (vkEndCommandBuffer too). DM_VK_X_NOBC=1: everything every draw
extern uint64_t g_cmd_gen;
static struct { uint64_t gen; VkViewport vp; VkRect2D sc; float bc[4]; int sm[2], sw[2], sr[2], line; p_poly_offset po; VkPipelineLayout lay; uint32_t nsets, ndofs;
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
    if (!BC_SAME(sm, S.smask)) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.smask[0]); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.smask[1]); memcpy(g_bc.sm, S.smask, sizeof g_bc.sm); }
    if (!BC_SAME(sw, S.swmask)) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.swmask[0]); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.swmask[1]); memcpy(g_bc.sw, S.swmask, sizeof g_bc.sw); }
    if (!BC_SAME(sr, S.sref)) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.sref[0]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.sref[1]); memcpy(g_bc.sr, S.sref, sizeof g_bc.sr); }
    if (!BC_SAME(po, S.pofs)) { vkCmdSetDepthBias(cb, S.pofs.units, S.pofs.clamp, S.pofs.factor); g_bc.po = S.pofs; }
    if (fresh || !g_bc.line) { vkCmdSetLineWidth(cb, 1.0f); g_bc.line = 1; }
}
static int g_no_cull = -1;
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
    if (!begin_pass() || !pass_commit()) { g_vkst.skip_rt++; return 0; }
    if (g_x_tracepass >= 0 && g_sub_pass - 1 == g_x_tracepass) g_tracing = 1;
    uint64_t t0 = tick();
    VkCommandBuffer cb = vk_cmd();
    if (g_no_cull < 0) g_no_cull = getenv("DM_VK_NOCULL") != NULL;
    pkey k; memset(&k, 0, sizeof k);
    for (int i = 0; i < 3; i++) k.key[i] = S.sh[i] ? S.sh[i]->key : 0;
    k.topo = topo(prim); k.restart = S.prim_restart && (k.topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || k.topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || k.topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) && indexed;
    k.raster_discard = S.rast_discard; k.alt = (uint32_t)g_spv_alt; k.cull = g_no_cull ? 0 : S.poly.cull;
    // counter-clockwise front faces in Vulkan's framebuffer: Vulkan's clip space has y down, Metal's up (nvn_mtl.m: CCW when (front == CCW) != lower left), and the
    // lower-left origin mode flips the viewport again, so CCW when (front == CCW) == lower left. Taken as given, upper-left games (NEO) culled their ground and walls
    k.front = ((S.poly.front == NVN_FRONT_FACE_CCW) == (g_lower_left != 0)) != (g_flipy != 0); k.mode = S.poly.mode; k.depth_clamp = S.depth_clamp;
    k.bias_en = (S.poly.offset_en & (S.poly.mode == 0 ? 1 : S.poly.mode == 1 ? 2 : 4)) != 0;
    if (var == 2) k.bias_en = 0;
    if (var == 8) k.raster_discard = 1;   // no fragment shader in the pipeline
    int has_depth = (S.daspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, has_stencil = (S.daspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    k.dt = has_depth && S.ds.dt; k.dw = S.ds.dw; k.df = S.ds.df; k.st = has_stencil && S.ds.st;
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
    for (int loc = 0; loc < 16; loc++) {
        if (!(in_mask >> loc & 1)) continue;
        va_st a; memcpy(&a, &S.vattr[loc], 4);
        VkFormat f = a.fmt ? vertex_format(a.fmt, int_mask >> loc & 1, sint_mask >> loc & 1) : VK_FORMAT_UNDEFINED;
        if (f == VK_FORMAT_UNDEFINED || !S.vbuf[a.stream].address) { k.attr[k.nattr++] = (typeof(k.attr[0])){(uint32_t)loc, 16, int_mask >> loc & 1 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT, 0}; need_zero = 1; continue; }
        k.attr[k.nattr++] = (typeof(k.attr[0])){(uint32_t)loc, a.stream, f, a.offset};
        used_streams |= 1u << a.stream;
    }
    for (int s = 0; s < 16; s++) if (used_streams >> s & 1) k.bind[k.nbind++] = (typeof(k.bind[0])){(uint32_t)s, S.vstream[s].stride, S.vstream[s].divisor ? 1u : 0u, S.vstream[s].divisor};
    if (need_zero) k.bind[k.nbind++] = (typeof(k.bind[0])){16, 0, 0, 0};
    // attachments and blending
    k.ncol = (uint32_t)S.ncol; k.dfmt = S.dfmt; k.samples = S.samples ? S.samples : 1; k.a2c = S.ms.a2c;
    k.logic_en = S.color.logic_op != 3 && S.color.logic_op != 0 && 0;   // logic ops: not on mobile render targets in version 1
    k.logic_op = S.color.logic_op;
    for (int i = 0; i < S.ncol; i++) {
        k.cfmt[i] = S.cfmt[i];
        k.wmask[i] = S.cfmt[i] && S.sh[1] ? (S.chmask.mask >> (4 * i)) & 0xf : 0;   // no fragment shader: no colour written
        const vkfmt_t *f = S.rt_has_col[i] ? vk_fmt(S.rt_col[i].d.format) : NULL;
        if (((S.color.blend_en >> i) & 1) && f && !f->is_int) {
            k.blend_en |= 1u << i;
            k.bl[i].src = S.blend[i].src_rgb; k.bl[i].dst = S.blend[i].dst_rgb; k.bl[i].srca = S.blend[i].src_a; k.bl[i].dsta = S.blend[i].dst_a; k.bl[i].eq = S.blend[i].eq_rgb; k.bl[i].eqa = S.blend[i].eq_a;
        }
    }
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
    for (int st = 0; st < 3; st++) if (S.sh[st] && !(st == 1 && k.raster_discard)) { sets[S.sh[st]->set] = stage_set(st, S.sh[st], dofs[S.sh[st]->set]); k.um[st] = g_num_um; k.im[st] = g_num_im; }
    uint64_t tdesc = tick() - t1d; g_vkst.ns_desc += tdesc;
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
    if (g_ubolog_key && g_ubolog_n > 0 && S.sh[0] && S.sh[0]->key == g_ubolog_key) {   // DM_VK_UBO_LOG: the pipeline the draw got, and its key's hash
        uint64_t kh = fnv(1469598103934665603ull, &k, sizeof k); VKLOG("ubo: present %llu pipeline %p key %016llx um %llx/%llx im %llx/%llx", (unsigned long long)g_tex_frame, (void *)p, (unsigned long long)kh,
              (unsigned long long)k.um[0], (unsigned long long)k.um[1], (unsigned long long)k.im[0], (unsigned long long)k.im[1]); }
    g_vkst.draws++; if (++g_pass_draws > g_vkst.max_pass_draws) g_vkst.max_pass_draws = g_pass_draws;
    if (p != S.bound_pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); S.bound_pipe = p; }
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
        if (b == 16) { buf = vk_zero_buffer(); off = 0; }
        else if (!vk_buf_at(S.vbuf[b].address, 1, &buf, &off)) { buf = vk_zero_buffer(); off = 0; VKRED_ONCE(51, "vertex buffer outside the memory pools"); }
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
static int xfb_begin(VkCommandBuffer cb) {   // inside the pass, after the pipeline is bound; 1 when a capture began
    vkshader_t *vs = S.sh[0];
    if (!g_tf_on || !V.has_xfb || !vs || !vs->xfb || !g_xfb_on) return 0;
    VkBuffer b[4]; VkDeviceSize o[4], n[4];
    for (int k = 0; k < 4; k++) {
        vkpool_t *p = g_tf[k].addr ? vk_pool_of(g_tf[k].addr + g_tf_off[k], 4) : NULL;
        if (vs->xfb_stride[k] && p && g_tf_off[k] < g_tf[k].size && vk_buf_at(g_tf[k].addr + g_tf_off[k], 4, &b[k], &o[k])) {
            n[k] = g_tf[k].size - g_tf_off[k]; if (o[k] + n[k] > p->hi - p->lo) n[k] = p->hi - p->lo - o[k];
        } else { vk_scratch(64, 64, &b[k], &o[k]); n[k] = 64; if (vs->xfb_stride[k]) VKRED_ONCE(83, "transform feedback buffer %d unbound, full or outside the memory pools: its capture is dropped", k); }
    }
    vkCmdBindTransformFeedbackBuffersEXT(cb, 0, 4, b, o, n);
    vkCmdBeginTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
    return 1;
}
static void xfb_end(VkCommandBuffer cb, int prim, uint32_t count, uint32_t inst) {   // after the draw: the capture ends and the bytes it wrote are counted
    vkCmdEndTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
    uint32_t v = xfb_verts(prim, count) * (inst ? inst : 1); g_tf_in_pass = 1; g_tf_verts += v;
    for (int k = 0; k < 4; k++) {
        uint64_t m = (uint64_t)v * S.sh[0]->xfb_stride[k]; if (!m || !g_tf[k].addr) continue;
        if (g_tf_off[k] + m > g_tf[k].size) m = g_tf[k].size > g_tf_off[k] ? g_tf[k].size - g_tf_off[k] : 0;
        void vk_gpu_wrote(uint64_t a, uint64_t n); if (m) vk_gpu_wrote(g_tf[k].addr + g_tf_off[k], m);
        g_tf_off[k] += m;
    }
}
// DrawTransformFeedback draws the vertices the captures ended into one control buffer wrote: counted since Begin (g_tf_verts) and kept by control buffer address at End / Pause
// (a table by address: Xenoblade Chronicles 3 gives every particle emitter a control buffer of its own, hundreds of them, 32 bytes apart)
enum { TFC = 4096 }; static struct { uint64_t ctl; uint32_t verts; } g_tf_ctl[TFC];
static int tf_ctl_slot(uint64_t ctl, int add) {   // the entry of a control buffer; with add, a new one (the oldest in its probe range is replaced when they are all taken)
    unsigned h = (unsigned)((ctl >> 5) * 2654435761u) % TFC;
    for (int k = 0; k < 16; k++) { unsigned i = (h + (unsigned)k) % TFC; if (g_tf_ctl[i].ctl == ctl) return (int)i; if (!g_tf_ctl[i].ctl) return add ? (int)i : -1; }
    return add ? (int)h : -1;
}
static void tf_state(const p_tf_state *t) {
    switch (t->what & 3) {
    case 0: memset(g_tf_off, 0, sizeof g_tf_off); g_tf_verts = 0; g_tf_on = 1; break;   // Begin: written from the start of each buffer
    case 3: g_tf_on = 1; break;                                          // Resume: on from the bytes written
    default: g_tf_on = 0;                                                // End / Pause: what was captured is read by later draws (as vertices), after the barrier the next pass begins with
        if (g_tf_in_pass) { vk_end_pass(); g_tf_in_pass = 0; }
        int i = tf_ctl_slot(t->ctl, 1); g_tf_ctl[i].ctl = t->ctl; g_tf_ctl[i].verts = g_tf_verts;
    }
}
static void draw(const p_draw *d);
static void draw_tf(const p_draw_tf *t) {
    int i = tf_ctl_slot(t->ctl, 0);
    if (i < 0) { VKRED_ONCE(86, "DrawTransformFeedback with a control buffer no capture ended into (%#llx): nothing drawn", (unsigned long long)t->ctl); return; }
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
static void draw(const p_draw *d) {
    if (d->mdi_cnt) VKRED_ONCE(52, "multi-draw indirect with a count: drawn once");
    int prim = d->prim;
    // before prepare_draw, whose driver segment carries it (c0 + 0x644): set after it, each draw's segment had the draw before's. An indirect draw's is in GPU memory: 0
    g_draw_binst = d->ind ? 0 : (uint32_t)d->base_instance;
    if (!prepare_draw(prim == 7 ? 4 : prim, prim == 7)) return;
    VkCommandBuffer cb = vk_cmd();
    int g_ok = draw_allowed(); if (g_ok == 2 || (g_tracing && g_x_tracepass >= 0)) trace_draw("draw", d->prim, (uint32_t)d->count, (uint32_t)d->instances, -1);
    if (g_var_now == 10 && prim == 7) g_ok = 0; if (g_var_now == 11 && prim != 7) g_ok = 0;   // bisection: quads from the found draw on, or the rest
    { static int nq, nn; if (g_var_now) { if (prim == 7) nq++; else nn++; if ((nq + nn) % 50 == 1) VKLOG("draws from the found one on: %d quads, %d others", nq, nn); } }
    uint32_t inst = d->instances ? (uint32_t)d->instances : 1;
    { static int n; if (n < 12 && (d->ind || (uint32_t)d->count > (1u << 20) || inst > (1u << 16))) { n++;
        VKLOG("unusual draw: count %u instances %u indirect %#llx prim %d first %d", (unsigned)d->count, inst, (unsigned long long)d->ind, d->prim, d->first); } }
    if (d->ind) {
        VkBuffer b; VkDeviceSize o;
        if (g_tf_on && S.sh[0] && S.sh[0]->xfb) VKRED_ONCE(84, "indirect draw under transform feedback: not captured");
        if (vk_buf_at(d->ind, 16, &b, &o)) if (!(dbg_draw() & 1) && g_ok) vkCmdDrawIndirect(cb, b, o, 1, 16); else VKRED_ONCE(53, "indirect draw arguments outside the memory pools");
        return;
    }
    if (prim == 7) {   // quads: the triangle list indices of one static buffer (built once), the first vertex as the vertex offset
        uint32_t nq = (uint32_t)d->count / 4; if (!nq) return;
        if (g_tf_on && S.sh[0] && S.sh[0]->xfb) VKRED_ONCE(85, "quads under transform feedback: not captured");
        VkBuffer qb = quad_index_buffer(&nq);
        bc_index(cb, qb, 0, VK_INDEX_TYPE_UINT32); if (!(dbg_draw() & 1) && g_ok) vkCmdDrawIndexed(cb, nq * 6, inst, 0, g_var_now == 4 ? 0 : d->first, (uint32_t)d->base_instance); return; }
    if (prim == 2) VKRED_ONCE(54, "line loops drawn as line strips");
    if (!(dbg_draw() & 1) && g_ok) { int xf = xfb_begin(cb); vkCmdDraw(cb, (uint32_t)d->count, inst, (uint32_t)d->first, (uint32_t)d->base_instance); if (xf) xfb_end(cb, prim, (uint32_t)d->count, inst); }
}
static void draw_idx(const p_draw_idx *d) {
    if (d->mdi_cnt) VKRED_ONCE(55, "multi-draw indirect (indexed) with a count: drawn once");
    int prim = d->prim;
    g_draw_binst = d->ind ? 0 : (uint32_t)d->base_instance;   // before prepare_draw (see draw)
    if (!prepare_draw(prim == 7 ? 4 : prim, 1)) return;
    VkCommandBuffer cb = vk_cmd();
    int g_ok = draw_allowed(); if (g_ok == 2 || (g_tracing && g_x_tracepass >= 0)) trace_draw("indexed draw", d->prim, (uint32_t)d->count, (uint32_t)d->instances, d->itype);
    if (g_var_now == 11) g_ok = 0;
    VkIndexType it = d->itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : d->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    uint32_t isz = d->itype == 0 ? 1 : d->itype == 1 ? 2 : 4;
    VkBuffer ib; VkDeviceSize io;
    if (!vk_buf_at(d->ibuf, 1, &ib, &io)) { VKRED_ONCE(56, "index buffer outside the memory pools"); return; }
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
    if (d->ind) { VkBuffer b; VkDeviceSize o; bc_index(cb, ib, io, it); if (vk_buf_at(d->ind, 20, &b, &o)) if (!(dbg_draw() & 1) && g_ok) vkCmdDrawIndexedIndirect(cb, b, o, 1, 20); return; }
    if (prim == 7) { VkBuffer b; VkDeviceSize o; int n = quad_indices((uint32_t)d->count, (const uint8_t *)(uintptr_t)d->ibuf, d->itype, 0, &b, &o); if (!n) return;
        bc_index(cb, b, o, VK_INDEX_TYPE_UINT32); if (!(dbg_draw() & 1) && g_ok) vkCmdDrawIndexed(cb, (uint32_t)n, inst, 0, d->base_vertex, (uint32_t)d->base_instance); return; }
    if (io % isz) { VKRED_ONCE(57, "index buffer offset not aligned to its index size"); return; }
    bc_index(cb, ib, io, it);
    if (!(dbg_draw() & 1) && g_ok) { int xf = xfb_begin(cb); vkCmdDrawIndexed(cb, (uint32_t)d->count, inst, 0, d->base_vertex, (uint32_t)d->base_instance); if (xf) xfb_end(cb, prim, (uint32_t)d->count, inst); }
}
static nvn_hmap g_cpipes = {.klen = 24};   // {shader, its integer-texel bindings}
static void dispatch(const p_dispatch *d) {
    vkshader_t *cs = S.sh[5];
    if (!cs) { VKRED_ONCE(60, "dispatch with no compute shader in the bundle: skipped"); return; }
    g_vkst.dispatches++;
    vk_end_pass_land();
    VkCommandBuffer cb = vk_cmd();
    VkDescriptorSetLayout dsl[5] = {cs->dsl}; VkPipelineLayout layout = vk_pipeline_layout(dsl, 1);
    uint32_t dofs[96]; VkDescriptorSet set = stage_set(5, cs, dofs);
    uint64_t ck[3] = {cs->key, g_num_um, g_num_im};
    VkPipeline p = nvn_hm_get(&g_cpipes, ck);
    if (!p) {
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, vk_shader_module(cs, ck[1], ck[2], 0, 0), "main", NULL}, layout, VK_NULL_HANDLE, -1};
        if (vkCreateComputePipelines(V.dev, V.pcache, 1, &ci, NULL, &p)) { LOG("[RED] [vk] compute pipeline failed: %016llx", (unsigned long long)cs->key); p = (VkPipeline)1; }
        nvn_hm_put(&g_cpipes, ck, p);
    }
    if (p == (void *)1) return;
    g_dispatch[0] = (uint32_t)d->x; g_dispatch[1] = (uint32_t)d->y; g_dispatch[2] = (uint32_t)d->z;
    vk_img_barrier_all(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, (uint32_t)cs->ndyn, dofs);
    if (d->ind) { VkBuffer b; VkDeviceSize o; if (vk_buf_at(d->ind, 12, &b, &o)) vkCmdDispatchIndirect(cb, b, o); }
    else vkCmdDispatch(cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    vk_img_barrier_all(cb);
    if (crumbs_on()) { crumb_t *c = crumb_cur(); if (c->n < 8) { c->k[c->n][0] = 0; c->k[c->n][1] = 0; c->k[c->n][2] = cs->key; c->n++; } crumb_mark(1); }
    if (g_x_tracepass == -2) VKLOG("--- dispatch %016llx %dx%dx%d%s after pass %d", (unsigned long long)cs->key, d->x, d->y, d->z, d->ind ? " (indirect)" : "", g_sub_pass - 1);   // (DM_VK_TRACE_PASS=-2 lists the dispatches too)
}

// ---------------- copies and clears ----------------
static VkImageSubresourceLayers sub_of(vkimg_t *im, const view_t *v, int has_v, int z) {
    uint32_t lv = has_v && v->set_levels ? v->base_level : 0, ly = has_v && v->set_layers ? v->min_layer : 0;
    int layered = im->layers > 1;
    return (VkImageSubresourceLayers){im->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT), lv, ly + (layered ? (uint32_t)z : 0), 1};
}
static void copy_b2t(const p_copy_b2t *c, int to_texture) {
    vk_end_pass_land();
    vkimg_t *im = vk_tex(c->tex); const vkfmt_t *f = vk_fmt(c->tex->d.format);
    if (!im || !f) return;
    VkBuffer b; VkDeviceSize o;
    if (!vk_buf_at(c->src, 1, &b, &o)) { VKRED_ONCE(70, "buffer of a buffer ↔ texture copy outside the memory pools"); return; }
    VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, im); vk_img_barrier_all(cb);
    unsigned bw = f->bw ? f->bw : 1, bh = f->bh ? f->bh : 1;
    uint32_t rowlen = S.row_stride > 0 && f->bpb ? (uint32_t)(S.row_stride / f->bpb) * bw : 0;
    uint32_t imgh = S.image_stride > 0 && S.row_stride > 0 ? (uint32_t)(S.image_stride / S.row_stride) * bh : 0;
    int layered = im->layers > 1, depth = c->r.depth ? c->r.depth : 1;
    VkBufferImageCopy r = {o, rowlen, imgh, sub_of(im, &c->view, c->has_view, c->r.zoffset), {c->r.xoffset, c->r.yoffset, layered ? 0 : c->r.zoffset},
                           {(uint32_t)c->r.width, (uint32_t)(c->r.height ? c->r.height : 1), layered ? 1 : (uint32_t)depth}};
    if (layered) r.imageSubresource.layerCount = (uint32_t)depth;
    if (g_watch_off >= 0) { char w[96]; snprintf(w, sizeof w, "buffer copy %s (buffer %#llx, %dx%d)", to_texture ? "destination" : "source", (unsigned long long)c->src, c->r.width, c->r.height); vk_watch(im, w); }
    if (to_texture) { vkCmdCopyBufferToImage(cb, b, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r); im->gpu_w = 1; }
    else { vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
        void vk_gpu_wrote(uint64_t a, uint64_t n); vkpool_t *p = vk_pool_of(c->src, 1); vk_gpu_wrote(c->src, p ? p->hi - c->src : 1); }   // (to the pool's end: the copy's extent in memory is not worked out here)
    vk_img_barrier_all(cb);
}
// Scaled copies of depth and stencil: Adreno blits no depth-stencil format (Pokémon Legends: Arceus copies its 1920x1080 D24S8 depth to a 480x270 one, which stayed unwritten), so each
// aspect goes image → buffer, through dm/android/shaders/depthscale.comp (the nearest texel), buffer → image. Two device buffers kept from copy to copy, grown when a region needs more
#include "depthscale_comp.spv.h"
static struct { VkPipeline p; VkPipelineLayout pl; VkDescriptorSetLayout dl; VkDescriptorPool dp; VkBuffer b[2]; VkDeviceMemory m[2]; VkDeviceSize n[2]; VkDescriptorSet set; int bad; } g_dsc;
static int dsc_init(void) {
    if (g_dsc.p || g_dsc.bad) return !g_dsc.bad;
    VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, lb};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 64, 1, &ps};
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof depthscale_comp_spv, depthscale_comp_spv}; VkShaderModule sm = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_dsc.dl) || vkCreateDescriptorPool(V.dev, &pi, NULL, &g_dsc.dp) || vkCreateShaderModule(V.dev, &mi, NULL, &sm)) { g_dsc.bad = 1; return 0; }
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_dsc.dl, 1, &pr};
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL}};
    if (vkCreatePipelineLayout(V.dev, &pli, NULL, &g_dsc.pl) || (ci.layout = g_dsc.pl, vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_dsc.p))) g_dsc.bad = 1;
    vkDestroyShaderModule(V.dev, sm, NULL);
    if (g_dsc.bad) VKLOG("[RED] the scaled depth copy's compute pipeline could not be made");
    return !g_dsc.bad;
}
static int dsc_buf(int i, VkDeviceSize n) {   // buffer i holds at least n bytes
    if (g_dsc.n[i] >= n) return 1;
    if (g_dsc.b[i]) vk_defer_free_buffer(g_dsc.b[i], g_dsc.m[i]);
    g_dsc.b[i] = VK_NULL_HANDLE; g_dsc.n[i] = 0; g_dsc.set = VK_NULL_HANDLE; n = n < (1u << 20) ? 1u << 20 : n + n / 2;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(V.dev, &bi, NULL, &g_dsc.b[i])) return 0;
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, g_dsc.b[i], &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (vkAllocateMemory(V.dev, &ai, NULL, &g_dsc.m[i])) { vkDestroyBuffer(V.dev, g_dsc.b[i], NULL); g_dsc.b[i] = VK_NULL_HANDLE; return 0; }
    vkBindBufferMemory(V.dev, g_dsc.b[i], g_dsc.m[i], 0); g_dsc.n[i] = n;
    return 1;
}
static int depth_scale(VkCommandBuffer cb, vkimg_t *s, vkimg_t *d, VkImageSubresourceLayers ss, VkImageSubresourceLayers ds, const p_copy_t2t *k) {   // 1: copied
    if (s->fmt != d->fmt || !(s->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) || s->aspect != d->aspect || !dsc_init()) return 0;
    uint32_t sw = (uint32_t)k->sr.width, sh = (uint32_t)k->sr.height, dw = (uint32_t)k->dr.width, dh = (uint32_t)k->dr.height;
    for (int a = 0; a < 2; a++) {
        VkImageAspectFlags as = a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT; if (!(s->aspect & as)) continue;
        uint32_t bytes = a ? 1 : s->fmt == VK_FORMAT_D16_UNORM ? 0 : 4; if (!bytes) return 0;   // (16-bit depth: two texels a word, not handled)
        if (!dsc_buf(0, ((VkDeviceSize)sw * sh * bytes + 3) & ~3ull) || !dsc_buf(1, ((VkDeviceSize)dw * dh * bytes + 3) & ~3ull)) return 0;
        if (!g_dsc.set) {
            VkDescriptorSetAllocateInfo sa = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dsc.dp, 1, &g_dsc.dl};
            if (vkAllocateDescriptorSets(V.dev, &sa, &g_dsc.set)) { g_dsc.set = VK_NULL_HANDLE; VKRED_ONCE(76, "scaled depth copy: no descriptor set left"); return 0; }
            VkDescriptorBufferInfo bi[2] = {{g_dsc.b[0], 0, VK_WHOLE_SIZE}, {g_dsc.b[1], 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_dsc.set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[0], NULL},
                                         {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_dsc.set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[1], NULL}};
            vkUpdateDescriptorSets(V.dev, 2, w, 0, NULL);
        }
        VkImageSubresourceLayers sl = ss, dl = ds; sl.aspectMask = as; dl.aspectMask = as;
        VkBufferImageCopy r = {0, 0, 0, sl, {k->sr.xoffset, k->sr.yoffset, 0}, {sw, sh, 1}};
        vkCmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, g_dsc.b[0], 1, &r); vk_img_barrier_all(cb);
        uint32_t pc[5] = {sw, sh, dw, dh, bytes}, n = bytes == 4 ? dw * dh : (dw * dh + 3) / 4, groups = (n + 63) / 64, gx = groups < 65535 ? groups : 65535, gy = (groups + gx - 1) / gx;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_dsc.p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_dsc.pl, 0, 1, &g_dsc.set, 0, NULL);
        vkCmdPushConstants(cb, g_dsc.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cb, gx, gy, 1); vk_img_barrier_all(cb);
        VkBufferImageCopy w = {0, 0, 0, dl, {k->dr.xoffset, k->dr.yoffset, 0}, {dw, dh, 1}};
        vkCmdCopyBufferToImage(cb, g_dsc.b[1], d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &w); vk_img_barrier_all(cb);
    }
    S.bound_pipe = VK_NULL_HANDLE;   // the compute pipeline took the bind point the replay's dispatches use
    return 1;
}
static void copy_t2t(const p_copy_t2t *c) {
    vk_end_pass();
    vkimg_t *s = vk_tex(c->src), *d = vk_tex(c->dst);
    if (!s || !d) return;
    vk_watch(s, "copy source"); vk_watch(d, "copy destination");
    VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, s); vk_tex_prepare(cb, d); vk_img_barrier_all(cb);
    VkImageSubresourceLayers ss = sub_of(s, &c->sv, c->has_sv, c->sr.zoffset), ds = sub_of(d, &c->dv, c->has_dv, c->dr.zoffset);
    // Vulkan refuses (vkEndCommandBuffer fails for the whole submission) a region outside either image or a blit the format cannot do: regions are clipped
    // to both images' levels, a copy with nothing left is skipped
    int32_t sw = (int32_t)(s->w >> ss.mipLevel ? s->w >> ss.mipLevel : 1), sh = (int32_t)(s->h >> ss.mipLevel ? s->h >> ss.mipLevel : 1);
    int32_t dw = (int32_t)(d->w >> ds.mipLevel ? d->w >> ds.mipLevel : 1), dh = (int32_t)(d->h >> ds.mipLevel ? d->h >> ds.mipLevel : 1);
    p_copy_t2t k = *c; c = &k; d->gpu_w = 1;
    if (k.sr.height <= 0) k.sr.height = 1; if (k.dr.height <= 0) k.dr.height = 1;
    if (ss.mipLevel >= s->levels || ds.mipLevel >= d->levels || k.sr.xoffset < 0 || k.sr.yoffset < 0 || k.dr.xoffset < 0 || k.dr.yoffset < 0 || k.sr.xoffset >= sw || k.sr.yoffset >= sh || k.dr.xoffset >= dw || k.dr.yoffset >= dh) {
        VKRED_ONCE(72, "texture copy outside its images (src %dx%d @%d,%d of %dx%d level %u, dst %dx%d @%d,%d of %dx%d level %u): skipped", c->sr.width, c->sr.height, c->sr.xoffset, c->sr.yoffset, sw, sh, ss.mipLevel,
                   c->dr.width, c->dr.height, c->dr.xoffset, c->dr.yoffset, dw, dh, ds.mipLevel); vk_img_barrier_all(cb); return; }
    int same = k.sr.width == k.dr.width && k.sr.height == k.dr.height;
    if (k.sr.xoffset + k.sr.width > sw) { VKRED_ONCE(73, "texture copy source region clipped to its image"); k.sr.width = sw - k.sr.xoffset; if (same) k.dr.width = k.sr.width; }
    if (k.sr.yoffset + k.sr.height > sh) { VKRED_ONCE(73, "texture copy source region clipped to its image"); k.sr.height = sh - k.sr.yoffset; if (same) k.dr.height = k.sr.height; }
    if (k.dr.xoffset + k.dr.width > dw) { VKRED_ONCE(74, "texture copy destination region clipped to its image"); k.dr.width = dw - k.dr.xoffset; if (same) k.sr.width = k.dr.width; }
    if (k.dr.yoffset + k.dr.height > dh) { VKRED_ONCE(74, "texture copy destination region clipped to its image"); k.dr.height = dh - k.dr.yoffset; if (same) k.sr.height = k.dr.height; }
    if (k.sr.width <= 0 || k.sr.height <= 0 || k.dr.width <= 0 || k.dr.height <= 0) { vk_img_barrier_all(cb); return; }
    if (!(c->flags & DM_COPY_RESOLVE) && !(k.sr.width == k.dr.width && k.sr.height == k.dr.height && s->fmt == d->fmt)) {   // a blit: both formats must allow it (and linear filtering)
        VkFormatProperties sp, dp; vkGetPhysicalDeviceFormatProperties(V.pd, s->fmt, &sp); vkGetPhysicalDeviceFormatProperties(V.pd, d->fmt, &dp);
        if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(dp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) || (s->aspect != d->aspect)) {
            if (depth_scale(cb, s, d, ss, ds, &k)) return;
            VKRED_ONCE(75, "texture blit between formats %d → %d that cannot be blitted (%dx%d → %dx%d, present %llu): skipped", s->fmt, d->fmt, k.sr.width, k.sr.height, k.dr.width, k.dr.height, (unsigned long long)g_tex_frame); vk_img_barrier_all(cb); return; }
        if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) || (s->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) k.flags &= ~1u;   // nearest where linear is not allowed
    }
    if (s == d && !memcmp(&ss, &ds, sizeof ss) && k.sr.xoffset == k.dr.xoffset && k.sr.yoffset == k.dr.yoffset && k.sr.width == k.dr.width && k.sr.height == k.dr.height && k.sr.zoffset == k.dr.zoffset) {
        vk_img_barrier_all(cb); return; }   // a copy of a region onto itself (two textures over the same storage): nothing to do, and Vulkan forbids overlapping copies
    if (c->flags & DM_COPY_RESOLVE) {
        VkImageResolve r = {ss, {c->sr.xoffset, c->sr.yoffset, 0}, ds, {c->dr.xoffset, c->dr.yoffset, 0}, {(uint32_t)c->sr.width, (uint32_t)c->sr.height, 1}};
        vkCmdResolveImage(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else if (c->sr.width == c->dr.width && c->sr.height == c->dr.height && s->fmt == d->fmt) {
        VkImageCopy r = {ss, {c->sr.xoffset, c->sr.yoffset, s->layers > 1 ? 0 : c->sr.zoffset}, ds, {c->dr.xoffset, c->dr.yoffset, d->layers > 1 ? 0 : c->dr.zoffset},
                         {(uint32_t)c->sr.width, (uint32_t)(c->sr.height ? c->sr.height : 1), s->layers > 1 ? 1 : (uint32_t)(c->sr.depth ? c->sr.depth : 1)}};
        vkCmdCopyImage(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
        VkImageBlit r = {ss, {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + (c->sr.height ? c->sr.height : 1), 1}},
                         ds, {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + (c->dr.height ? c->dr.height : 1), 1}}};
        vkCmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r, (c->flags & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    vk_img_barrier_all(cb);
}
static void clear_tex(const p_clear_tex *c) {
    vk_end_pass();
    vkimg_t *im = vk_tex(c->tex); if (!im) return;
    VkCommandBuffer cb = vk_cmd(); vk_tex_prepare(cb, im); vk_img_barrier_all(cb); im->gpu_w = 1;
    VkImageSubresourceLayers l = sub_of(im, &c->view, c->has_view, c->r.zoffset);
    VkImageSubresourceRange rg = {l.aspectMask, l.mipLevel, 1, l.baseArrayLayer, im->layers > 1 ? (uint32_t)(c->r.depth ? c->r.depth : 1) : 1};
    if (c->r.xoffset || c->r.yoffset || (uint32_t)c->r.width < (im->w >> l.mipLevel) || (uint32_t)c->r.height < (im->h >> l.mipLevel)) VKRED_ONCE(71, "partial texture clear: the whole level is cleared");
    if (im->aspect & VK_IMAGE_ASPECT_COLOR_BIT) { VkClearColorValue v; memcpy(&v, c->c.u, 16); vkCmdClearColorImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rg); }
    else { VkClearDepthStencilValue v = {c->c.f[0], c->c.u[1]}; rg.aspectMask = im->aspect; vkCmdClearDepthStencilImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rg); }
    vk_img_barrier_all(cb);
}

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
        if (op == OP_BIND_TEX || op == OP_BIND_TEX1) { if (slot >= 0 && slot < 256) d = &S.tex[st][slot]; }
        else if (op == OP_BIND_SAMP) { if (slot >= 0 && slot < 128) d = &S.samp[st][slot]; }
        else if (op == OP_BIND_IMG) { if (slot >= 0 && slot < 32) d = &S.img[st][slot]; }
        if (d && *d != b->h[i]) { *d = b->h[i]; g_bind_gen[st]++; }   // only a change counts: games bind the same handles again before many draws (stage_set's memo)
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
void mtl_exec(const uint8_t *p) {   // timed at the top level only (OP_CALL nests)
    static int depth; uint64_t t0 = depth ? 0 : tnow(); depth++; if (depth == 1) prof_tick();
    exec_ops(p);
    if (!--depth) g_vkst.ns_exec += tnow() - t0;
}
static void exec_ops(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.op < 64) g_sub_ops[h.op]++;
        static uint64_t skip = ~0ull; if (skip == ~0ull) { skip = 0; const char *e = getenv("DM_VK_SKIP_OPS"); for (const char *q = e; q && *q;) { char *n; long v = strtol(q, &n, 10); if (n == q) break; if (v > 0 && v < 64) skip |= 1ull << v; q = *n ? n + 1 : n; } }   // debugging: record ops not replayed
        if (h.op < 64 && h.op > 3 && (skip >> h.op & 1)) { p += h.size; continue; }
        const uint8_t *a = p + 8;
        switch (h.op) {
        case OP_END: return;
        case OP_BEGIN: break;
        case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); exec_ops((const uint8_t *)(uintptr_t)t); break; }
        case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
        case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); if (DM_GAME_RENDER_COND) rc_note(ad, t == 0 || t == 0x10 ? 1 : 2); mtl_sub_counter(ad, (uint32_t)t); break; }
        case OP_RESET_COUNTER: break;
        case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = mtl_last_fence() + 1; s->fenced = 1; break; }
        case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s_, d, n); break; }
        case OP_CLEAR_BUF: { const p_clear_buf *c = (const p_clear_buf *)a; mtl_sub_fill(c->addr, c->size, c->value); break; }
        case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); vk_sub_update(d, a + 16, n); break; }
        case OP_SET_RT: set_rt((const p_set_rt *)a); break;
        case OP_CLEAR_COLOR: if (!g_rc_skip) clear_color((const p_clear_color *)a); break;
        case OP_CLEAR_DS: if (!g_rc_skip) clear_ds((const p_clear_ds *)a); break;
        case OP_VIEWPORTS: { const p_viewports *v = (const p_viewports *)a; for (int i = 0; i < v->n && v->first + i < 16; i++) memcpy(S.vp[v->first + i], v->v[i], 16); break; }
        case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.sc_set = 1; break;
        case OP_DEPTH_RANGE: { const p_depth_range *d = (const p_depth_range *)a; S.dnear = d->n; S.dfar = d->f; break; }
        case OP_BIND_PROGRAM: bind_program((const p_bind_program *)a); break;
        case OP_BIND_STATE: { const p_bind_state *s = (const p_bind_state *)a;
            switch (s->kind) {
            case 0: { blend_st b; memcpy(&b, s->bytes, sizeof b); if (b.target < 8) S.blend[b.target] = b; break; }
            case 1: memcpy(&S.color, s->bytes, sizeof S.color); break;
            case 2: memcpy(&S.chmask, s->bytes, sizeof S.chmask); break;
            case 3: memcpy(&S.ms, s->bytes, sizeof S.ms); break;
            case 4: memcpy(&S.poly, s->bytes, sizeof S.poly); break;
            case 5: memcpy(&S.ds, s->bytes, sizeof S.ds); break;
            }
            break; }
        case OP_BIND_VATTR: { const p_bind_vattr *v = (const p_bind_vattr *)a; memset(S.vattr, 0, sizeof S.vattr); for (int i = 0; i < v->n && i < 16; i++) S.vattr[v->first + i] = v->st[i]; break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *v = (const p_bind_vstream *)a; for (int i = 0; i < v->n && v->first + i < 16; i++) S.vstream[v->first + i] = v->st[i]; break; }
        case OP_BIND_VBUF: { const p_bind_vbuf *v = (const p_bind_vbuf *)a; for (int i = 0; i < v->n && v->first + i < 16; i++) S.vbuf[v->first + i] = v->r[i]; break; }
        case OP_BIND_UBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16 && memcmp(&S.ubo[b->stage][b->index], b, sizeof *b)) { S.ubo[b->stage][b->index] = *b; g_ubo_gen[b->stage]++; } break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16 && memcmp(&S.ssbo[b->stage][b->index], b, sizeof *b)) { S.ssbo[b->stage][b->index] = *b; g_bind_gen[b->stage]++; } break; }
        case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_BIND_TEX1: bind_handles_rec(h.op, a); break;
        case OP_STENCIL: { const p_stencil *s = (const p_stencil *)a;
            for (int f = 0; f < 2; f++) if (s->face & (1 << f)) { if (s->which == 0) S.smask[f] = s->value_mask; else if (s->which == 1) S.swmask[f] = s->mask; else S.sref[f] = s->ref; }
            break; }
        case OP_POLY_OFFSET: S.pofs = *(const p_poly_offset *)a; break;
        case OP_BLEND_COLOR: memcpy(S.blendc, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_depth_clamp *)a)->on; break;
        case OP_RAST_DISCARD: S.rast_discard = ((const p_rast_discard *)a)->on; break;
        case OP_PRIM_RESTART: S.prim_restart = ((const p_prim_restart *)a)->on; break;
        case OP_DRAW: { if (g_rc_skip) break; p_draw t; draw(draw_rec(a, h.size, &t)); break; }
        case OP_DRAW_IDX: { if (g_rc_skip) break; p_draw_idx t; draw_idx(draw_idx_rec(a, h.size, &t)); break; }
        case OP_DISPATCH: dispatch((const p_dispatch *)a); break;
        case OP_BARRIER:   // the pass ends at every barrier: kept across XC3's fragment- and primitive-order barriers (0x1 0x2 0x4) its characters drew torn in some frames
            if (g_x_listpasses) VKLOG("    barrier %#x", *(const int32_t *)a);
            vk_end_pass(); vk_img_barrier_all(vk_cmd()); break;
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;   // the handles were rebased when recorded (hle_nvn.c bind_handles)
        case OP_COPY_STRIDE: { const p_copy_stride *c = (const p_copy_stride *)a; if (c->image) S.image_stride = c->stride; else S.row_stride = c->stride; break; }
        case OP_COPY_B2T: copy_b2t((const p_copy_b2t *)a, 1); break;
        case OP_COPY_T2B: copy_b2t((const p_copy_b2t *)a, 0); break;
        case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
        case OP_CLEAR_TEX: clear_tex((const p_clear_tex *)a); break;
        case OP_RENDER_COND: render_cond((const p_render_cond *)a); break;   // recorded only with the profile key render_cond (hle_nvn.c)
        case OP_DRAW_TF: if (!g_rc_skip) draw_tf((const p_draw_tf *)a); break;
        case OP_TF_BIND: { const p_tf_bind *b = (const p_tf_bind *)a; if (b->index >= 0 && b->index < 4) g_tf[b->index] = *b; break; }
        case OP_TF_STATE: if (!V.has_xfb) VKRED_ONCE(80, "transform feedback: the device has none, its draws write nothing"); tf_state((const p_tf_state *)a); break;
        case OP_VP_SWIZZLE: { const p_vp_swizzle *b = (const p_vp_swizzle *)a; memcpy(S.vp_sw[b->first], b->sw, 4 * (size_t)b->n); break; }
        default: VKRED_ONCE(82, "unknown record op %u", h.op); break;
        }
        p += h.size;
    }
}
