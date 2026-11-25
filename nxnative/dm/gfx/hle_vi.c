// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include "../host/hle.h"
#include <stdatomic.h>

static struct { uint32_t magic; } g_display = {0x50534944}, g_layer = {0x5259414c};
static struct { uint32_t magic; void *layer; } g_native = {0x4e49574e, 0};

HLE_FORCE("_ZN2nn2vi10InitializeEv", "display service init; the host has no vi service", void, h_vi_Init, void) {}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "open default display; the host returns a static object", Result, h_vi_OpenDisp, void **out) {
    *out = &g_display; return RES_OK;
}
// The named overload (South Park: The Stick of Truth, SDK 4.5.0, opens the display as "Default" early in nnMain). The host has no vi service, and this one's own
// SDK implementation resolves the display through it (vi::GetService, sdk+0x401130): the pointer comes back null and the first vtable call reads address 0
// (SIGSEGV at sdk+0x1d2f30, nn::vi::OpenDisplay+0xf4). Same two-enumeration shape as CreateLayer's size overload below: the host returns its static display.
HLE_FORCE("_ZN2nn2vi11OpenDisplayEPPNS0_7DisplayEPKc", "open the named display (\"Default\"); the host returns the same static object as OpenDefaultDisplay", Result, h_vi_OpenDispNamed, void **out, const char *name) {
    LOG("vi::OpenDisplay(\"%s\")", name ? name : "");
    *out = &g_display; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "create layer; the host returns a static object", Result, h_vi_CreateLayer, void **out, void *disp) {
    (void)disp; *out = &g_layer; return RES_OK;
}
// The overload with the layer's size (SDL3's Switch video driver, Absolum: 1920x1080 docked); the SDK's version calls through the vi service object the host never made (SIGSEGV at +0xe4).
// An NVN or Vulkan window's textures decide what is presented; an EGL window surface takes the layer's size (dm/gfx/gl/hle_gl.m: Digimon Story Cyber Sleuth sizes its layer
// itself and draws into the surface at that size)
static _Atomic uint32_t g_layer_wh;   // w | h << 16 of the last sized CreateLayer, 0 = none
void dm_vi_layer_size(int *w, int *h) { uint32_t v = g_layer_wh; *w = (int)(v & 0xffff); *h = (int)(v >> 16); }
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii", "create layer of a given size; the host returns a static object", Result, h_vi_CreateLayerWH, void **out, void *disp, int w, int h) {
    (void)disp; LOG("vi::CreateLayer %dx%d", w, h); *out = &g_layer;
    if (w > 0 && h > 0 && w < 0x10000 && h < 0x10000) g_layer_wh = (uint32_t)w | (uint32_t)h << 16;
    return RES_OK;
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "native window handle, passed to the NVN window builder; the host returns a static object", Result, h_vi_GetNative, void **out, void *layer) {
    (void)layer; *out = &g_native; return RES_OK;
}
// Link's Awakening also crops its layer and tears the display down on exit; the SDK's versions dereference the vi service objects the host never made (SetLayerCrop: SIGSEGV at +0x64).
// The crop is kept for the Vulkan present (hle_vk.m): Super Mario Galaxy keeps a 1920x1080 swapchain in handheld mode, draws into its top-left 1280x720 and crops the layer
// to that every frame. NVN games crop through their window instead (nvnWindowSetCrop), so their present does not read it. Logged only when it changes
static _Atomic uint64_t g_crop;   // x | y << 16 | w << 32 | h << 48, 0 = none
void dm_vi_layer_crop(int c[4]) { uint64_t v = g_crop; c[0] = (int)(v & 0xffff); c[1] = (int)(v >> 16 & 0xffff); c[2] = (int)(v >> 32 & 0xffff); c[3] = (int)(v >> 48); }
HLE_FORCE("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", "layer crop; the host records it and the Vulkan present crops by it", Result, h_vi_SetCrop, void *layer, int x, int y, int w, int h) {
    (void)layer;
    uint64_t v = x < 0 || y < 0 || w <= 0 || h <= 0 || x > 0xffff || y > 0xffff || w > 0xffff || h > 0xffff ? 0 : (uint64_t)x | (uint64_t)y << 16 | (uint64_t)w << 32 | (uint64_t)h << 48;
    if (atomic_exchange(&g_crop, v) != v) LOG("vi::SetLayerCrop %d,%d %dx%d", x, y, w, h);
    return RES_OK;
}
// Super Mario Odyssey sets its layer's scaling mode; the SDK's version uses the vi service objects the host never made
HLE_FORCE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", "layer scaling mode; the host present does its own scaling", Result, h_vi_SetScaling, void *layer, int mode) {
    (void)layer; LOG("vi::SetLayerScalingMode %d (ignored)", mode); return RES_OK;
}
// Unreal Engine paces its frames by the layer's frame number (Octopath Traveler 0, main+0x10140e8); the SDK's versions read the Android surface of the vi service the host never made
// (SIGSEGV in android::Surface::getQueuedFrameNumber). The host's frame number is the count of frames the game has presented
uint64_t dm_nvn_frame_count(void);
HLE_FORCE("_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE", "the layer's latest frame number: the frames presented so far", Result, h_vi_FrameNo, uint64_t *out, const void *layer) {
    (void)layer; *out = dm_nvn_frame_count(); return RES_OK;
}
// The layer's frame history: the last 8 frames presented, newest first, each with whether it reached the screen. Unreal Engine 5's Switch RHI tracks its flips by it: its
// "Viewport Thread" (Minecraft Dungeons II main+0x142afb0) waits for each vsync, asks how many entries there are (a null buffer: the SDK answers 8, the history's size), reads them and
// takes the first one shown as the latest flip, which completes the frame-end fences that wait for the swap chain (r.GTSyncType 2). With no history no flip ever came, and the first
// FlushRenderingCommands, which waits for every fence (FFrameEndSync, main+0x15015f0), waited for ever right after the opening movie: the game thread polled its fence each ms, nothing
// was presented again. FrameInfo is 40 bytes, as the SDK's ListFrameHistory (sdk 20.5.15, sdk+0x4510a0) fills it from the compositor's records: status (2 shown with a present
// time, 1 queued and not shown yet), frame number (the numbering of GetLatestFrameNumber), the queue tick and the present tick twice; sorted by frame number, newest first.
// A frame is shown once the backend's present queue has handed it on (gfx_vk_present.c / nvn_mtl.m g_pq_done); its present time is the flip time the backend reports
// (mtl_flip_time: on Android the display's actual present time from VK_GOOGLE_display_timing, else the time vkQueuePresentKHR returned for it; on Apple the drawable's
// presented time). Else (headless, before the time comes back, or a frame never put on screen), when the host pacer drives a vsync grid, it is the first tick of that grid at
// or after both its present and the end of its GPU work, latched once reached so a frame never reverts to queued or shifts its flip tick. A game that times its frames by these
// flips (Star Fox's throttle) sees a fixed vsync grid, as on the Switch, instead of its own first-read time. Without a pacer grid (DM_DETCLOCK, or unpaced presents), the time it
// was first seen handed on stays. A Switch's compositor shows every frame it is handed, in order, so a frame the host dropped still counts as shown: listed as queued, the newest
// ones stayed so for ever when the game presented nothing more until their flip came (Orbitals, main+0x1540460: its FFrameEndSync waited for the flip of a dropped frame, until
// Unreal aborted: "GameThread timed out waiting for ..."). Each entry takes the backend's own ticket for its present (dm_vi_frame_ticket, from wherever mtl_present ran: the
// game thread, or the replay thread under DM_NVN_ASYNC), so a frame is never judged by the previous frame's present
#define VFH 8
typedef struct { uint64_t frame, ticket, queued_ns, seen_ns, at, grid, tick, vsync_ns; uint8_t queued, enqueued; } vfh_ent;   // at, grid, tick, vsync_ns: present, pacer vsync, one vsync, latched flip (now_ns clock, ns); queued: the game thread recorded it; enqueued: its ticket is known
static vfh_ent g_vfh[VFH]; static pthread_mutex_t g_vfh_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_vfh_asked;   // the game has called ListFrameHistory, with or without a buffer
int dm_vi_history_asked(void) { return atomic_load_explicit(&g_vfh_asked, memory_order_relaxed); }   // hle_nvn.c: until then nvnWindowGetPresentInterval answers as before the history
void mtl_present_progress(uint64_t *queued, uint64_t *done);   // the backend: the last present ticket queued, and the last its present queue has handed on
double mtl_flip_time(uint64_t n);                              // the backend: when present ticket n reached the screen (seconds on the now_ns clock), 0 not known
int mtl_gpu_done(uint64_t f, uint64_t *t1);                    // the backend: 1 the frame's GPU work completed (latest end, now_ns clock; 0 if unknown), 0 running, -1 no record
void dm_vi_frame_queued(uint64_t frame, uint64_t at, uint64_t grid, uint64_t tick) {   // hle_nvn.c, at each present the game queues: frame = GetLatestFrameNumber's count with it
    pthread_mutex_lock(&g_vfh_mu);
    vfh_ent *e = &g_vfh[frame % VFH];
    uint64_t tk = e->frame == frame && e->enqueued ? e->ticket : 0;
    uint8_t enq = e->frame == frame ? e->enqueued : 0;
    *e = (vfh_ent){frame, tk, guest_ns(), 0, at, grid, tick, 0, 1, enq};
    pthread_mutex_unlock(&g_vfh_mu);
}
void dm_vi_frame_ticket(uint64_t frame, uint64_t ticket) {   // hle_nvn.c, when mtl_present has given this frame's present its ticket (0: nothing presented), in either order with the above
    pthread_mutex_lock(&g_vfh_mu);
    vfh_ent *e = &g_vfh[frame % VFH];
    if (e->frame == frame) { e->ticket = ticket; e->enqueued = 1; }
    else if (e->frame < frame) *e = (vfh_ent){frame, ticket, 0, 0, 0, 0, 0, 0, 0, 1};
    pthread_mutex_unlock(&g_vfh_mu);
}
// The vsync a display would show the frame at (now_ns clock): 1 with it, 0 not yet (its GPU work still running, or that vsync still to come), -1 no pacer grid.
// Once reached, e->vsync_ns latches the flip time so later reads stay monotonic. The frame's GPU work is tagged with the frame count before its present
// (mtl_set_frame at the previous present), one less than its number here.
static int vfh_vsync(vfh_ent *e, uint64_t hnow, uint64_t *at) {
    if (e->vsync_ns) { *at = e->vsync_ns; return 1; }
    if (!e->grid || !e->tick) return -1;
    uint64_t ready = e->at, end = 0; int g = mtl_gpu_done(e->frame - 1, &end);
    if (!g) return 0;
    if (g > 0 && end > ready) ready = end;
    uint64_t t = ready <= e->grid ? e->grid : e->grid + (ready - e->grid + e->tick - 1) / e->tick * e->tick;
    if (t > hnow) return 0;
    e->vsync_ns = t; *at = t; return 1;
}
static uint64_t vfh_ticks(uint64_t ns) { return ns * 12 / 625; }   // the 19.2 MHz system counter, as GetSystemTick (hle_svc.c)
HLE_FORCE("_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", "the layer's frame history: the last 8 presents, newest first, shown once the present queue handed them on", int, h_vi_FrameHist, void *out, int n, const void *layer) {
    (void)layer; atomic_store_explicit(&g_vfh_asked, 1, memory_order_relaxed);
    if (!out || n <= 0) return VFH;
    uint64_t q, d; mtl_present_progress(&q, &d); (void)q;
    uint64_t gnow = guest_ns(), hnow = now_ns(), latest = dm_nvn_frame_count();
    int k = 0;
    pthread_mutex_lock(&g_vfh_mu);
    for (uint64_t f = latest; f > 0 && latest - f < VFH && k < n; f--) {
        vfh_ent *e = &g_vfh[f % VFH]; if (e->frame != f || !e->queued) continue;
        uint64_t *o = (uint64_t *)out + 5 * k++, shown = 0;
        int handed = e->enqueued && e->ticket <= d;
        double pt = handed && e->ticket ? mtl_flip_time(e->ticket) : 0;
        if (pt > 0) { uint64_t ago = hnow > (uint64_t)(pt * 1e9) ? hnow - (uint64_t)(pt * 1e9) : 0; shown = gnow > ago ? gnow - ago : 1; }
        else if (handed) {
            uint64_t vs; int v = vfh_vsync(e, hnow, &vs);
            if (v > 0) { uint64_t ago = hnow > vs ? hnow - vs : 0; shown = gnow > ago ? gnow - ago : 1; }
            else if (v < 0) { if (!e->seen_ns) e->seen_ns = gnow; shown = e->seen_ns; }
        }
        o[0] = shown ? 2 : 1; o[1] = f; o[2] = vfh_ticks(e->queued_ns); o[3] = o[4] = shown ? vfh_ticks(shown) : 0;
    }
    pthread_mutex_unlock(&g_vfh_mu);
    static int once; if (k && !once++) LOG("vi::ListFrameHistory: %d of the last %d presents returned (up to frame %llu, the present queue at %llu)", k, VFH, (unsigned long long)latest, (unsigned long long)d);
    return k;
}
// Rhythm Heaven Groove (SDK 21) asks for the built-in panel's identity at start (main+0xa9a4); the SDK's version calls through the vi service object the host never made (SIGSEGV at +0x20).
//   The game reads a u32 kind and, when it is 2 or more, 0x14 more bytes that it hands on; any other answer makes it hand on zeros. The host has no panel: an all-zero identifier, kind 0
HLE_FORCE("_ZN2nn2vi32GetDefaultDisplayPanelIdentifierEPNS0_15PanelIdentifierE", "built-in panel identity; the host has no panel: all zero", Result, h_vi_PanelId, void *out) {
    memset(out, 0, 0x18); return RES_OK;
}
// The Great Ace Attorney Chronicles (SDK 10) lists up to 10 displays before opening the default one (main+0x808f08) and does not read the list. DisplayInfo is 0x60 bytes:
//   name[0x40], bool has a layer limit at +0x40, then u64 layer count limit, largest layer width and height. One display, "Default", as Horizon lists for an application
HLE_FORCE("_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", "list displays; the host has one, \"Default\"", int, h_vi_ListDisp, void *out, int n) {
    if (n <= 0 || !out) return 0;
    uint8_t *p = out; memset(p, 0, 0x60); strcpy((char *)p, "Default");
    p[0x40] = 1; uint64_t lim[3] = {1, 1920, 1080}; memcpy(p + 0x48, lim, sizeof lim);
    return 1;
}
HLE_FORCE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", "destroy layer; host static object", void, h_vi_DestroyLayer, void *layer) { (void)layer; }
HLE_FORCE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", "close display; host static object", void, h_vi_CloseDisp, void *disp) { (void)disp; }
HLE_FORCE("_ZN2nn2vi8FinalizeEv", "display service shutdown; the host has no vi service", void, h_vi_Fini, void) {}
