// Android port, Vulkan backend: presentation (see gfx_vk.h).
// With a window (the app's ANativeWindow, dm_front_window): the game's swap chain texture is scaled into a Vulkan swapchain image (aspect kept, black borders) and
// presented with FIFO (vsync) pacing. Headless (dmhost from adb shell): frames are counted, and DM_VK_SHOT=n1,n2,... writes those presented frames as PPM files to
// DM_VK_SHOT_DIR (default DM_ROOT), which is how a run without a screen is checked.
#include "gfx_vk.h"
#include <android/native_window.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "overlay_layout.h"
#include "font5x7.h"
#include "overlay_vert.spv.h"
#include "overlay_frag.spv.h"

__attribute__((weak)) ANativeWindow *dm_front_window(void) { return NULL; }
static uint64_t g_presents;
// Vulkan present IDs include overlay redraws; only the game's tickets have a time for the NVN frame statistics.
enum { PRES_TIMES = 64 };
static struct { uint64_t ticket, ns; } g_pres_t[PRES_TIMES];
static struct { uint32_t id; uint64_t ticket; } g_pres_ids[PRES_TIMES];
static pthread_mutex_t g_pres_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pres_recorded;   // display times recorded so far (present_time_record): none after 240 presents with display timing on, the hand-over time stands in
static void present_time_begin(uint64_t ticket, uint64_t ns) {   // ns: the time to report now (no display times on this device), or 0 for the display's
    pthread_mutex_lock(&g_pres_mu);
    g_pres_t[ticket % PRES_TIMES] = (typeof(g_pres_t[0])){ticket, ns};
    pthread_mutex_unlock(&g_pres_mu);
}
static void present_time_map(uint32_t id, uint64_t ticket) {   // the present thread, under g_present_mu
    g_pres_ids[id % PRES_TIMES] = (typeof(g_pres_ids[0])){id, ticket};
}
static void present_time_record(uint32_t id, uint64_t ns) {   // the present thread, under g_present_mu
    typeof(g_pres_ids[0]) *m = &g_pres_ids[id % PRES_TIMES];
    if (m->id != id || !m->ticket || !ns) return;
    pthread_mutex_lock(&g_pres_mu);
    typeof(g_pres_t[0]) *p = &g_pres_t[m->ticket % PRES_TIMES];
    if (p->ticket == m->ticket) p->ns = ns;
    pthread_mutex_unlock(&g_pres_mu); g_pres_recorded++;
}
static void present_time_reset(void) {   // the old swapchain's pending results cannot describe the new one
    memset(g_pres_ids, 0, sizeof g_pres_ids);
    pthread_mutex_lock(&g_pres_mu); memset(g_pres_t, 0, sizeof g_pres_t); pthread_mutex_unlock(&g_pres_mu);
}
static double present_time_get(uint64_t ticket) {
    pthread_mutex_lock(&g_pres_mu);
    typeof(g_pres_t[0]) *p = &g_pres_t[ticket % PRES_TIMES];
    double t = ticket && p->ticket == ticket ? p->ns / 1e9 : 0;
    pthread_mutex_unlock(&g_pres_mu);
    return t;
}
static VkSurfaceKHR g_surf; static VkSwapchainKHR g_swap; static VkImage g_simg[8]; static uint32_t g_nsimg; static VkExtent2D g_sext; static VkFormat g_sfmt;
// The swapchain's format follows the window texture's sRGB-ness: a blit from an sRGB image into a UNORM one decodes to linear and stores that unencoded, which darkened the whole
// picture (Donkey Kong Country Returns HD presents RGBA8_SRGB, NVN format 0x38; issue #4). The Switch scans the window texture's bytes out as they are, and an SRGB -> SRGB blit
// decodes and encodes them again. UNORM games keep the UNORM swapchain. The overlay drawn after the blit is encoded too in an sRGB swapchain, so it shows a little lighter there
static VkFormat g_want = VK_FORMAT_R8G8B8A8_UNORM;
static VkSemaphore g_acq[8], g_done_sem[8]; static VkFence g_pfence[8]; static VkCommandPool g_ppool; static VkCommandBuffer g_pcb[8]; static uint32_t g_pi;
static ANativeWindow *g_win;
static VkImageView g_sview[8];
// Where the game's picture lies in the window (fractions: x, y, width, height; aspect kept, black borders around it), for the front end's touch points, which hle_touch.c
// takes relative to the picture: normalized by the whole window, a tap in Unpacking's 3040x1904 window (97 px borders) was off by up to 5% of the height
static _Atomic uint32_t g_pic[4] = {0, 0, 1u << 20, 1u << 20};
static void vk_pic_set(float x, float y, float w, float h) { atomic_store(&g_pic[0], (uint32_t)(x * (1 << 20))); atomic_store(&g_pic[1], (uint32_t)(y * (1 << 20))); atomic_store(&g_pic[2], (uint32_t)(w * (1 << 20))); atomic_store(&g_pic[3], (uint32_t)(h * (1 << 20))); }
void dm_picture_rect(float r[4]) { for (int i = 0; i < 4; i++) r[i] = (float)atomic_load(&g_pic[i]) / (1 << 20); }
__attribute__((weak)) int dm_overlay_visible(void) { return 0; }
__attribute__((weak)) float dm_overlay_scale(void) { return 1; }
__attribute__((weak)) void dm_overlay_knob(int stick, float *dx, float *dy) { (void)stick; *dx = *dy = 0; }
__attribute__((weak)) int64_t dm_overlay_pressed(int ctl, uint64_t bit) { (void)ctl; (void)bit; return 0; }
__attribute__((weak)) int dm_menu_state(void) { return -1; }
__attribute__((weak)) int dm_overlay_fps(void) { return 0; }   // front_android.c: the pause menu's Show FPS
__attribute__((weak)) int dm_overlay_custom(int layout, int ctl, float *x, float *y, float *s) { (void)layout; (void)ctl; (void)x; (void)y; *s = 1; return 0; }
__attribute__((weak)) int dm_overlay_custom_ver(void) { return 0; }
__attribute__((weak)) float dm_overlay_opacity(void) { return 0.6f; }
__attribute__((weak)) int dm_overlay_layout(void) { return 0; }
__attribute__((weak)) int dm_text_entry(char *title, int tcap, char *text, int xcap, int *sel, int *shift) { (void)title; (void)tcap; (void)text; (void)xcap; (void)sel; (void)shift; return 0; }
static void pace_reset(void); static void ov_fb_drop(void);
static void swap_destroy(void) {
    pace_reset();
    present_time_reset();
    if (g_swap) { vkDeviceWaitIdle(V.dev); ov_fb_drop(); for (uint32_t i = 0; i < g_nsimg; i++) if (g_sview[i]) { vkDestroyImageView(V.dev, g_sview[i], NULL); g_sview[i] = VK_NULL_HANDLE; }
                  vkDestroySwapchainKHR(V.dev, g_swap, NULL); g_swap = VK_NULL_HANDLE; }
    if (g_surf) { vkDestroySurfaceKHR(V.inst, g_surf, NULL); g_surf = VK_NULL_HANDLE; }
}
static int swap_make(void) {
    ANativeWindow *w = dm_front_window();
    if (w != g_win) { swap_destroy(); g_win = w; }
    if (!w) return 0;
    if (g_swap) return 1;
    VkAndroidSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, w};
    if (vkCreateAndroidSurfaceKHR(V.inst, &si, NULL, &g_surf)) return 0;
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.pd, g_surf, &caps);
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, g_surf, &nf, NULL); VkSurfaceFormatKHR sf[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, g_surf, &nf, sf);
    g_sfmt = sf[0].format; for (uint32_t i = 0; i < nf; i++) if (sf[i].format == VK_FORMAT_R8G8B8A8_UNORM) g_sfmt = sf[i].format;
    for (uint32_t i = 0; i < nf; i++) if (sf[i].format == g_want) g_sfmt = g_want;   // the window texture's sRGB-ness (g_want), so the blit copies its bytes as they are
    if (g_sfmt != g_want) VKRED_ONCE(95, "the surface offers no format %d for the window texture: its picture is %s", g_want, g_want == VK_FORMAT_R8G8B8A8_SRGB ? "darker" : "lighter");
    g_sext = (VkExtent2D){(uint32_t)ANativeWindow_getWidth(w), (uint32_t)ANativeWindow_getHeight(w)};   // the window as it is now; Android's compositor rotates (identity transform)
    if (!g_sext.width || !g_sext.height) g_sext = caps.currentExtent;
    uint32_t n = caps.minImageCount < 3 ? 3 : caps.minImageCount; if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, g_surf, n, g_sfmt, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, g_sext, 1,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, 1, VK_NULL_HANDLE};
    VkResult r = vkCreateSwapchainKHR(V.dev, &ci, NULL, &g_swap);
    if (r) { VKLOG("[RED] swapchain creation failed: %d", r); swap_destroy(); return 0; }
    g_nsimg = 8; vkGetSwapchainImagesKHR(V.dev, g_swap, &g_nsimg, g_simg);
    for (uint32_t i = 0; i < g_nsimg; i++) { VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_simg[i], VK_IMAGE_VIEW_TYPE_2D, g_sfmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKCHK(vkCreateImageView(V.dev, &vi, NULL, &g_sview[i])); }
    if (!g_ppool) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.qf}; VKCHK(vkCreateCommandPool(V.dev, &pci, NULL, &g_ppool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_ppool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 8}; VKCHK(vkAllocateCommandBuffers(V.dev, &ai, g_pcb));
        for (int i = 0; i < 8; i++) {
            VkSemaphoreCreateInfo s = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKCHK(vkCreateSemaphore(V.dev, &s, NULL, &g_acq[i])); VKCHK(vkCreateSemaphore(V.dev, &s, NULL, &g_done_sem[i]));
            VkFenceCreateInfo f = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT}; VKCHK(vkCreateFence(V.dev, &f, NULL, &g_pfence[i]));
        }
    }
    VKLOG("swapchain %ux%u format %d, %u images", g_sext.width, g_sext.height, g_sfmt, g_nsimg);
    return 1;
}
static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
// ---- the on-screen gamepad: rasterized on the CPU once per window size (half resolution), blended over each frame ----
// The texture is the static layer (w x h), a strip of sprites under it (sh rows) and the pause menu's panel under that (pw x ph). The sprites: each stick's
// knob, drawn every frame where the finger holds it; the menu button, drawn on its own at OV_MENU_ALPHA whether the gamepad is shown or not; a white disc, the
// glow over a pressed round button (a wider one glows in its own shape); a black block, the dimming behind the menu. sp: each sprite in the strip (centre x, y,
// half width and height, texels; 0 = none). The panel is rasterized again whenever the menu's state (dm_menu_state) changes. Under the panel, the frame rate
// counter's characters (OV_FPS_CHARS, gh rows: one cell each), from which the counter is put together on every present, so a changing number needs no upload.
// With a theme in the APK (overlay_theme.c), the gamepad is its sprites instead: an atlas per layout in a texture of its own (g_tt), uploaded once, every
// control drawn from it on each present; the procedural layer is then only the menu's panel, the dimming and the counter.
enum { SP_DISC = OV_N, SP_BLACK, SP_WHITE, SP_ACCENT, SP_N };   // (black, white, accent: solid blocks, the dimming and the layout editor's frames)
#define OV_FPS_CHARS "0123456789 FPS-"
static struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler samp; VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkDescriptorPool pool; VkDescriptorSet set;
                VkPipeline pipe; VkFormat pfmt; uint32_t w, h, sh, pw, ph, gh, gs; VkBuffer up; VkDeviceMemory upm; uint8_t *px; int uploaded, panel, lay, cver; float sp[SP_N][4];
                VkRenderPass rp; VkFramebuffer fb[8]; VkImageView fbview[8]; VkExtent2D fbext[8]; } g_ov;
static struct { const ov_theme *t; VkImage img; VkDeviceMemory mem; VkImageView view; VkDescriptorSet set; VkBuffer up; VkDeviceMemory upm; uint32_t upk; int ready; } g_tt[2];   // per layout
// Without dynamic rendering (a Vulkan 1.1 / 1.2 device, V.has_dynrender) the overlay draws in a render pass object over the swapchain image: one pass for the swapchain's
// format (load and store, the image already in COLOR_ATTACHMENT_OPTIMAL), and a framebuffer per swapchain image, made again when its view or the extent changes
static VkRenderPass ov_rp(void) {
    static VkFormat fmt; if (g_ov.rp && fmt == g_sfmt) return g_ov.rp;
    if (g_ov.rp) { vkDestroyRenderPass(V.dev, g_ov.rp, NULL); g_ov.rp = VK_NULL_HANDLE; }   // the swapchain's format changed (g_want); swap_destroy dropped its framebuffers
    fmt = g_sfmt;
    VkAttachmentDescription a = {0, g_sfmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference r = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &r, NULL, NULL, 0, NULL};
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &a, 1, &sp, 0, NULL};
    if (vkCreateRenderPass(V.dev, &ci, NULL, &g_ov.rp)) { VKRED_ONCE(93, "overlay render pass failed"); g_ov.rp = VK_NULL_HANDLE; }
    return g_ov.rp;
}
// every overlay framebuffer goes with the views or the pass it was made for: a view handle made again later may have the same value (ov_fb would take it for its own)
static void ov_fb_drop(void) { for (int i = 0; i < 8; i++) { if (g_ov.fb[i]) vkDestroyFramebuffer(V.dev, g_ov.fb[i], NULL); g_ov.fb[i] = VK_NULL_HANDLE; g_ov.fbview[i] = VK_NULL_HANDLE; g_ov.fbext[i] = (VkExtent2D){0, 0}; } }
static VkFramebuffer ov_fb(uint32_t idx) {
    if (idx >= 8 || !ov_rp()) return VK_NULL_HANDLE;
    if (g_ov.fb[idx] && g_ov.fbview[idx] == g_sview[idx] && g_ov.fbext[idx].width == g_sext.width && g_ov.fbext[idx].height == g_sext.height) return g_ov.fb[idx];
    if (g_ov.fb[idx]) vkDestroyFramebuffer(V.dev, g_ov.fb[idx], NULL);   // (swap_destroy drops them all with the views; here only an extent change)
    VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_ov.rp, 1, &g_sview[idx], g_sext.width, g_sext.height, 1};
    if (vkCreateFramebuffer(V.dev, &ci, NULL, &g_ov.fb[idx])) { VKRED_ONCE(94, "overlay framebuffer failed"); g_ov.fb[idx] = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    g_ov.fbview[idx] = g_sview[idx]; g_ov.fbext[idx] = g_sext;
    return g_ov.fb[idx];
}
static _Atomic uint64_t g_fps_frames;   // the game's frames shown (mtl_present), which the counter counts
static int ov_round(const ov_ctl *c) { return c->w == c->h; }
static uint32_t ov_layout_sprites(uint32_t w, uint32_t h) {   // places the sprites, returns the strip's height
    float x = 4, top = (float)h, mr = 0, disc = 0, b[4];
    for (int i = 0; i < OV_N; i++) { ov_box(&OV_CTLS[i], (float)w, (float)h, b); if (OV_CTLS[i].kind == OV_BUTTON && ov_round(&OV_CTLS[i]) && b[2] > disc) disc = b[2]; }
    for (int i = 0; i < SP_N; i++) {
        const ov_ctl *c = i < OV_N ? &OV_CTLS[i] : NULL; float hw = 0, hh = 0;
        if (!c) hw = hh = i == SP_DISC ? disc : 6;
        else { ov_box(c, (float)w, (float)h, b);
            if (c->kind == OV_STICK_L || c->kind == OV_STICK_R) hw = hh = b[2] * OV_KNOB;
            else if (c->kind == OV_MENU || (c->kind == OV_BUTTON && !ov_round(c))) { hw = b[2]; hh = b[3]; } }   // a wider button's glow is its own shape
        g_ov.sp[i][2] = hw; g_ov.sp[i][3] = hh; if (!hw) continue; g_ov.sp[i][0] = x + hw + 2; x += 2 * hw + 8; if (hh > mr) mr = hh;
    }
    for (int i = 0; i < SP_N; i++) g_ov.sp[i][1] = top + mr + 4;
    return (uint32_t)(2 * mr + 8);
}
static void ov_plot(uint8_t *px, uint32_t w, uint32_t h, int x, int y, uint8_t a, uint8_t v) {
    if (x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h) return;
    uint8_t *p = px + 4 * ((size_t)y * w + (size_t)x); if (p[3] >= a) return;
    p[0] = p[1] = p[2] = v; p[3] = a;
}
static void ov_circle(uint8_t *px, uint32_t w, uint32_t h, float cx, float cy, float r, float ring) {
    for (int y = (int)(cy - r - 2); y <= (int)(cy + r + 2); y++) for (int x = (int)(cx - r - 2); x <= (int)(cx + r + 2); x++) {
        float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
        if (d <= r && d >= r - ring) ov_plot(px, w, h, x, y, 150, 255);   // the ring
        else if (d < r - ring) ov_plot(px, w, h, x, y, 50, 30);           // a dark, see-through inside
    }
}
static void ov_box_ring(uint8_t *px, uint32_t w, uint32_t h, const ov_ctl *c, const float b[4], float ring) {   // ov_circle for any control's shape (b: ov_box)
    for (int y = (int)(b[1] - b[3] - 2); y <= (int)(b[1] + b[3] + 2); y++) for (int x = (int)(b[0] - b[2] - 2); x <= (int)(b[0] + b[2] + 2); x++) {
        float d = ov_sdf(c, b, x - b[0], y - b[1]);
        if (d <= 0 && d >= -ring) ov_plot(px, w, h, x, y, 150, 255);
        else if (d < -ring) ov_plot(px, w, h, x, y, 50, 30);
    }
}
static void ov_text(uint8_t *px, uint32_t w, uint32_t h, float cx, float cy, const char *s, int scale) {
    int n = (int)strlen(s), x0 = (int)(cx - n * 6 * scale / 2.0f), y0 = (int)(cy - 7 * scale / 2.0f);
    for (int i = 0; i < n; i++) {
        unsigned c = (unsigned char)s[i]; const unsigned char *g = FONT5X7[(c >= 32 && c < 127 ? c : 63) - 32];
        for (int col = 0; col < 5; col++) for (int row = 0; row < 7; row++) if (g[col] >> row & 1)
            for (int dy = 0; dy < scale; dy++) for (int dx = 0; dx < scale; dx++) ov_plot(px, w, h, x0 + i * 6 * scale + col * scale + dx, y0 + row * scale + dy, 220, 255);
    }
}
// colour drawing for the menu button and the panel: straight alpha, "over" blending, shapes antialiased by their distance
typedef struct { uint8_t *px; uint32_t stride, x0, y0, w, h; } ov_canvas;   // a w x h window of rows `stride` texels wide, at (x0, y0)
static void cv_blend(const ov_canvas *cv, int x, int y, const uint8_t c[4], float cov) {
    if (x < 0 || y < 0 || (uint32_t)x >= cv->w || (uint32_t)y >= cv->h || cov <= 0) return;
    uint8_t *p = cv->px + 4 * ((size_t)(cv->y0 + y) * cv->stride + cv->x0 + x);
    float a = c[3] / 255.0f * (cov > 1 ? 1 : cov), da = p[3] / 255.0f, oa = a + da * (1 - a);
    if (oa <= 0) return;
    for (int k = 0; k < 3; k++) p[k] = (uint8_t)((c[k] * a + p[k] * da * (1 - a)) / oa + 0.5f);
    p[3] = (uint8_t)(oa * 255 + 0.5f);
}
static void cv_rrect(const ov_canvas *cv, float x0, float y0, float x1, float y1, float rad, const uint8_t c[4]) {   // a filled rounded rectangle
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - rad, hy = (y1 - y0) / 2 - rad;
    for (int y = (int)y0 - 1; y <= (int)y1 + 1; y++) for (int x = (int)x0 - 1; x <= (int)x1 + 1; x++) {
        float qx = fabsf(x + 0.5f - cx) - hx, qy = fabsf(y + 0.5f - cy) - hy, ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
        float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - rad;
        cv_blend(cv, x, y, c, 0.5f - d);
    }
}
static void cv_disc(const ov_canvas *cv, float cx, float cy, float r, const uint8_t c[4]) { cv_rrect(cv, cx - r, cy - r, cx + r, cy + r, r, c); }
static void cv_box(const ov_canvas *cv, const ov_ctl *c, const float b[4], float inset, const uint8_t col[4]) {   // a control's shape filled (b: ov_box), inset texels in
    for (int y = (int)(b[1] - b[3]) - 1; y <= (int)(b[1] + b[3]) + 1; y++) for (int x = (int)(b[0] - b[2]) - 1; x <= (int)(b[0] + b[2]) + 1; x++)
        cv_blend(cv, x, y, col, 0.5f - (ov_sdf(c, b, x + 0.5f - b[0], y + 0.5f - b[1]) + inset));
}
static void cv_ring(const ov_canvas *cv, float cx, float cy, float r, float t, const uint8_t c[4]) {
    for (int y = (int)(cy - r) - 1; y <= (int)(cy + r) + 1; y++) for (int x = (int)(cx - r) - 1; x <= (int)(cx + r) + 1; x++) {
        float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
        cv_blend(cv, x, y, c, fminf(0.5f - (d - r), 0.5f + (d - (r - t))));
    }
}
static float cv_text_w(const char *s, int scale) { return (float)strlen(s) * 6 * scale - scale; }
static void cv_text(const ov_canvas *cv, float x, float cy, const char *s, int scale, const uint8_t c[4]) {   // x: the left edge
    int x0 = (int)(x + 0.5f), y0 = (int)(cy - 7 * scale / 2.0f + 0.5f);
    for (int i = 0; s[i]; i++) {
        unsigned ch = (unsigned char)s[i]; const unsigned char *g = FONT5X7[(ch >= 32 && ch < 127 ? ch : 63) - 32];
        for (int col = 0; col < 5; col++) for (int row = 0; row < 7; row++) if (g[col] >> row & 1)
            for (int dy = 0; dy < scale; dy++) for (int dx = 0; dx < scale; dx++) cv_blend(cv, x0 + i * 6 * scale + col * scale + dx, y0 + row * scale + dy, c, 1);
    }
}
static const uint8_t C_WHITE[4] = {255, 255, 255, 255}, C_ACCENT[4] = {255, 60, 142, 255};
static void ov_menu_icon(const ov_canvas *cv, const ov_ctl *c, const float b[4]) {   // the menu button: a dark pill with a ring and three bars
    static const uint8_t fill[4] = {20, 20, 26, 170}, ring[4] = {255, 255, 255, 210};
    float r = b[3], t = fmaxf(2, r * 0.07f);
    for (int y = (int)(b[1] - b[3]) - 1; y <= (int)(b[1] + b[3]) + 1; y++) for (int x = (int)(b[0] - b[2]) - 1; x <= (int)(b[0] + b[2]) + 1; x++) {
        float d = ov_sdf(c, b, x + 0.5f - b[0], y + 0.5f - b[1]);
        cv_blend(cv, x, y, fill, 0.5f - d); cv_blend(cv, x, y, ring, fminf(0.5f - d, 0.5f + d + t));
    }
    float bw = r * 0.50f, bt = fmaxf(2, r * 0.11f), gap = r * 0.30f, cx = b[0], cy = b[1];
    for (int k = -1; k <= 1; k++) cv_rrect(cv, cx - bw, cy + k * gap - bt / 2, cx + bw, cy + k * gap + bt / 2, bt / 2, C_WHITE);
}
// the text entry (front_android.c front_text_input), in the pause menu's panel: the game's title for it, the field with the text and a caret, the keyboard
// (overlay_layout.h ov_kb_*: the same rectangles the touches are tested against) with the highlighted key, and the controller's keys
static void ov_text_panel(const ov_canvas *cv) {
    char title[128], text[256]; int sel, shift; if (!dm_text_entry(title, sizeof title, text, sizeof text, &sel, &shift)) return;
    float W = (float)g_ov.pw, H = (float)g_ov.ph, p[4] = {0, 0, W, H}, pad = W * 0.03f;
    static const uint8_t bg[4] = {14, 14, 20, 236}, field[4] = {255, 255, 255, 30}, key[4] = {255, 255, 255, 26}, dim[4] = {200, 200, 210, 255};
    cv_rrect(cv, 0, 0, W, H, H * 0.03f, bg);
    int ts = (int)fmaxf(1, roundf(H * 0.045f / 7)), xs = (int)fmaxf(1, roundf(H * 0.06f / 7)), ks = (int)fmaxf(1, roundf(H * 0.05f / 7)), hs = (int)fmaxf(1, roundf(H * 0.036f / 7));
    float room = W - 4 * pad;
    while (title[0] && cv_text_w(title, ts) > room) title[strlen(title) - 1] = 0;   // a long title is cut
    cv_text(cv, (W - cv_text_w(title, ts)) / 2, H * 0.07f, title, ts, C_WHITE);
    cv_rrect(cv, pad, H * 0.14f, W - pad, H * 0.27f, H * 0.02f, field);
    char shown[260]; snprintf(shown, sizeof shown, "%s_", text);   // the caret
    const char *s = shown; while (*s && cv_text_w(s, xs) > room) s++;   // the end of a long text
    cv_text(cv, 2 * pad, H * 0.205f, s, xs, C_WHITE);
    static const char *wide[4] = {"Shift", "Space", "Del", "OK"};
    for (int i = 0; i < OVK_N; i++) {
        float r[4]; ov_kb_key(i, p, r);
        int on = i == sel || (i == OVK_SHIFT && shift);
        cv_rrect(cv, r[0], r[1], r[0] + r[2], r[1] + r[3], r[3] * 0.18f, i == sel ? C_ACCENT : on ? field : key);
        char one[2] = {(char)ov_kb_char(i, shift), 0}; const char *l = i < 40 ? one : wide[i - 40];
        int ls = ks; while (ls > 1 && cv_text_w(l, ls) > r[2] * 0.85f) ls--;   // a wide key's word fits inside it
        cv_text(cv, r[0] + (r[2] - cv_text_w(l, ls)) / 2, r[1] + r[3] / 2, l, ls, i == OVK_OK && i != sel ? C_ACCENT : C_WHITE);
    }
    const char *k = "A key  B delete  Y shift  + OK  - cancel";   // one line, centred, smaller until it fits
    while (hs > 1 && cv_text_w(k, hs) > W - 2 * pad) hs--;
    cv_text(cv, (W - cv_text_w(k, hs)) / 2, H * 0.95f, k, hs, dim);
}
static const uint8_t C_BG[4] = {14, 14, 20, 236}, C_LINE[4] = {255, 255, 255, 40}, C_HI[4] = {255, 255, 255, 34}, C_DIM[4] = {200, 200, 210, 255}, C_OFF[4] = {80, 80, 92, 255},
                     C_KNOB_OFF[4] = {215, 215, 222, 255};
static void cv_button(const ov_canvas *cv, const float b[4], const char *l, int fill, int held) {   // a rounded button with its word, as large as fits
    static const uint8_t face[4] = {255, 255, 255, 30}, rim[4] = {255, 255, 255, 90};
    float rad = b[3] * 0.28f; cv_rrect(cv, b[0], b[1], b[0] + b[2], b[1] + b[3], rad, held ? C_WHITE : rim);
    cv_rrect(cv, b[0] + 2, b[1] + 2, b[0] + b[2] - 2, b[1] + b[3] - 2, rad - 2, held ? C_ACCENT : fill ? C_ACCENT : face);
    int s = (int)fmaxf(1, roundf(b[3] * 0.40f / 7)); while (s > 1 && cv_text_w(l, s) > b[2] * 0.82f) s--;
    cv_text(cv, b[0] + (b[2] - cv_text_w(l, s)) / 2, b[1] + b[3] / 2, l, s, C_WHITE);
}
// The pause menu (overlay_layout.h: the rows, the tabs, the slider's track), in the raster's panel rectangle. Its letters are as large as a row allows (0.30 of
// its height, about 2.7 mm on a 6.7" phone and 3.6 mm on the 8.8" tablet) and smaller only when the widest row would not fit beside its control
static const char *const OVM_LABELS[OVM_N] = {"Show gamepad", "Opacity", "Edit layout", "Rumble", "Show FPS", "Mode", "Resume", ""};
static const char *const OVM_MODES[2] = {"Handheld", "Docked"};
static void ov_menu_panel(const ov_canvas *cv, int st) {
    float p[4] = {0, 0, (float)cv->w, (float)cv->h}, r[4]; ov_menu_rowrect(p, 0, r);
    float rh = r[3], rw = r[2], gap = rh * 0.35f, sh = rh * 0.40f, sw = sh * 1.85f;
    int tab = st >> 8 & 1, sel = (st >> 4 & 15) - 1, op = st >> 9 & 127;
    int is = (int)fmaxf(1, roundf(rh * 0.30f / 7));
    for (; is > 1; is--) {   // the widest row: its label, a gap and its control
        float need = 0;
        for (int i = 0; i < OVM_RESUME; i++) {
            float lw = cv_text_w(OVM_LABELS[i], is), cw = i == OVM_MODE ? 2 * (cv_text_w(OVM_MODES[0], is) + sh) : i == OVM_EDIT ? cv_text_w("Edit", is) + 2 * sh : sw + gap + cv_text_w("OFF", is);
            need = fmaxf(need, i == OVM_OPACITY ? fmaxf(lw / 0.38f, cv_text_w("80%", is) / 0.14f) : lw + gap + cw + rh * 0.3f);
        }
        if (need <= rw && cv_text_w(OVM_TAB_NAMES[0], is) < rw / 2 - sh) break;
    }
    cv_rrect(cv, 0, 0, p[2], p[3], rh * 0.18f, C_BG);
    for (int row = 0; row < OVM_ROWS; row++) {
        int it = ov_menu_item(tab, row); ov_menu_rowrect(p, row, r); float cy = r[1] + rh / 2, x1 = r[0] + rw - rh * 0.15f;
        if (it < 0) continue;
        if (row == sel) cv_rrect(cv, r[0], r[1] + rh * 0.08f, r[0] + rw, r[1] + rh * 0.92f, rh * 0.14f, C_HI);
        if (it == OVM_TAB) {   // the tabs: a segmented bar, the open one filled
            float th = rh * 0.62f, tw = rw / OVM_TABS;
            cv_rrect(cv, r[0], cy - th / 2, r[0] + rw, cy + th / 2, th / 2, C_OFF);
            cv_rrect(cv, r[0] + tab * tw, cy - th / 2, r[0] + (tab + 1) * tw, cy + th / 2, th / 2, C_ACCENT);
            for (int k = 0; k < OVM_TABS; k++) cv_text(cv, r[0] + k * tw + (tw - cv_text_w(OVM_TAB_NAMES[k], is)) / 2, cy, OVM_TAB_NAMES[k], is, k == tab ? C_WHITE : C_DIM);
            cv_rrect(cv, r[0], r[1] + rh - 1, r[0] + rw, r[1] + rh, 0.5f, C_LINE);
            continue;
        }
        if (it == OVM_RESUME) { cv_rrect(cv, r[0], r[1], r[0] + rw, r[1] + 1, 0.5f, C_LINE); cv_text(cv, r[0] + (rw - cv_text_w("Resume", is)) / 2, cy, "Resume", is, C_ACCENT); continue; }
        cv_text(cv, r[0] + rh * 0.15f, cy, OVM_LABELS[it], is, row == sel ? C_WHITE : C_DIM);
        if (it == OVM_OPACITY) {   // the slider: its track, filled to the value, the knob and the percent
            float t[3]; ov_menu_track(p, row, t); float f = (float)(op - OV_OPACITY_MIN) / (OV_OPACITY_MAX - OV_OPACITY_MIN), th = fmaxf(2, rh * 0.08f), kr = rh * 0.20f;
            f = f < 0 ? 0 : f > 1 ? 1 : f;
            cv_rrect(cv, t[0], t[1] - th / 2, t[0] + t[2], t[1] + th / 2, th / 2, C_OFF);
            cv_rrect(cv, t[0], t[1] - th / 2, t[0] + f * t[2], t[1] + th / 2, th / 2, C_ACCENT);
            cv_disc(cv, t[0] + f * t[2], t[1], kr, C_WHITE);
            char v[8]; snprintf(v, sizeof v, "%d%%", op); cv_text(cv, x1 - cv_text_w(v, is), cy, v, is, C_WHITE);
        } else if (it == OVM_EDIT) {   // a button
            float bw = cv_text_w("Edit", is) + 2 * sh, b[4] = {x1 - bw, cy - rh * 0.30f, bw, rh * 0.60f}; cv_button(cv, b, "Edit", 0, 0);
        } else if (it == OVM_MODE) {   // two segments, the current one filled: handheld (performance) | docked (resolution)
            int docked = st >> 2 & 1; float hh = rh * 0.46f, segw = fmaxf(cv_text_w(OVM_MODES[0], is), cv_text_w(OVM_MODES[1], is)) + sh, x0 = x1 - 2 * segw;
            cv_rrect(cv, x0, cy - hh / 2, x1, cy + hh / 2, hh / 2, C_OFF);
            cv_rrect(cv, x0 + docked * segw, cy - hh / 2, x0 + (docked + 1) * segw, cy + hh / 2, hh / 2, C_ACCENT);
            for (int k = 0; k < 2; k++) cv_text(cv, x0 + k * segw + (segw - cv_text_w(OVM_MODES[k], is)) / 2, cy, OVM_MODES[k], is, k == docked ? C_WHITE : C_DIM);
        } else {   // a switch
            int on = st >> (it == OVM_OVERLAY ? 0 : it == OVM_RUMBLE ? 1 : 3) & 1; float sx0 = x1 - sw;
            cv_rrect(cv, sx0, cy - sh / 2, x1, cy + sh / 2, sh / 2, on ? C_ACCENT : C_OFF);
            cv_disc(cv, on ? x1 - sh / 2 : sx0 + sh / 2, cy, sh / 2 - fmaxf(2, sh * 0.1f), on ? C_WHITE : C_KNOB_OFF);
            const char *word = on ? "ON" : "OFF"; cv_text(cv, sx0 - gap - cv_text_w(word, is), cy, word, is, on ? C_WHITE : C_DIM);
        }
    }
}
// The layout editor's toolbar and prompt (overlay_layout.h ov_ed_*), at half the window's scale in the raster's panel rectangle: the toolbar at its top, the
// prompt under it (gfx_vk_present.c ov_render draws each where overlay_layout.h puts it)
static void ov_ed_rects(float tb[4], float pr[4]) {   // their places in the raster
    float W = (float)g_sext.width, H = (float)g_sext.height, a[4], b[4]; ov_ed_rect(W, H, a); ov_ed_prompt_rect(W, H, b);
    tb[0] = 0; tb[1] = 0; tb[2] = fminf(a[2] / 2, (float)g_ov.pw); tb[3] = a[3] / 2;
    pr[0] = 0; pr[1] = tb[3] + 2; pr[2] = fminf(b[2] / 2, (float)g_ov.pw); pr[3] = fminf(b[3] / 2, (float)g_ov.ph - pr[1]);
}
static const char *ov_ctl_name(const ov_ctl *c) {
    return c->kind == OV_STICK_L ? "Left stick" : c->kind == OV_STICK_R ? "Right stick" : c->kind == OV_DPAD ? "D-pad" : c->kind == OV_MENU ? "Menu button"
         : !strcmp(c->label, "-") ? "Minus" : !strcmp(c->label, "+") ? "Plus" : c->label;
}
static void ov_edit_panel(const ov_canvas *cv, int st) {
    float tb[4], pr[4]; ov_ed_rects(tb, pr);
    int held = (st >> 4 & 15) - 1, sel = (st >> 8 & 255) - 1;
    static const char *const words[OVE_ALL] = {"-", "+", "Reset", "Cancel", "Save", "Discard", "Keep editing"};
    cv_rrect(cv, tb[0], tb[1], tb[0] + tb[2], tb[1] + tb[3], tb[3] * 0.14f, C_BG);
    char hint[96];
    if (sel >= 0 && sel < OV_N) snprintf(hint, sizeof hint, "%s: drag, pinch, - +", ov_ctl_name(&OV_CTLS[sel]));   // (short: 2x-scale letters on a phone)
    else snprintf(hint, sizeof hint, "Drag to move, pinch to resize");
    int hs = (int)fmaxf(1, roundf(tb[3] * 0.17f / 7)); while (hs > 1 && cv_text_w(hint, hs) > tb[2] * 0.94f) hs--;
    cv_text(cv, tb[0] + (tb[2] - cv_text_w(hint, hs)) / 2, tb[1] + tb[3] * 0.21f, hint, hs, sel >= 0 ? C_WHITE : C_DIM);
    for (int i = 0; i < OVE_N; i++) { float b[4]; ov_ed_button(i, tb, b); cv_button(cv, b, words[i], i == OVE_SAVE, i == held); }
    if (!(st & 1)) return;
    cv_rrect(cv, pr[0], pr[1], pr[0] + pr[2], pr[1] + pr[3], pr[3] * 0.10f, C_BG);
    const char *q = "Discard your changes?"; int qs = (int)fmaxf(1, roundf(pr[3] * 0.13f / 7)); while (qs > 1 && cv_text_w(q, qs) > pr[2] * 0.9f) qs--;
    cv_text(cv, pr[0] + (pr[2] - cv_text_w(q, qs)) / 2, pr[1] + pr[3] * 0.27f, q, qs, C_WHITE);
    for (int i = OVE_DISCARD; i <= OVE_KEEP; i++) { float b[4]; ov_ed_button(i, pr, b); cv_button(cv, b, words[i], i == OVE_DISCARD, i == held); }
}
static void ov_panel(int st) {   // the pause menu, the text entry or the layout editor, for menu state st (dm_menu_state), into the panel's rows of the upload buffer
    ov_canvas cv = {g_ov.px, g_ov.w, 0, g_ov.h + g_ov.sh, g_ov.pw, g_ov.ph};
    for (uint32_t y = 0; y < g_ov.ph; y++) memset(g_ov.px + 4 * ((size_t)(cv.y0 + y) * g_ov.w), 0, (size_t)g_ov.pw * 4);
    if (st >> 16 & 1) ov_text_panel(&cv);   // a text entry is open (dm_menu_state)
    else if (st & OVE_ST) ov_edit_panel(&cv, st);
    else ov_menu_panel(&cv, st);
}
static void ov_raster(uint8_t *px, uint32_t w, uint32_t h, uint32_t sh) {
    memset(px, 0, (size_t)w * (h + sh) * 4);
    int ts = h > 600 ? 3 : 2;
    for (int i = 0; i < OV_N && g_tt[g_ov.lay].ready != 1; i++) {   // (with a theme the controls are its sprites: the static layer stays empty)
        const ov_ctl *c = &OV_CTLS[i]; float b[4]; ov_box(c, (float)w, (float)h, b);
        if (c->kind == OV_MENU) continue;   // a sprite only
        if (c->kind == OV_DPAD) {
            for (int k = 0; k < 4; k++) { float ox, oy, dr; ov_dpad_dir(k, &ox, &oy, &dr); ov_circle(px, w, h, b[0] + ox * b[2], b[1] + oy * b[2], dr * b[2], 2.5f); }
            continue;
        }
        if (ov_round(c)) ov_circle(px, w, h, b[0], b[1], b[2], c->kind == OV_BUTTON ? 2.5f : 3.0f);
        else ov_box_ring(px, w, h, c, b, 2.5f);
        int ls = ts; while (ls > 1 && 7 * ls > fminf(b[2], b[3])) ls--;   // smaller on a small button (-, +)
        if (c->label[0]) ov_text(px, w, h, b[0], b[1], c->label, ls);
    }
    ov_canvas strip = {px, w, 0, 0, w, h + sh};
    for (int i = 0; i < SP_N; i++) {   // the sprites (the knobs are drawn only as sprites, where the finger has them)
        float x = g_ov.sp[i][0], y = g_ov.sp[i][1], r = g_ov.sp[i][2], b[4] = {x, y, r, g_ov.sp[i][3]}; if (!r) continue;
        if (i == SP_DISC) cv_disc(&strip, x, y, r, C_WHITE);
        else if (i == SP_BLACK || i == SP_WHITE || i == SP_ACCENT) { static const uint8_t k[4] = {0, 0, 0, 255}; cv_rrect(&strip, x - r, y - r, x + r, y + r, 0, i == SP_BLACK ? k : i == SP_WHITE ? C_WHITE : C_ACCENT); }
        else if (OV_CTLS[i].kind == OV_MENU) ov_menu_icon(&strip, &OV_CTLS[i], b);
        else if (OV_CTLS[i].kind == OV_BUTTON) cv_box(&strip, &OV_CTLS[i], b, fminf(b[2], b[3]) * 0.07f, C_WHITE);   // inset as the disc's 0.93
        else ov_circle(px, w, h + sh, x, y, r, 2.5f);
    }
}
typedef struct { float pos[4], uv[4], alpha; } ov_quad;
static void ov_draw(VkCommandBuffer cb, const ov_quad *q) { vkCmdPushConstants(cb, g_ov.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *q, q); vkCmdDraw(cb, 6, 1, 0, 0); }
static void ov_rect(VkCommandBuffer cb, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float alpha) {   // screen pixels, texels
    float W = (float)g_sext.width, H = (float)g_sext.height, tw = (float)g_ov.w, th = (float)(g_ov.h + g_ov.sh + g_ov.ph + g_ov.gh);
    ov_quad q = {{x0 / W * 2 - 1, y0 / H * 2 - 1, x1 / W * 2 - 1, y1 / H * 2 - 1}, {u0 / tw, v0 / th, u1 / tw, v1 / th}, alpha}; ov_draw(cb, &q);
}
static void ov_sprite(VkCommandBuffer cb, int i, float sx, float sy, float r, float alpha) {   // sprite i centred on screen pixel (sx, sy), r screen pixels in half width
    float k = r / g_ov.sp[i][2], hx = g_ov.sp[i][2] + 3, hy = g_ov.sp[i][3] + 3;
    ov_rect(cb, sx - hx * k, sy - hy * k, sx + hx * k, sy + hy * k, g_ov.sp[i][0] - hx, g_ov.sp[i][1] - hy, g_ov.sp[i][0] + hx, g_ov.sp[i][1] + hy, alpha);
}
// The theme's sprites (overlay_layout.h ov_tsprite): the design's CSS pixels scaled by the window's height over the design's (and dm_overlay_scale), each at its
// offset from its control's centre. Decorations first, then each control in the table's order: a held button, d-pad direction or stick in its held sprite,
// a stick's knob where the finger has it (as far as the design lets it travel); the menu button last (ov_theme_menu), over everything
static void ov_tsp(VkCommandBuffer cb, const ov_theme *t, const ov_tsprite *s, float cx, float cy, float k, float alpha) {
    float W = (float)g_sext.width, H = (float)g_sext.height, x0 = cx + s->ox * k, y0 = cy + s->oy * k, x1 = x0 + s->w / t->scale * k, y1 = y0 + s->h / t->scale * k;
    ov_quad q = {{x0 / W * 2 - 1, y0 / H * 2 - 1, x1 / W * 2 - 1, y1 / H * 2 - 1}, {s->u / t->w, s->v / t->h, (s->u + s->w) / t->w, (s->v + s->h) / t->h}, alpha}; ov_draw(cb, &q);
}
static float ov_glow(int64_t t, int64_t now) {   // a held button's glow: bright at the press, fading to a steady one
    if (!t) return 0;
    float f = (float)(now - t) / OV_FLASH_NS; return f >= 1 ? 0.22f : 0.22f + 0.48f * (1 - f) * (1 - f);
}
// The frame rate counter: the game's frames shown per second, measured over half a second, in the window's top left corner on a dark block
static float ov_fps_cell(void) { return 6.0f * g_ov.gs + 2; }   // a character cell's width in texels (a glyph of 5 columns, a gap, a clear texel each side)
static void ov_fps_glyphs(void) {   // the characters into the rows under the panel
    ov_canvas cv = {g_ov.px, g_ov.w, 0, g_ov.h + g_ov.sh + g_ov.ph, g_ov.w, g_ov.gh};
    for (int i = 0; OV_FPS_CHARS[i]; i++) { char one[2] = {OV_FPS_CHARS[i], 0}; cv_text(&cv, i * ov_fps_cell() + 1 + g_ov.gs * 0.5f, g_ov.gh / 2.0f, one, (int)g_ov.gs, C_WHITE); }
}
static void ov_fps(VkCommandBuffer cb, int64_t now) {
    static uint64_t n0; static int64_t t0; static int fps = -1;
    uint64_t n = atomic_load(&g_fps_frames);
    if (!t0 || now - t0 > 2000000000ll) { t0 = now; n0 = n; fps = -1; }   // the first frame, or after a pause in the presents (the menu, the background): measure anew
    else if (now - t0 >= 500000000ll) { fps = (int)((double)(n - n0) * 1e9 / (double)(now - t0) + 0.5); t0 = now; n0 = n; }
    char s[16]; if (fps < 0) snprintf(s, sizeof s, "-- FPS"); else snprintf(s, sizeof s, "%d FPS", fps > 999 ? 999 : fps);
    float k = (float)g_sext.width / g_ov.w, cw = ov_fps_cell() * k, ch = (float)g_ov.gh * k, m = (float)g_sext.height * 0.02f, x = m + ch * 0.25f, pad = ch * 0.2f;
    float bx = g_ov.sp[SP_BLACK][0], by = g_ov.sp[SP_BLACK][1], gy = (float)(g_ov.h + g_ov.sh + g_ov.ph);
    ov_rect(cb, m, m, m + ch * 0.5f + strlen(s) * cw, m + ch + 2 * pad, bx - 2, by - 2, bx + 2, by + 2, 0.45f);
    for (int i = 0; s[i]; i++, x += cw) {
        const char *c = strchr(OV_FPS_CHARS, s[i]); if (!c || s[i] == ' ') continue;
        float u = (float)(c - OV_FPS_CHARS) * ov_fps_cell(); ov_rect(cb, x, m + pad, x + cw, m + pad + ch, u, gy, u + ov_fps_cell(), gy + g_ov.gh, 1);
    }
}
static void ov_theme_draw(VkCommandBuffer cb, const ov_theme *t, float alpha) {   // (alpha: the player's opacity; each control at its own size factor)
    float W = (float)g_sext.width, H = (float)g_sext.height, k = H / t->dh * dm_overlay_scale(), b[4]; const ov_ctl *L = OV_LAYOUTS[t->layout];
    for (int j = 0; j < t->n; j++) if (t->sp[j].state == OVT_DECOR) { const ov_ctl *c = &L[t->sp[j].ctl]; ov_box(c, W, H, b); ov_tsp(cb, t, &t->sp[j], b[0], b[1], k * ov_cscale(c), alpha); }
    for (int i = 0; i < OV_N; i++) {
        const ov_ctl *c = &L[i]; if (c->kind == OV_MENU) continue;
        ov_box(c, W, H, b); float kc = k * ov_cscale(c); int stick = c->kind == OV_STICK_L || c->kind == OV_STICK_R, held = c->kind != OV_DPAD && dm_overlay_pressed(i, 0) != 0;
        float kx = 0, ky = 0; if (stick) { dm_overlay_knob(c->kind == OV_STICK_R, &kx, &ky); kx *= t->travel * kc; ky *= t->travel * kc; }
        for (int j = 0; j < t->n; j++) {
            const ov_tsprite *s = &t->sp[j]; if (s->ctl != i || s->state == OVT_DECOR) continue;
            if (s->state == OVT_KNOB) { ov_tsp(cb, t, s, b[0] + kx, b[1] + ky, kc, alpha); continue; }
            int on = c->kind == OV_DPAD ? s->dir >= 0 && dm_overlay_pressed(i, OV_DPAD_BITS[s->dir]) != 0 : held;
            if (s->state == (on ? OVT_HELD : OVT_IDLE)) ov_tsp(cb, t, s, b[0], b[1], kc, alpha);
        }
    }
}
static void ov_theme_menu(VkCommandBuffer cb, const ov_theme *t, int i, float alpha) {
    const ov_ctl *c = &OV_LAYOUTS[t->layout][i]; float W = (float)g_sext.width, H = (float)g_sext.height, b[4]; ov_box(c, W, H, b);
    int held = dm_overlay_pressed(i, 0) != 0;
    for (int j = 0; j < t->n; j++) if (t->sp[j].ctl == i && t->sp[j].state == (held ? OVT_HELD : OVT_IDLE)) ov_tsp(cb, t, &t->sp[j], b[0], b[1], H / t->dh * dm_overlay_scale() * ov_cscale(c), alpha);
}
static void ov_block(VkCommandBuffer cb, int sp, float x0, float y0, float x1, float y1, float alpha) {   // a solid rectangle in a block sprite's colour
    float bx = g_ov.sp[sp][0], by = g_ov.sp[sp][1]; ov_rect(cb, x0, y0, x1, y1, bx - 2, by - 2, bx + 2, by + 2, alpha);
}
static void ov_frame(VkCommandBuffer cb, int sp, const float b[4], float t, float alpha) {   // a rectangle's outline around a control's box (b: ov_box), t pixels thick
    float x0 = b[0] - b[2] - t, x1 = b[0] + b[2] + t, y0 = b[1] - b[3] - t, y1 = b[1] + b[3] + t;
    ov_block(cb, sp, x0, y0, x1, y0 + t, alpha); ov_block(cb, sp, x0, y1 - t, x1, y1, alpha); ov_block(cb, sp, x0, y0 + t, x0 + t, y1 - t, alpha); ov_block(cb, sp, x1 - t, y0 + t, x1, y1 - t, alpha);
}
static void ov_render(VkCommandBuffer cb, int mode, int menu, int fps) {
    float W = (float)g_sext.width, H = (float)g_sext.height, op = dm_overlay_opacity();
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); int64_t now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    int edit = menu >= 0 && !(menu >> 16 & 1) && (menu & OVE_ST);   // the layout editor: the gamepad over the held frame, every control (shown or not)
    if (edit) mode = 1;
    float bx = g_ov.sp[SP_BLACK][0], by = g_ov.sp[SP_BLACK][1];
    if (menu >= 0 && !edit) {   // paused: the frame dimmed, the panel, and the menu button (which closes it)
        ov_rect(cb, 0, 0, W, H, bx - 2, by - 2, bx + 2, by + 2, 0.55f);
        float r[4]; ov_menu_rect(W, H, r);
        ov_rect(cb, r[0], r[1], r[0] + r[2], r[1] + r[3], 0, (float)(g_ov.h + g_ov.sh), (float)g_ov.pw, (float)(g_ov.h + g_ov.sh + g_ov.ph), 1);
    } else if (edit) ov_rect(cb, 0, 0, W, H, bx - 2, by - 2, bx + 2, by + 2, 0.30f);
    const ov_theme *th = g_tt[g_ov.lay].ready == 1 ? g_tt[g_ov.lay].t : NULL; int tbound = 0;   // (the theme's texture bound)
    if ((menu < 0 || edit) && mode == 1 && th) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ov.layout, 0, 1, &g_tt[g_ov.lay].set, 0, NULL); tbound = 1;
        ov_theme_draw(cb, th, op);
    } else if ((menu < 0 || edit) && mode == 1) {   // without a theme: the outlines (their opacity: 60% looks as they always did), the knobs and the glows
        float fo = fminf(1, op / 0.6f);
        ov_rect(cb, 0, 0, W, H, 0, 0, (float)g_ov.w, (float)g_ov.h, fo);
        for (int i = 0; i < OV_N; i++) { const ov_ctl *c = &OV_CTLS[i]; float b[4]; ov_box(c, W, H, b);
            if (c->kind == OV_STICK_L || c->kind == OV_STICK_R) {
                float dx, dy; dm_overlay_knob(c->kind == OV_STICK_R, &dx, &dy); float travel = b[2] * (1 - OV_KNOB);   // the knob's edge reaches the ring
                ov_sprite(cb, i, b[0] + dx * travel, b[1] + dy * travel, b[2] * OV_KNOB, fo);
            } else if (c->kind == OV_BUTTON) { float g = ov_glow(dm_overlay_pressed(i, 0), now); if (g <= 0) continue;
                if (!ov_round(c)) ov_sprite(cb, i, b[0], b[1], b[2], g); else ov_sprite(cb, SP_DISC, b[0], b[1], b[2] * 0.93f, g); }
            else if (c->kind == OV_DPAD) for (int k = 0; k < 4; k++) {
                float g = ov_glow(dm_overlay_pressed(i, OV_DPAD_BITS[k]), now), ox, oy, dr; if (g <= 0) continue;
                ov_dpad_dir(k, &ox, &oy, &dr); ov_sprite(cb, SP_DISC, b[0] + ox * b[2], b[1] + oy * b[2], dr * b[2] * 0.93f, g);
            }
        }
    }
    for (int i = 0; i < OV_N; i++) if (OV_CTLS[i].kind == OV_MENU && mode) {   // the menu button, on top of the controls, where the player put it
        float ma = mode == 1 && (menu < 0 || edit) ? fmaxf(op, 0.5f) : OV_MENU_ALPHA;   // (findable at any opacity)
        if (th) { if (!tbound) { vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ov.layout, 0, 1, &g_tt[g_ov.lay].set, 0, NULL); tbound = 1; }
                  ov_theme_menu(cb, th, i, ma); continue; }
        const ov_ctl *c = &OV_CTLS[i]; float b[4], g = ov_glow(dm_overlay_pressed(i, 0), now); ov_box(c, W, H, b);
        ov_sprite(cb, i, b[0], b[1], b[2], ma);
        if (g > 0) ov_sprite(cb, i, b[0], b[1], b[2], g);
    }
    if (tbound) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ov.layout, 0, 1, &g_ov.set, 0, NULL);   // the rest is the procedural texture's
    if (edit) {   // every control's box faintly, the chosen one in the accent colour; the toolbar; the prompt over a darker frame
        int sel = (menu >> 8 & 255) - 1; float t = fmaxf(2, H * 0.003f);
        for (int i = 0; i < OV_N; i++) { float b[4]; ov_box(&OV_CTLS[i], W, H, b); if (i == sel) ov_frame(cb, SP_ACCENT, b, 2 * t, 1); else ov_frame(cb, SP_WHITE, b, t, 0.35f); }
        float tb[4], pr[4], a[4], p[4], y0 = (float)(g_ov.h + g_ov.sh); ov_ed_rects(tb, pr); ov_ed_rect(W, H, a); ov_ed_prompt_rect(W, H, p);
        ov_rect(cb, a[0], a[1], a[0] + a[2], a[1] + a[3], tb[0], y0 + tb[1], tb[0] + tb[2], y0 + tb[1] + tb[3], 1);
        if (menu & 1) {
            ov_rect(cb, 0, 0, W, H, bx - 2, by - 2, bx + 2, by + 2, 0.45f);
            ov_rect(cb, p[0], p[1], p[0] + p[2], p[1] + p[3], pr[0], y0 + pr[1], pr[0] + pr[2], y0 + pr[1] + pr[3], 1);
        }
    }
    if (fps && menu < 0) ov_fps(cb, now);
}
// the theme's atlas for a layout into its texture, once (overlay_theme.c reads it at the first present that shows the gamepad in that layout): copied in this
// present's command buffer; its staging buffer goes at a later present, once the copy has run (its slot's fence signalled, or the slot came round again)
static void ov_theme_upload(VkCommandBuffer cb, uint32_t k, int lay) {
    typeof(g_tt[0]) *T = &g_tt[lay];
    if (T->up && (k == T->upk || vkWaitForFences(V.dev, 1, &g_pfence[T->upk], 1, 0) == VK_SUCCESS)) { vkDestroyBuffer(V.dev, T->up, NULL); vkFreeMemory(V.dev, T->upm, NULL); T->up = VK_NULL_HANDLE; }
    if (T->ready) return;
    const ov_theme *t = dm_overlay_theme(lay); T->ready = -1;
    if (!t || !t->px) return;
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {(uint32_t)t->w, (uint32_t)t->h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VKCHK(vkCreateImage(V.dev, &ic, NULL, &T->img)); VkMemoryRequirements rq; vkGetImageMemoryRequirements(V.dev, T->img, &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &T->mem)); VKCHK(vkBindImageMemory(V.dev, T->img, T->mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, T->img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VKCHK(vkCreateImageView(V.dev, &vi, NULL, &T->view));
    VkDeviceSize n = (VkDeviceSize)t->w * t->h * 4;
    VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VKCHK(vkCreateBuffer(V.dev, &bc, NULL, &T->up)); VkMemoryRequirements bq; vkGetBufferMemoryRequirements(V.dev, T->up, &bq);
    VkMemoryAllocateInfo bi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, bq.size, vk_mem_type(bq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
    VKCHK(vkAllocateMemory(V.dev, &bi, NULL, &T->upm)); VKCHK(vkBindBufferMemory(V.dev, T->up, T->upm, 0));
    void *px; VKCHK(vkMapMemory(V.dev, T->upm, 0, VK_WHOLE_SIZE, 0, &px)); memcpy(px, t->px, (size_t)n);   // (freeing the memory unmaps it)
    dm_overlay_theme_done(t);
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_ov.pool, 1, &g_ov.dsl};
    if (vkAllocateDescriptorSets(V.dev, &da, &T->set)) { VKRED_ONCE(95, "overlay theme descriptor set failed: the gamepad is drawn without its theme"); return; }
    VkDescriptorImageInfo ii = {g_ov.samp, T->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, T->set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii};
    vkUpdateDescriptorSets(V.dev, 1, &wr, 0, NULL);
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, T->img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy c = {0, (uint32_t)t->w, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)t->w, (uint32_t)t->h, 1}};
    vkCmdCopyBufferToImage(cb, T->up, T->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    T->t = t; T->upk = k; T->ready = 1;
}
static int overlay_ready(VkCommandBuffer cb, uint32_t k, int menu) {   // k: this present's slot (its fence is not signalled yet)
    uint32_t w = g_sext.width / 2, h = g_sext.height / 2; int lay = dm_overlay_layout();
    if (!g_ov.dsl) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b}; VKCHK(vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_ov.dsl));
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ov_quad)};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_ov.dsl, 1, &pr}; VKCHK(vkCreatePipelineLayout(V.dev, &pl, NULL, &g_ov.layout));
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}; VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 3, 1, &ps};   // the procedural texture, a theme atlas per layout
        VKCHK(vkCreateDescriptorPool(V.dev, &pi, NULL, &g_ov.pool));
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_ov.pool, 1, &g_ov.dsl}; VKCHK(vkAllocateDescriptorSets(V.dev, &ai, &g_ov.set));
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        VKCHK(vkCreateSampler(V.dev, &si, NULL, &g_ov.samp));
    }
    if (g_ov.pfmt != g_sfmt) {   // the pipeline renders into the swapchain's format
        VkShaderModule vm, fm;
        VkShaderModuleCreateInfo vc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof overlay_vert_spv, overlay_vert_spv}; VKCHK(vkCreateShaderModule(V.dev, &vc, NULL, &vm));
        VkShaderModuleCreateInfo fc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof overlay_frag_spv, overlay_frag_spv}; VKCHK(vkCreateShaderModule(V.dev, &fc, NULL, &fm));
        VkPipelineShaderStageCreateInfo ss[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", NULL},
                                                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", NULL}};
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
        VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1};
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState ba = {1, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 0xf};
        VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &ba};
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
        VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &g_sfmt};
        if (!V.has_dynrender && g_ov.rp) { ov_fb_drop(); vkDestroyRenderPass(V.dev, g_ov.rp, NULL); g_ov.rp = VK_NULL_HANDLE; }   // the pass follows the swapchain's format, as the pipeline does
        VkRenderPass orp = V.has_dynrender ? VK_NULL_HANDLE : ov_rp();
        if (!V.has_dynrender && !orp) return 0;
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, orp ? NULL : &rci, 0, 2, ss, &vi, &ia, NULL, &vs, &rs, &ms, NULL, &cbs, &dy, g_ov.layout, orp};
        VkResult r = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &g_ov.pipe);
        vkDestroyShaderModule(V.dev, vm, NULL); vkDestroyShaderModule(V.dev, fm, NULL);
        if (r) { VKRED_ONCE(92, "overlay pipeline failed: %d", r); return 0; }
        g_ov.pfmt = g_sfmt;
    }
    ov_theme_upload(cb, k, lay);
    if (g_ov.w != w || g_ov.h != h || g_ov.lay != lay || (g_tt[lay].ready != 1 && g_ov.cver != dm_overlay_custom_ver())) {   // (re)rasterize for this window size and layout
        g_ov.cver = dm_overlay_custom_ver();   // (and, without a theme, the player's own layout: the outlines are in the static layer)
        vkDeviceWaitIdle(V.dev);
        if (g_ov.view) { vkDestroyImageView(V.dev, g_ov.view, NULL); vkDestroyImage(V.dev, g_ov.img, NULL); vkFreeMemory(V.dev, g_ov.mem, NULL); vkDestroyBuffer(V.dev, g_ov.up, NULL); vkFreeMemory(V.dev, g_ov.upm, NULL); }
        g_ov.lay = lay; uint32_t sh = ov_layout_sprites(w, h); float mr[4]; ov_menu_rect((float)g_sext.width, (float)g_sext.height, mr);
        g_ov.pw = (uint32_t)(mr[2] / 2) + 1; g_ov.ph = (uint32_t)(mr[3] / 2) + 1; if (g_ov.pw > w) g_ov.pw = w;
        g_ov.gs = (uint32_t)fmaxf(2, roundf(h * 0.035f / 7)); g_ov.gh = 7 * g_ov.gs + 4;   // the counter's characters: 3.5% of the window's height
        uint32_t th = h + sh + g_ov.ph + g_ov.gh;
        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, th, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VKCHK(vkCreateImage(V.dev, &ic, NULL, &g_ov.img)); VkMemoryRequirements rq; vkGetImageMemoryRequirements(V.dev, g_ov.img, &rq);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
        VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &g_ov.mem)); VKCHK(vkBindImageMemory(V.dev, g_ov.img, g_ov.mem, 0));
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_ov.img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKCHK(vkCreateImageView(V.dev, &vi, NULL, &g_ov.view));
        VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)w * th * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VKCHK(vkCreateBuffer(V.dev, &bc, NULL, &g_ov.up)); VkMemoryRequirements bq; vkGetBufferMemoryRequirements(V.dev, g_ov.up, &bq);
        VkMemoryAllocateInfo bi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, bq.size, vk_mem_type(bq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        VKCHK(vkAllocateMemory(V.dev, &bi, NULL, &g_ov.upm)); VKCHK(vkBindBufferMemory(V.dev, g_ov.up, g_ov.upm, 0));
        void *px; VKCHK(vkMapMemory(V.dev, g_ov.upm, 0, VK_WHOLE_SIZE, 0, &px)); g_ov.px = px;
        g_ov.w = w; g_ov.h = h; g_ov.sh = sh; ov_raster(g_ov.px, w, h, sh); memset(g_ov.px + (size_t)w * (h + sh) * 4, 0, (size_t)w * (g_ov.ph + g_ov.gh) * 4); ov_fps_glyphs();
        VkDescriptorImageInfo ii = {g_ov.samp, g_ov.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_ov.set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii};
        vkUpdateDescriptorSets(V.dev, 1, &wr, 0, NULL);
        g_ov.uploaded = 0; g_ov.panel = -1;
    }
    uint32_t y0 = 0, rows = g_ov.h + g_ov.sh + g_ov.ph + g_ov.gh;   // the rows to upload
    if (g_ov.uploaded && menu >= 0 && menu != g_ov.panel) {   // only the panel, after the last copy out of the buffer has run
        VkFence f[8]; uint32_t n = 0; for (uint32_t i = 0; i < 8; i++) if (i != k) f[n++] = g_pfence[i];
        vkWaitForFences(V.dev, n, f, 1, 1000000000ull);
        y0 = g_ov.h + g_ov.sh; rows = g_ov.ph; g_ov.uploaded = 0;
    }
    if (menu >= 0 && menu != g_ov.panel) { ov_panel(menu); g_ov.panel = menu; }
    if (!g_ov.uploaded) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, y0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_ov.img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy c = {(VkDeviceSize)y0 * g_ov.w * 4, g_ov.w, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, (int32_t)y0, 0}, {g_ov.w, rows, 1}};
        vkCmdCopyBufferToImage(cb, g_ov.up, g_ov.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        g_ov.uploaded = 1;
    }
    return 1;
}
// ---- frame pacing on the display (VK_GOOGLE_display_timing) ----
// Without a time of its own a frame goes on screen at the first vsync after it is latched. Mario vs. Donkey Kong at 60 fps on the Legion Y700's 120 Hz panel was ready about
// 4.6 ms before that latch (SurfaceFlinger's latency: 12.9 ms from ready to shown, GPU 9.5-10.5 ms a frame), and about one frame in ten missed it by a millisecond or so: shown
// for three vsyncs, the next one for one (11 of 125 frames each way, a visible stutter; Hades II's title, ready far earlier, showed every frame for two). Each frame now gets a
// desired present time: the last one's plus the game's period, on the display's vsync grid (anchored at a past frame's actual present time), half a vsync before that vsync so
// it cannot go out early. A frame that would not be ready in time moves the cadence to the first vsync it can make (a "resync": one longer frame, and from then on a vsync of
// slack). The pause menu's redraws carry no time. DM_VK_PACE=0 (live) presents without times
static uint64_t g_refresh_ns, g_pace_target, g_pace_anchor, g_pace_n, g_pace_resync, g_pace_slip; static uint32_t g_pace_id; static int g_pace_on = 1;
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void pace_reset(void) { g_pace_target = g_pace_anchor = g_refresh_ns = 0; }
static uint64_t pace_snap(uint64_t t) {   // the first vsync at or after t on the grid through the anchor
    uint64_t r = g_refresh_ns, a = g_pace_anchor;
    if (t <= a) return a - (a - t) / r * r;
    return a + (t - a + r - 1) / r * r;
}
static uint64_t pace_desired(double period) {   // the frame's desired present time (0 = none)
    if (!V.has_dtiming || !g_swap) return 0;
    if (g_pace_n % 30 == 0) { const char *e = getenv("DM_VK_PACE"); g_pace_on = !(e && !atoi(e)); }
    if (!g_refresh_ns || g_pace_n % 120 == 0) {
        VkRefreshCycleDurationGOOGLE rc; uint64_t was = g_refresh_ns;
        if (vkGetRefreshCycleDurationGOOGLE(V.dev, g_swap, &rc) == VK_SUCCESS && rc.refreshDuration > 1000000) g_refresh_ns = rc.refreshDuration;
        if (g_refresh_ns && g_refresh_ns != was) { VKLOG("[perf] display timing: refresh %.3f ms", g_refresh_ns / 1e6); g_pace_target = 0; }
    }
    VkPastPresentationTimingGOOGLE pt[16]; uint32_t np = 16;
    if (vkGetPastPresentationTimingGOOGLE(V.dev, g_swap, &np, pt) >= 0)
        for (uint32_t i = 0; i < np; i++) {
            present_time_record(pt[i].presentID, pt[i].actualPresentTime);
            if (pt[i].actualPresentTime > g_pace_anchor) g_pace_anchor = pt[i].actualPresentTime;
            if (pt[i].desiredPresentTime && pt[i].actualPresentTime > pt[i].desiredPresentTime + g_refresh_ns) g_pace_slip++;   // went out a vsync or more after its own
        }
    g_pace_n++;
    if (g_pace_n % 600 == 0) VKLOG("[perf] display timing: %llu frames, %llu resyncs, %llu shown a vsync or more after their time (cumulative)%s",
                                   (unsigned long long)g_pace_n, (unsigned long long)g_pace_resync, (unsigned long long)g_pace_slip, g_pace_on ? "" : "; off (DM_VK_PACE=0)");
    if (!g_pace_on || !g_refresh_ns || !g_pace_anchor || period <= 0) { g_pace_target = 0; return 0; }
    // The earliest vsync this frame can make: its GPU work (submitted just before), the blit, then the latch a vsync before it is shown. When the cadence's next vsync is
    // earlier, the cadence restarts half a vsync further out than that, so the next frames have room for the jitter: restarting at the earliest itself made 31 long frames
    // in 4,200 (5 shown late), and going on until the target was under a vsync away made 54 in 3,600 go out late
    double vk_gpu_ms_recent(void); double g = vk_gpu_ms_recent(); uint64_t gpu = (uint64_t)((g > 4 ? g : 4) * 1e6) + 3000000;
    uint64_t now = mono_ns(), t = g_pace_target ? pace_snap(g_pace_target + (uint64_t)(period * 1e9) - g_refresh_ns / 2) : 0;
    if (t < now + gpu + g_refresh_ns) { if (g_pace_target) g_pace_resync++; t = pace_snap(now + gpu + g_refresh_ns * 3 / 2); }
    g_pace_target = t;
    return t - g_refresh_ns / 2;
}
static void present_window(vkimg_t *im, const int crop[4], double period, uint64_t ticket) {   // period: the game's frame period (s); < 0 for a redraw (ticket 0)
    uint32_t k = g_pi++ % 8;
    vkWaitForFences(V.dev, 1, &g_pfence[k], 1, UINT64_MAX);
    VkFormat want = im->fmt == VK_FORMAT_R8G8B8A8_SRGB || im->fmt == VK_FORMAT_B8G8R8A8_SRGB || im->fmt == VK_FORMAT_A8B8G8R8_SRGB_PACK32 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (want != g_want) { g_want = want; swap_destroy(); if (!swap_make()) return; }   // once, at the first present of the other kind
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(V.dev, g_swap, UINT64_MAX, g_acq[k], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { swap_destroy(); if (!swap_make()) return; r = vkAcquireNextImageKHR(V.dev, g_swap, UINT64_MAX, g_acq[k], VK_NULL_HANDLE, &idx); }
    if (r < 0) { VKRED_ONCE(90, "vkAcquireNextImageKHR: %d", r); return; }
    vkResetFences(V.dev, 1, &g_pfence[k]);
    VkCommandBuffer cb = g_pcb[k];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; vkBeginCommandBuffer(cb, &bi);
    vk_img_barrier_all(cb);   // after everything submitted before on this queue
    barrier(cb, g_simg[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g_simg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
    int cx = crop ? crop[0] : 0, cy = crop ? crop[1] : 0, cw = crop && crop[2] ? crop[2] : (int)im->w, ch = crop && crop[3] ? crop[3] : (int)im->h;
    double s = fmin((double)g_sext.width / cw, (double)g_sext.height / ch); int dw = (int)(cw * s), dh = (int)(ch * s), dx = ((int)g_sext.width - dw) / 2, dy = ((int)g_sext.height - dh) / 2;
    vk_pic_set((float)dx / g_sext.width, (float)dy / g_sext.height, (float)dw / g_sext.width, (float)dh / g_sext.height);
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{cx, cy, 0}, {cx + cw, cy + ch, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    if (vk_window_bottom_up()) { b.srcOffsets[0].y = cy + ch; b.srcOffsets[1].y = cy; }   // lower-left origin: the window image is stored bottom-up (gfx_vk_exec.c)
    vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, g_simg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    VkImageLayout lay = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    int ovm = dm_overlay_visible(), menu = dm_menu_state(), fps = dm_overlay_fps();
    if ((ovm || menu >= 0 || fps) && overlay_ready(cb, k, menu) && (V.has_dynrender || ov_fb(idx))) {   // without dynamic rendering, only with the image's framebuffer
        barrier(cb, g_simg[idx], lay, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); lay = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, g_sview[idx], lay, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, g_sext}, 1, 0, 1, &ca, NULL, NULL};
        VkFramebuffer ofb = V.has_dynrender ? VK_NULL_HANDLE : ov_fb(idx);
        if (V.has_dynrender) vkCmdBeginRendering(cb, &ri);
        else { VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, g_ov.rp, ofb, {{0, 0}, g_sext}, 0, NULL};
               vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE); }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ov.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ov.layout, 0, 1, &g_ov.set, 0, NULL);
        VkViewport vp = {0, 0, (float)g_sext.width, (float)g_sext.height, 0, 1}; VkRect2D sc = {{0, 0}, g_sext};
        vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
        ov_render(cb, ovm, menu, fps);
        if (V.has_dynrender) vkCmdEndRendering(cb); else vkCmdEndRenderPass(cb);
    }
    barrier(cb, g_simg[idx], lay, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &g_acq[k], &ws, 1, &cb, 1, &g_done_sem[k]};
    extern pthread_mutex_t g_vk_queue_mu;
    pthread_mutex_lock(&g_vk_queue_mu);
    if (vkQueueSubmit(V.q, 1, &si, g_pfence[k])) { pthread_mutex_unlock(&g_vk_queue_mu); VKRED_ONCE(91, "present submit failed"); return; }
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &g_done_sem[k], 1, &g_swap, &idx, NULL};
    VkPresentTimeGOOGLE ptm = {++g_pace_id, period >= 0 ? pace_desired(period) : 0};
    VkPresentTimesInfoGOOGLE pti = {VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, NULL, 1, &ptm};
    if (V.has_dtiming) pi.pNext = &pti;
    r = vkQueuePresentKHR(V.q, &pi);
    if (V.has_dtiming && r >= 0) present_time_map(ptm.presentID, ticket);
    pthread_mutex_unlock(&g_vk_queue_mu);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) swap_destroy();
}
// headless frame shots
static int shot_wanted(uint64_t n) {
    static uint64_t until; const char *k = getenv("DM_VK_SHOTS");   // a DM_CTL "env DM_VK_SHOTS=N" line: each of the next N presents (a transition caught frame by frame)
    if (k && atoi(k) > 0) { until = n + (uint64_t)atoi(k); setenv("DM_VK_SHOTS", "0", 1); VKLOG("frame shots: presents %llu to %llu", (unsigned long long)n, (unsigned long long)until - 1); }
    if (n < until) return 1;
    static const char *e; static int init; if (!init) { init = 1; e = getenv("DM_VK_SHOT"); }
    if (!e) return 0;
    for (const char *p = e; *p;) { char *end; unsigned long long v = strtoull(p, &end, 10); if (end == p) break; if (v == n) return 1; p = *end == ',' ? end + 1 : end; }
    return 0;
}
static void shot(vkimg_t *im, uint64_t n) {
    void mtl_sub_begin(void); uint64_t mtl_sub_commit(void); int mtl_wait(uint64_t, uint64_t);
    uint32_t w = im->w, h = im->h; VkBuffer sb; VkDeviceSize so;
    mtl_sub_begin();
    uint8_t *p = vk_scratch((VkDeviceSize)w * h * 4, 16, &sb, &so);
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    VkBufferImageCopy c = {so, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &c);
    mtl_wait(mtl_sub_commit(), ~0ull);
    const char *dir = getenv("DM_VK_SHOT_DIR"); if (!dir) dir = getenv("DM_ROOT"); if (!dir) dir = ".";
    char path[1024]; snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dir, (unsigned long long)n);
    FILE *f = fopen(path, "wb");
    if (!f) { VKLOG("[RED] cannot write %s", path); return; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    int bgra = im->fmt == VK_FORMAT_B8G8R8A8_UNORM || im->fmt == VK_FORMAT_B8G8R8A8_SRGB, ll = vk_window_bottom_up();   // a lower-left window image is stored bottom-up: written top row first, as shown
    uint8_t *row = malloc((size_t)w * 3);
    for (uint32_t y = 0; y < h; y++) { const uint8_t *s = p + (size_t)(ll ? h - 1 - y : y) * w * 4; for (uint32_t x = 0; x < w; x++) { row[3 * x] = s[4 * x + (bgra ? 2 : 0)]; row[3 * x + 1] = s[4 * x + 1]; row[3 * x + 2] = s[4 * x + (bgra ? 0 : 2)]; } fwrite(row, 1, (size_t)w * 3, f); }
    free(row); fclose(f);
    VKLOG("frame %llu written to %s (%ux%u)", (unsigned long long)n, path, w, h);
}
static pthread_mutex_t g_present_mu = PTHREAD_MUTEX_INITIALIZER;   // the swapchain against the window going away (vk_window_lost, on the main thread)
void vk_window_lost(void) { pthread_mutex_lock(&g_present_mu); swap_destroy(); g_win = NULL; pthread_mutex_unlock(&g_present_mu); }
// The present thread: mtl_present only queues the frame and returns (as on Apple), so the game thread never waits in vkAcquireNextImageKHR or on the
// swapchain's fences (a FIFO swapchain blocks there until a vsync); the window texture is not drawn again before its blit, because NVN's acquire waits for
// its ticket (mtl_present_wait), and the blit's submission comes after the frame's own on the one queue. DM_VK_SYNC_PRESENT=1 presents on the game thread.
#define PQ_N 4
static struct { vkimg_t *im; int crop[4], has_crop; uint64_t n; double period; } g_pq[PQ_N];
static uint64_t g_pq_head, g_pq_tail, g_pq_done;   // queued [tail, head), presents up to ticket g_pq_done are blitted
static pthread_mutex_t g_pq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pq_cv = PTHREAD_COND_INITIALIZER;
// While the game is held (the pause menu) no frame comes: dm_overlay_redraw presents the last one again with what the overlay now shows.
static _Atomic int g_redraw;
void dm_overlay_redraw(void) { atomic_store(&g_redraw, 1); pthread_mutex_lock(&g_pq_mu); pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu); }
static void *present_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk present");
    typeof(g_pq[0]) last = {0};
    for (;;) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_pq_tail == g_pq_head && !atomic_load(&g_redraw)) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        atomic_store(&g_redraw, 0);
        if (g_pq_tail == g_pq_head) {   // a redraw, no new frame
            pthread_mutex_unlock(&g_pq_mu);
            if (last.im) { pthread_mutex_lock(&g_present_mu); if (swap_make()) present_window(last.im, last.has_crop ? last.crop : NULL, -1, 0); pthread_mutex_unlock(&g_present_mu); }
            continue;
        }
        typeof(g_pq[0]) e = g_pq[g_pq_tail % PQ_N];
        pthread_mutex_unlock(&g_pq_mu);
        if (e.im) { last = e; pthread_mutex_lock(&g_present_mu); if (swap_make()) present_window(e.im, e.has_crop ? e.crop : NULL, e.period, e.n); pthread_mutex_unlock(&g_present_mu); }
        pthread_mutex_lock(&g_pq_mu); g_pq_tail++; g_pq_done = e.n; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
    }
    return NULL;
}
static void present_thread_start(void) { pthread_t t; pthread_create(&t, NULL, present_thread, NULL); pthread_detach(t); }
static uint64_t present_enqueue(vkimg_t *im, const int crop[4], uint64_t n, double period) {   // im NULL: nothing to show, the ticket still completes in order
    static int sync = -1; static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (sync < 0) sync = getenv("DM_VK_SYNC_PRESENT") != NULL;
    if (sync || !dm_front_window()) {   // headless: nothing to show; or presenting on this thread
        if (sync && im) { pthread_mutex_lock(&g_present_mu); if (swap_make()) present_window(im, crop, period, n); pthread_mutex_unlock(&g_present_mu); }
        pthread_mutex_lock(&g_pq_mu); if (g_pq_tail == g_pq_head) g_pq_done = n; pthread_mutex_unlock(&g_pq_mu);   // done at once (after any queued ones)
        return n;
    }
    pthread_once(&once, present_thread_start);
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_head - g_pq_tail >= PQ_N) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // never more than PQ_N frames behind
    typeof(g_pq[0]) *e = &g_pq[g_pq_head % PQ_N]; e->im = im; e->n = n; e->period = period; e->has_crop = crop != NULL; if (crop) memcpy(e->crop, crop, sizeof e->crop);
    g_pq_head++; pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    return n;
}
void dm_perf_frame(double period); void dm_perf_window(ANativeWindow *w, double fps); void dm_perf_frame_cost(int64_t ns);   // perf_android.c
uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {
    vk_init();
    { uint64_t vk_replay_ns(void); double vk_gpu_ms_recent(void);   // what else the frame costs: its replay and its GPU time
      static uint64_t last_exec; uint64_t ex = vk_replay_ns(), rp = ex - last_exec; last_exec = ex; double g = vk_gpu_ms_recent(); int64_t gn = g > 0 ? (int64_t)(g * 1e6) : 0;
      dm_perf_frame_cost((int64_t)rp > gn ? (int64_t)rp : gn); }
    dm_perf_frame(period);   // paced to the game's frame period, and its work time reported to ADPF
    static ANativeWindow *rate_win; static double rate_period;
    if (g_win && (g_win != rate_win || period != rate_period)) { rate_win = g_win; rate_period = period; dm_perf_window(g_win, period > 0 ? 1.0 / period : 60); }
    uint64_t n = ++g_presents;
    // without display times (no VK_GOOGLE_display_timing, no window, or none came back) the hand-over time stands in, as before: the governor's late check needs readings
    { int disp = V.has_dtiming && dm_front_window() && (n <= 240 || g_pres_recorded); struct timespec ts; if (!disp) clock_gettime(CLOCK_MONOTONIC, &ts);
      present_time_begin(n, disp ? 0 : (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec); }
    vkimg_t *im = vk_tex(t);
    static uint64_t no_img, undrawn, shown;
    if (n % 600 == 0) {
        extern struct { uint64_t draws, skip_prog, skip_rt, skip_pipe, passes, folded, dispatches, pipes_built, rt_kept, ns_exec, ns_desc, ns_pipe, max_pass_draws; } g_vkst; extern uint64_t g_ubd_landed, g_ubd_superseded, g_ubd_shadows, g_ubd_bytes, g_ns_submit, g_gpu_ns, g_pkg_n, g_pkg_bytes, g_pkg_ns, g_set_hits, g_set_new, g_ubd_racy, g_ns_set_build, g_ns_set_hash, g_ns_set_new, g_set_memo, g_set_memo_bad;
        static uint64_t psb, psh, psn, pe, pd, pp, ps, pb, pdr, pg, pkn, pkb, pkt, psm;
        VKLOG("replay up to present %llu: draws %llu, skipped (no program %llu, no target %llu, no pipeline %llu), passes %llu, clears folded %llu, dispatches %llu, pipelines %llu; presents shown %llu, no image %llu, never drawn %llu; uniform updates landed %llu (superseded %llu, shadows %llu, CPU reads racing a GPU write %llu), same targets kept %llu, most draws in one pass %llu",
              (unsigned long long)n, (unsigned long long)g_vkst.draws, (unsigned long long)g_vkst.skip_prog, (unsigned long long)g_vkst.skip_rt, (unsigned long long)g_vkst.skip_pipe,
              (unsigned long long)g_vkst.passes, (unsigned long long)g_vkst.folded, (unsigned long long)g_vkst.dispatches, (unsigned long long)g_vkst.pipes_built,
              (unsigned long long)shown, (unsigned long long)no_img, (unsigned long long)undrawn, (unsigned long long)g_ubd_landed, (unsigned long long)g_ubd_superseded, (unsigned long long)g_ubd_shadows, (unsigned long long)g_ubd_racy, (unsigned long long)g_vkst.rt_kept, (unsigned long long)g_vkst.max_pass_draws);
        double nd = g_vkst.draws > pdr ? (double)(g_vkst.draws - pdr) / 600 : 0;
        VKLOG("replay per frame (last 600): %.0f draws, replay %.2f ms (descriptor sets %.2f, pipeline keys %.2f), queue submit %.2f ms, uniform shadows %.0f KB; GPU busy %.2f ms; texture uploads %.1f (%.2f MB, %.2f ms); descriptor sets reused %llu, new %llu (collecting %.2f ms, hashing %.2f, new ones %.2f), kept per stage %.0f a frame (memo differences %llu)",
              nd, (g_vkst.ns_exec - pe) / 6e8, (g_vkst.ns_desc - pd) / 6e8, (g_vkst.ns_pipe - pp) / 6e8, (g_ns_submit - ps) / 6e8, (g_ubd_bytes - pb) / 600.0 / 1024, (g_gpu_ns - pg) / 6e8, (g_pkg_n - pkn) / 600.0, (g_pkg_bytes - pkb) / 600.0 / 1048576, (g_pkg_ns - pkt) / 6e8, (unsigned long long)g_set_hits, (unsigned long long)g_set_new, (g_ns_set_build - psb) / 6e8, (g_ns_set_hash - psh) / 6e8, (g_ns_set_new - psn) / 6e8,
              (g_set_memo - psm) / 600.0, (unsigned long long)g_set_memo_bad);
        void vk_pass_report(void); vk_pass_report();
        pe = g_vkst.ns_exec; pd = g_vkst.ns_desc; pp = g_vkst.ns_pipe; ps = g_ns_submit; pb = g_ubd_bytes; pdr = g_vkst.draws; pg = g_gpu_ns; pkn = g_pkg_n; pkb = g_pkg_bytes; pkt = g_pkg_ns; psb = g_ns_set_build; psh = g_ns_set_hash; psn = g_ns_set_new; psm = g_set_memo;
    }
    if (!im) { no_img++; return present_enqueue(NULL, NULL, n, period); }
    if (im->layout != VK_IMAGE_LAYOUT_GENERAL) { undrawn++; return present_enqueue(NULL, NULL, n, period); }   // never drawn into: nothing to show yet
    shown++; atomic_fetch_add(&g_fps_frames, 1);
    if (shot_wanted(n)) shot(im, n);
    { void vk_pass_dump_poll(uint64_t done); uint64_t vk_fence_done(void); vk_pass_dump_poll(vk_fence_done()); void vk_pass_frame(void); vk_pass_frame(); void vk_tex_sweep(void); vk_tex_sweep(); }
    if (n % 1800 == 0) {   // every ~30 s: the pipeline cache to disk on a thread of its own (an Android app can be killed without any exit path)
        void vk_pipeline_cache_save(void); pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &a, (void *(*)(void *))(void *)vk_pipeline_cache_save, NULL); pthread_attr_destroy(&a);
    }
    return present_enqueue(im, crop, n, period);
}
void mtl_present_wait(uint64_t tk) {   // NVN acquire: the window texture's last present has been blitted (the game may draw into it again)
    if (!tk) return;
    pthread_mutex_lock(&g_pq_mu);
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 1;
    while (g_pq_done < tk && pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &dl) == 0) {}
    pthread_mutex_unlock(&g_pq_mu);
}
double mtl_presented_time(uint64_t n) { return present_time_get(n); }
double vk_refresh_ms(void) { return V.has_dtiming && g_pres_recorded && g_refresh_ns ? g_refresh_ns / 1e6 : 0; }   // perf_android.c's late check: the display's refresh, when display times are what it gets
uint32_t mtl_present_qos(void) { return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_has_window(void) { return dm_front_window() != NULL; }
int mtl_display_hz(void) { return 60; }
int mtl_fps_target(void) { return DM_GAME_FPS_TARGET > 0 ? DM_GAME_FPS_TARGET : 60; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_prog_release(prog_t *p) { (void)p; }
