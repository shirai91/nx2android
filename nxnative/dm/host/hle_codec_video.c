// nn::codec video decoders of the Switch 2's SDK, served on the host. The SDK sends every call to a decoder service the host does not have: Donkey Kong Bananza
// crashed in it (a null read in the sdk, x6 / x7 holding "video-fo" / "rame-cou") after its first punch, when its guide movie starts (Movie/*.webm, VP9 960x540).
// The protocol, as the SDK 20.5.19 implements it:
//   - the client makes one image pool per picture and registers it with the decoder under an index; registering makes the index acquirable (a queue 8 deep)
//     with a sync handle already reached;
//   - Release gives the buffer to the decoder and makes it acquirable again at once, with a new sync handle reached when a picture is in it; Acquire takes the
//     oldest index (3604 when none). The client waits on the handle (nn::gs::TryWaitNativeSync, or a sync point event), then reads the picture's properties
//     (3604 until filled) and the picture (ReadDecoderOutputImageNv12 / Rgba);
//   - Submit takes one packet with its time and an end-of-stream flag (3603 while the decoder cannot take more: the client retries on the submission-ready
//     event, a SystemEvent inside the decoder type).
// VP9 decodes with libvpx on the submitting thread; pictures go into released buffers in release order, up to NHOLD wait for one. VP8, H.264 and AV1 are
// refused at Initialize (red line), so the client takes its error path. The opaque types hold the host object's pointer at +0
#include "hle.h"
#include <pthread.h>
#include <stdbool.h>
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"   // (vpx_codec_vp9_dx)
#include "hle_yuv.h"

#define CV_R(desc) ((Result)(111 | (desc) << 9))   // nn::codec results
enum { CV_BAD = 2004, CV_BUSY = 3603, CV_EMPTY = 3604 };
enum { NBUF = 32, NQ = 8, NHOLD = 4, NDEC = 16, NSPT = 64 };
enum { EVENT_OFF = 0x100 };   // the submission-ready SystemEventType inside the decoder type (0x400 bytes)
#define MAGIC 0x56436d44u     // 'DmCV'
#define SYNC 0x53436d44u      // 'DmCS'

typedef struct { uint32_t magic, dec, idx, gen; } cv_sync;   // nn::gs::NativeSyncHandle (16 bytes): a host one names a decoder's buffer and its release
typedef struct { uint16_t w, h; uint32_t fmt, mode, magic, pad[2]; } cv_pref;   // VideoDecoderImagePreference as the host makes it
typedef struct { uint32_t magic; uint16_t w, h; uint32_t n; vpx_image_t **img; } cv_pool;
typedef struct {
    uint32_t magic, id; pthread_mutex_t mu; vpx_codec_ctx_t vpx; int vpx_ok; void *event; uint64_t submits, pictures;
    struct { cv_pool *pool; uint32_t pidx, gen, filled_gen; uint8_t reg, filled, eos; uint16_t w, h; int64_t ts; } b[NBUF];
    uint32_t acq[NQ], nacq, want[NBUF], nwant;   // acquirable indices; released ones waiting for a picture
    vpx_image_t *held[NHOLD]; int64_t held_ts[NHOLD]; uint8_t held_eos[NHOLD]; int nheld, eos_pending;
} cv_dec;

static cv_dec *g_dec[NDEC];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // lock order: g_mu, a decoder's mu, g_spt_mu
static cv_dec *dec_of(void *t) { cv_dec *d = t ? *(cv_dec **)t : NULL; return d && d->magic == MAGIC ? d : NULL; }
static cv_pool *pool_of(const void *t) { cv_pool *p = t ? *(cv_pool *const *)t : NULL; return p && p->magic == MAGIC ? p : NULL; }
static void q_drop(uint32_t *q, uint32_t *n, uint32_t i) { uint32_t k = 0; for (uint32_t j = 0; j < *n; j++) if (q[j] != i) q[k++] = q[j]; *n = k; }
static void *os_sym(const char *n) { void *p = guest_sym(n); if (!p) die("nn::codec: missing SDK symbol %s", n); return p; }
static void destroy_event(void *e) { static void (*f)(void *); if (!f) f = os_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"); f(e); }

// ---- sync point events (nn::syncpt::sdk) ----
// The player's worker waits on one in a MultiWait for the handle of the picture it expects. Their library is set up by the graphics initialisation the host
// replaces (CreateSyncPointEvent jumped through a null table). The event type holds a SystemEvent the host binds at +0 and the awaited handle at +0x40
typedef struct { uint32_t magic, armed; cv_sync want; } cv_spt;
#define SPT(e) ((cv_spt *)((uint8_t *)(e) + 0x40))
static void *g_spt[NSPT];
static pthread_mutex_t g_spt_mu = PTHREAD_MUTEX_INITIALIZER;
static void spt_disarm(void *e) { pthread_mutex_lock(&g_spt_mu); for (int k = 0; k < NSPT; k++) if (g_spt[k] == e) g_spt[k] = NULL; pthread_mutex_unlock(&g_spt_mu); SPT(e)->armed = 0; }
static void spt_fire(const cv_dec *d, uint32_t i) {   // buffer i of d is filled (decoder lock held)
    pthread_mutex_lock(&g_spt_mu);
    for (int k = 0; k < NSPT; k++) { cv_spt *s = g_spt[k] ? SPT(g_spt[k]) : NULL;
        if (s && s->want.dec == d->id && s->want.idx == i && d->b[i].filled_gen >= s->want.gen) { s->armed = 0; hle_signal_system_event(g_spt[k]); g_spt[k] = NULL; } }
    pthread_mutex_unlock(&g_spt_mu);
}
static bool reached(const cv_sync *s) {   // a handle the host did not make reads as reached
    if (s->magic != SYNC || !s->gen) return true;
    bool r = true;
    pthread_mutex_lock(&g_mu);   // held until the decoder's lock is: Finalize cannot free it in between
    cv_dec *d = s->dec && s->dec <= NDEC ? g_dec[s->dec - 1] : NULL;
    if (d && s->idx < NBUF) { pthread_mutex_lock(&d->mu); r = d->b[s->idx].filled && d->b[s->idx].filled_gen >= s->gen; pthread_mutex_unlock(&d->mu); }
    pthread_mutex_unlock(&g_mu);
    return r;
}
HLE_FORCE("_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", "decoder sync handles are the host's", bool, h_gs_trywait, const cv_sync *s) { return reached(s); }
HLE_FORCE("_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", "a host-bound SystemEvent inside the event type", Result, h_spt_create, void *e) {
    memset(e, 0, 0x58); hle_bind_system_event(e, 0); SPT(e)->magic = SYNC; return 0;
}
HLE_FORCE("_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE", "links the event's SystemEvent", void, h_spt_holder, void *holder, void *e) {
    static void (*f)(void *, void *); if (!f) f = os_sym("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE");
    f(holder, e);
}
HLE_FORCE("_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE", "signalled when the decoder fills the awaited buffer", Result, h_spt_request, void *e, const cv_sync *s) {
    spt_disarm(e); hle_clear_system_event(e);
    SPT(e)->want = *s; SPT(e)->armed = 1;
    pthread_mutex_lock(&g_spt_mu); int k = 0; while (k < NSPT && g_spt[k]) k++; if (k < NSPT) g_spt[k] = e; pthread_mutex_unlock(&g_spt_mu);
    if (k == NSPT) LOG("[RED] nn::syncpt: more than %d armed events: signalled at once", NSPT);
    if (k == NSPT || reached(s)) { spt_disarm(e); hle_signal_system_event(e); }
    return 0;
}
HLE_FORCE("_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", "disarms the event", void, h_spt_cancel, void *e) { spt_disarm(e); hle_clear_system_event(e); }
HLE_FORCE("_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", "destroys the event's SystemEvent", void, h_spt_destroy, void *e) {
    spt_disarm(e); destroy_event(e); SPT(e)->magic = 0;
}

// ---- image pools: the host keeps each decoded picture (I420) ----
HLE_FORCE("_ZN2nn5codec5video10InitializeEv", "the decoder service is the host's", Result, h_cv_init, void) { return 0; }
HLE_FORCE("_ZN2nn5codec5video8FinalizeEv", "the decoder service is the host's", Result, h_cv_fin, void) { return 0; }
HLE_FORCE("_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", "carries the size and format to the host's pool",
          cv_pref, h_cv_pref, uint16_t w, uint16_t h, uint32_t fmt, uint32_t mode) { return (cv_pref){w, h, fmt, mode, MAGIC, {0, 0}}; }
HLE_FORCE("_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", "the host keeps the pictures: a page of game memory",
          Result, h_cv_poolsize, uint64_t *size, uint64_t *align, const cv_pref *p, uint32_t n) { (void)p; (void)n; *size = 0x1000; *align = 0x1000; return 0; }
HLE_FORCE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEjNS0_9CodecModeE", "a host pool of decoded pictures",
          Result, h_cv_pool, void *t, void *mem, uint64_t size, const cv_pref *p, uint32_t n, uint32_t mode) {
    (void)mem; (void)size; (void)mode;
    cv_pool *o = calloc(1, sizeof *o); o->magic = MAGIC; o->w = p->w; o->h = p->h; o->n = n ? n : 1; o->img = calloc(o->n, sizeof *o->img);
    *(cv_pool **)t = o;
    return 0;
}
HLE_FORCE("_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", "frees the host pool", void, h_cv_poolfin, void *t) {
    cv_pool *p = pool_of(t); if (!p) return;
    pthread_mutex_lock(&g_mu);   // a buffer still registered with this pool keeps no pool to write into
    for (int k = 0; k < NDEC; k++) if (g_dec[k]) { pthread_mutex_lock(&g_dec[k]->mu); for (int i = 0; i < NBUF; i++) if (g_dec[k]->b[i].pool == p) g_dec[k]->b[i].pool = NULL; pthread_mutex_unlock(&g_dec[k]->mu); }
    pthread_mutex_unlock(&g_mu);
    for (uint32_t i = 0; i < p->n; i++) if (p->img[i]) vpx_img_free(p->img[i]);
    free(p->img); p->magic = 0; free(p); *(cv_pool **)t = NULL;
}
HLE_FORCE("_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", "the GPU route of decoded pictures is not served", uint32_t, h_cv_native, const void *t, uint32_t i) {
    (void)t; (void)i; static int once; if (!once++) LOG("[RED] nn::codec: a native image handle was asked for: the GPU route of decoded pictures is not served"); return 0;
}
static const vpx_image_t *picture(const void *t, uint32_t i) { const cv_pool *p = pool_of(t); return p && i < p->n ? p->img[i] : NULL; }
static int whole(const char *what, uint16_t x, uint16_t y) { if (!x && !y) return 1; static int once; if (!once++) LOG("[RED] nn::codec::%s of a region at %u,%u: only whole pictures are served", what, x, y); return 0; }
HLE_FORCE("_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", "the host's picture as RGBA",
          Result, h_cv_rgba, const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *dst, uint64_t stride, uint64_t size) {
    const vpx_image_t *im = picture(t, i);
    if (!im || !whole("ReadDecoderOutputImageRgba", x, y) || stride != (uint64_t)w * 4 || size < (uint64_t)w * h * 4) { memset(dst, 0, size); return 0; }
    dm_yuv_rgba(im, dst, w, h); return 0;
}
HLE_FORCE("_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", "the host's picture as NV12",
          Result, h_cv_nv12, const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t *ydst, uint64_t ystride, uint64_t ysize, uint8_t *uvdst, uint64_t uvstride, uint64_t uvsize) {
    const vpx_image_t *im = picture(t, i);
    uint64_t ny = (uint64_t)w * h;
    if (!im || !whole("ReadDecoderOutputImageNv12", x, y) || ystride != w || uvstride != w || ysize < ny || uvsize < ny / 2) { memset(ydst, 16, ysize); memset(uvdst, 128, uvsize); return 0; }
    if (uvdst == ydst + ny) { dm_yuv_nv12(im, ydst, w, h); return 0; }
    uint8_t *tmp = malloc(ny * 3 / 2); if (!tmp) return 0;
    dm_yuv_nv12(im, tmp, w, h); memcpy(ydst, tmp, ny); memcpy(uvdst, tmp + ny, ny / 2); free(tmp);
    return 0;
}

// ---- decoders ----
static void deliver(cv_dec *d) {   // held pictures and a pending end of stream into released buffers, in order (decoder lock held)
    while (d->nwant && (d->nheld || d->eos_pending)) {
        uint32_t i = d->want[0]; q_drop(d->want, &d->nwant, i);
        typeof(d->b[0]) *b = &d->b[i];
        if (d->nheld) {
            vpx_image_t *im = d->held[0]; b->ts = d->held_ts[0]; b->eos = d->held_eos[0];
            d->nheld--; memmove(d->held, d->held + 1, sizeof *d->held * d->nheld); memmove(d->held_ts, d->held_ts + 1, sizeof *d->held_ts * d->nheld); memmove(d->held_eos, d->held_eos + 1, d->nheld);
            b->w = (uint16_t)im->d_w; b->h = (uint16_t)im->d_h;
            if (b->pool && b->pidx < b->pool->n) { if (b->pool->img[b->pidx]) vpx_img_free(b->pool->img[b->pidx]); b->pool->img[b->pidx] = im; } else vpx_img_free(im);
            if (!d->pictures++) LOG("[codec] %s decoder %u: first picture %ux%u", "VP9", d->id, b->w, b->h);
        } else { b->w = b->h = 0; b->ts = 0; b->eos = 1; d->eos_pending = 0; }
        b->filled = 1; b->filled_gen = b->gen;
        spt_fire(d, i);
    }
    if (d->nheld < NHOLD) hle_signal_system_event(d->event); else hle_clear_system_event(d->event);
}
static void reopen(cv_dec *d) {   // a new stream: decoding starts again at the next key frame
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    vpx_codec_dec_cfg_t cfg = {.threads = 2};
    d->vpx_ok = vpx_codec_dec_init(&d->vpx, vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK;
    if (!d->vpx_ok) LOG("[RED] nn::codec: libvpx would not start a decoder");
}
static Result dec_init(void *t, uint16_t w, uint16_t h) {
    cv_dec *d = calloc(1, sizeof *d); d->magic = MAGIC; pthread_mutex_init(&d->mu, NULL);
    reopen(d);
    pthread_mutex_lock(&g_mu); for (int i = 0; i < NDEC && !d->id; i++) if (!g_dec[i]) { g_dec[i] = d; d->id = (uint32_t)i + 1; } pthread_mutex_unlock(&g_mu);
    if (!d->id) die("nn::codec: more than %d video decoders", NDEC);
    d->event = (uint8_t *)t + EVENT_OFF; hle_bind_system_event(d->event, 0); hle_signal_system_event(d->event);
    *(cv_dec **)t = d;
    LOG("[codec] VP9 decoder %u: %ux%u, decoded with libvpx on the host", d->id, w, h);
    return 0;
}
static Result dec_fin(void *t) {
    cv_dec *d = dec_of(t); if (!d) return 0;
    pthread_mutex_lock(&g_mu); g_dec[d->id - 1] = NULL; pthread_mutex_lock(&d->mu); pthread_mutex_unlock(&d->mu); pthread_mutex_unlock(&g_mu);
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    // the event goes with the decoder, as in the SDK: kept bound, a decoder made again in the same (cleared) memory would find it "bound" and get none
    destroy_event(d->event);
    pthread_mutex_destroy(&d->mu); d->magic = 0; free(d); *(cv_dec **)t = NULL;
    return 0;
}
static Result dec_reg(void *t, uint32_t i, void *pool, uint32_t pidx) {
    cv_dec *d = dec_of(t); if (!d || i >= NBUF) return CV_R(CV_BAD);
    pthread_mutex_lock(&d->mu);
    d->b[i].pool = pool_of(pool); d->b[i].pidx = pidx; d->b[i].reg = 1; d->b[i].filled = 0; d->b[i].gen = d->b[i].filled_gen = 0;
    if (d->nacq < NQ) d->acq[d->nacq++] = i; else LOG("[RED] nn::codec: acquirable queue full at register %u", i);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result dec_unreg(void *t, uint32_t i) {
    cv_dec *d = dec_of(t); if (!d || i >= NBUF) return CV_R(CV_BAD);
    pthread_mutex_lock(&d->mu);
    d->b[i].reg = d->b[i].filled = 0; d->b[i].pool = NULL; d->b[i].gen++;
    q_drop(d->acq, &d->nacq, i); q_drop(d->want, &d->nwant, i);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result dec_reset(void *t) {   // Stop / Flush: undelivered pictures and a pending end of stream are dropped
    cv_dec *d = dec_of(t); if (!d) return 0;
    pthread_mutex_lock(&d->mu);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    d->nheld = d->eos_pending = 0; reopen(d); hle_signal_system_event(d->event);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result dec_submit(uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) {
    cv_dec *d = dec_of(t); if (!d) return CV_R(CV_BAD);
    pthread_mutex_lock(&d->mu);
    if (d->nheld >= NHOLD) { hle_clear_system_event(d->event); pthread_mutex_unlock(&d->mu); return CV_R(CV_BUSY); }
    int got = 0;
    if (size && d->vpx_ok) {
        if (vpx_codec_decode(&d->vpx, data, (unsigned)size, NULL, 0) != VPX_CODEC_OK) { static int n; if (n++ < 4) LOG("[RED] nn::codec: %s", vpx_codec_error(&d->vpx)); }
        vpx_codec_iter_t it = NULL; const vpx_image_t *im;
        while ((im = vpx_codec_get_frame(&d->vpx, &it))) {   // an owned copy: libvpx reuses its buffers at the next decode
            if (d->nheld >= NHOLD) { static int n; if (n++ < 4) LOG("[RED] nn::codec: a packet gave more pictures than the host holds: one dropped"); continue; }
            vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, im->d_w, im->d_h, 1); if (!c) continue;
            c->cs = im->cs; c->range = im->range;
            for (int p = 0; p < 3; p++) for (unsigned y = 0, ph = p ? (im->d_h + 1) / 2 : im->d_h; y < ph; y++)
                memcpy(c->planes[p] + (size_t)y * c->stride[p], im->planes[p] + (size_t)y * im->stride[p], p ? (im->d_w + 1) / 2 : im->d_w);
            d->held[d->nheld] = c; d->held_ts[d->nheld] = ts; d->held_eos[d->nheld++] = 0; got++;
        }
    }
    if (eos) { if (got) d->held_eos[d->nheld - 1] = 1; else d->eos_pending = 1; }
    *out = ++d->submits;
    deliver(d);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result dec_acquire(uint32_t *idx, cv_sync *sync, void *t) {
    cv_dec *d = dec_of(t); if (!d) return CV_R(CV_BAD);
    pthread_mutex_lock(&d->mu);
    Result r = d->nacq ? 0 : CV_R(CV_EMPTY);
    if (!r) { uint32_t i = d->acq[0]; q_drop(d->acq, &d->nacq, i); *idx = i; *sync = (cv_sync){SYNC, d->id, i, d->b[i].gen}; }
    pthread_mutex_unlock(&d->mu);
    return r;
}
static void dec_release(void *t, uint32_t i) {
    cv_dec *d = dec_of(t); if (!d || i >= NBUF || !d->b[i].reg) { LOG("[RED] nn::codec: release of buffer %u, which is not registered", i); return; }
    pthread_mutex_lock(&d->mu);
    d->b[i].gen++; d->b[i].filled = 0;
    if (d->nwant < NBUF) d->want[d->nwant++] = i;
    if (d->nacq < NQ) d->acq[d->nacq++] = i; else LOG("[RED] nn::codec: acquirable queue full at release %u", i);
    deliver(d);
    pthread_mutex_unlock(&d->mu);
}
static Result dec_props(uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, bool *eos, void *t, uint32_t i) {
    cv_dec *d = dec_of(t); if (!d || i >= NBUF) return CV_R(CV_BAD);
    pthread_mutex_lock(&d->mu);
    int ok = d->b[i].filled;
    if (ok) { *w = *cw = d->b[i].w; *h = *ch = d->b[i].h; *ts = d->b[i].ts; *eos = d->b[i].eos; }
    pthread_mutex_unlock(&d->mu);
    return ok ? 0 : CV_R(CV_EMPTY);
}

#define CV_TYPE "EPNS0_14Vp9DecoderTypeE"
#define CV_INIT_ARGS "ttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE"
HLE_FORCE("_ZN2nn5codec29CalculateVp9DecoderWorkBufferEPmS1_mtt", "the host decoder needs no work memory", Result, h_vp9_work, uint64_t *size, uint64_t *align) { *size = *align = 0x1000; return 0; }
HLE_FORCE("_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEPvmm" CV_INIT_ARGS, "decoded on the host", Result, h_vp9_init, void *t, void *work, uint64_t wsize, uint64_t bs, uint16_t w, uint16_t h) { (void)work; (void)wsize; (void)bs; return dec_init(t, w, h); }
HLE_FORCE("_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEm" CV_INIT_ARGS, "decoded on the host", Result, h_vp9_init2, void *t, uint64_t bs, uint16_t w, uint16_t h) { (void)bs; return dec_init(t, w, h); }
HLE_FORCE("_ZN2nn5codec18FinalizeVp9Decoder" CV_TYPE, "frees the host decoder", Result, h_vp9_fin, void *t) { return dec_fin(t); }
HLE_FORCE("_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", "a buffer of the host decoder, acquirable at once", Result, h_vp9_reg, void *t, uint32_t i, void *pool, uint32_t pidx) { return dec_reg(t, i, pool, pidx); }
HLE_FORCE("_ZN2nn5codec26UnregisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEj", "the buffer leaves the host decoder", Result, h_vp9_unreg, void *t, uint32_t i) { return dec_unreg(t, i); }
HLE_FORCE("_ZN2nn5codec15StartVp9Decoder" CV_TYPE, "the host decoder decodes on submission", Result, h_vp9_start, void *t) { (void)t; return 0; }
HLE_FORCE("_ZN2nn5codec14StopVp9Decoder" CV_TYPE, "drops undelivered pictures", Result, h_vp9_stop, void *t) { return dec_reset(t); }
HLE_FORCE("_ZN2nn5codec15FlushVp9Decoder" CV_TYPE, "drops undelivered pictures", Result, h_vp9_flush, void *t) { return dec_reset(t); }
HLE_FORCE("_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Vp9DecoderTypeE", "signalled while the host decoder takes packets", Result, h_vp9_event, void **out, void *t) {
    cv_dec *d = dec_of(t); *out = d ? d->event : NULL; return d ? 0 : CV_R(CV_BAD); }
HLE_FORCE("_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb", "decodes the packet on the submitting thread", Result, h_vp9_submit, uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) {
    return dec_submit(out, t, data, size, ts, eos); }
HLE_FORCE("_ZN2nn5codec36SubmitEncryptedVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKNS0_18EncryptionPropertyEPKvmNS_8TimeSpanEb", "protected streams are not served", Result, h_vp9_enc, void) {
    static int once; if (!once++) LOG("[RED] nn::codec: encrypted input is not served"); return CV_R(CV_BAD); }
HLE_FORCE("_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE", "the oldest acquirable buffer and its sync handle", Result, h_vp9_acq, uint32_t *idx, cv_sync *sync, void *t) { return dec_acquire(idx, sync, t); }
HLE_FORCE("_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE", "the buffer waits for a picture and is acquirable again", void, h_vp9_rel, void *t, uint32_t i) { dec_release(t, i); }
HLE_FORCE("_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj", "the filled buffer's picture size, time and end of stream", Result, h_vp9_props,
          uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, bool *eos, void *t, uint32_t i) { return dec_props(w, h, cw, ch, ts, eos, t, i); }

// The other codecs: Initialize fails, so the client takes its error path and calls nothing else of theirs
static Result cv_refuse(const char *codec) { LOG("[RED] nn::codec: a %s decoder: not served, Initialize fails", codec); return CV_R(CV_BAD); }
HLE_FORCE("_ZN2nn5codec29CalculateVp8DecoderWorkBufferEPmS1_mtt", "no decoder to size", Result, h_vp8_work, uint64_t *size, uint64_t *align) { *size = *align = 0x1000; return 0; }
HLE_FORCE("_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEPvmm" CV_INIT_ARGS, "not served", Result, h_vp8_init, void) { return cv_refuse("Vp8"); }
HLE_FORCE("_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEm" CV_INIT_ARGS, "not served", Result, h_vp8_init2, void) { return cv_refuse("Vp8"); }
HLE_FORCE("_ZN2nn5codec18FinalizeVp8DecoderEPNS0_14Vp8DecoderTypeE", "nothing was made", Result, h_vp8_fin, void) { return 0; }
HLE_FORCE("_ZN2nn5codec29CalculateAv1DecoderWorkBufferEPmS1_mtt", "no decoder to size", Result, h_av1_work, uint64_t *size, uint64_t *align) { *size = *align = 0x1000; return 0; }
HLE_FORCE("_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEPvmm" CV_INIT_ARGS, "not served", Result, h_av1_init, void) { return cv_refuse("Av1"); }
HLE_FORCE("_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEm" CV_INIT_ARGS, "not served", Result, h_av1_init2, void) { return cv_refuse("Av1"); }
HLE_FORCE("_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE", "nothing was made", Result, h_av1_fin, void) { return 0; }
HLE_FORCE("_ZN2nn5codec30CalculateH264DecoderWorkBufferEPmS1_mtt", "no decoder to size", Result, h_h264_work, uint64_t *size, uint64_t *align) { *size = *align = 0x1000; return 0; }
HLE_FORCE("_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEPvmm" CV_INIT_ARGS "PKvm", "not served", Result, h_h264_init, void) { return cv_refuse("H264"); }
HLE_FORCE("_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEm" CV_INIT_ARGS "PKvm", "not served", Result, h_h264_init2, void) { return cv_refuse("H264"); }
HLE_FORCE("_ZN2nn5codec19FinalizeH264DecoderEPNS0_15H264DecoderTypeE", "nothing was made", Result, h_h264_fin, void) { return 0; }
