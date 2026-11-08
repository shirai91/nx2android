// Android port: H.264 for the SDK's movie library (dm/host/hle_movie_avc.h) on MediaCodec, VideoToolbox's counterpart (dm/host/hle_movie_avc.c).
// Unpacking's only movie is its 5 s Humble Games logo (StreamingAssets/HumbleGames_LogoIntro-2021_5sec_1080p30.mp4, H.264 Main 1080p, no audio), which Unity 2019.4 plays through
// the SDK's decoder in CPU mode (NV12 out); with no pictures the intro ended at once and the game crashed right after it (main+0x8a74d0).
// The decoder is configured from SPS / PPS (csd-0 / csd-1 with start codes, as Android's own movie players pass them) and fed one access unit per call, in Annex B.
// MediaCodec hands out pictures in presentation order, a few frames after their input (the hardware decoder's pipeline): mavc_take waits briefly for one while inputs are in flight,
// and at the end of the stream sends an end-of-stream input and collects every picture still in the codec. Pictures leave the codec as compact NV12 copies (the output buffer's stride,
// slice height and crop come from its format: Qualcomm's decoder pads rows to 128 bytes and planes to 32 rows), so a buffer goes back to the codec at once
#include "dm.h"
#include "hle_movie_avc.h"
#include <stdlib.h>
#include <string.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define QMAX 16
#define COLOR_YUV420_PLANAR 19   // MediaCodecInfo.CodecCapabilities: I420 (the software decoder's); every other format is read as semi-planar (21, Qualcomm's 0x7fa30c04 venus NV12)
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty;
    AMediaCodec *mc; int eos_sent;
    int fw, fh, fstride, fslice, fcolor, cl, ct, cr, cb;   // output format: size, stride, slice height, colour format, crop rectangle (inclusive)
    int fstandard, ffull;                                  // Android color-standard and full-range flag for the current output format
    long nin, nout;                                        // inputs queued and pictures out since the codec started (or was flushed)
    struct { uint8_t *nv12; int w, h, standard, full; int64_t pts; } q[QMAX]; int nq;
    uint8_t *au; size_t aucap;   // Annex B copy of the access unit being decoded
    int red, rgba;               // rgba: pictures as 4-byte RGBA (native texture mode) instead of NV12
    // The codec's timestamps: the game's while they increase, else the last one + 1 µs, each mapped back to the game's on output (pmap). 13 Sentinels: Aegis Rim sends every
    // packet of its 1024x576 movies with time 0, and c2.qti.avc.decoder gave no picture for 81 packets and then took no more input. With made-up timestamps the codec's
    // own output order stands (synth: the queue is not sorted)
    int64_t last_in; int synth; struct { int64_t c, g; } pmap[64]; int npmap;
};

mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); a->last_in = INT64_MIN; return a; }
static void close_codec(mavc *a) { if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); a->mc = NULL; } a->nin = a->nout = 0; a->eos_sent = 0; a->last_in = INT64_MIN; a->npmap = 0; }
void mavc_drop(mavc *a) {
    for (int i = 0; i < a->nq; i++) free(a->q[i].nv12);
    a->nq = 0;
    if (a->mc) { AMediaCodec_flush(a->mc); a->nin = a->nout = 0; a->eos_sent = 0; a->last_in = INT64_MIN; a->npmap = 0; }   // a seek: pictures in the codec belong to the old position
}
void mavc_free(mavc *a) { if (!a) return; mavc_drop(a); close_codec(a); free(a->au); free(a); }
int mavc_queued(const mavc *a) { return a->nq; }
int mavc_held(const mavc *a) { return a->nq + (a->mc && !a->eos_sent && a->nin > a->nout ? (int)(a->nin - a->nout) : 0); }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }

static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->dirty = 1;
}
// walk the NAL units of p (Annex B start codes, or else 4-byte lengths); fn gets each unit without its prefix (as hle_movie_avc.c)
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {   // AVCDecoderConfigurationRecord (the MP4 avcC box)
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}

static void set_csd(AMediaFormat *f, const char *key, const uint8_t *nal, int n) {
    uint8_t b[4 + 512] = {0, 0, 0, 1}; memcpy(b + 4, nal, (size_t)n); AMediaFormat_setBuffer(f, key, b, (size_t)n + 4);
}
static int open_codec(mavc *a) {
    close_codec(a);
    if (!a->nsps || !a->npps) { if (!a->red++) LOG("[RED] movie: AVC has no SPS / PPS, outputting black frames"); return 0; }
    a->mc = AMediaCodec_createDecoderByType("video/avc");
    if (!a->mc) { if (!a->red++) LOG("[RED] movie: MediaCodec has no AVC decoder, outputting black frames"); return 0; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, 1920); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, 1080);   // the decoder takes the real size from the SPS (an output format change)
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 4 << 20);
    set_csd(f, AMEDIAFORMAT_KEY_CSD_0, a->sps, a->nsps); set_csd(f, AMEDIAFORMAT_KEY_CSD_1, a->pps, a->npps);
    media_status_t st = AMediaCodec_configure(a->mc, f, NULL, NULL, 0);
    if (st == AMEDIA_OK) st = AMediaCodec_start(a->mc);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK) { if (!a->red++) LOG("[RED] movie: MediaCodec AVC decoder refused its configuration (%d), outputting black frames", (int)st); AMediaCodec_delete(a->mc); a->mc = NULL; return 0; }
    char *name = NULL; AMediaCodec_getName(a->mc, &name);
    LOG("[movie] AVC: MediaCodec decoder %s, profile %d", name ? name : "?", a->nsps > 1 ? a->sps[1] : 0);
    if (name) AMediaCodec_releaseName(a->mc, name);
    a->fw = a->fh = 0; a->dirty = 0; return 1;
}
static void read_format(mavc *a) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(a->mc); int32_t v;
    a->fw = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &v) ? v : 0; a->fh = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &v) ? v : 0;
    a->fstride = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) && v >= a->fw ? v : a->fw;
    a->fslice = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &v) && v >= a->fh ? v : a->fh;
    a->fcolor = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v) ? v : 0;
    a->fstandard = AMediaFormat_getInt32(f, "color-standard", &v) ? v : 0;
    a->ffull = AMediaFormat_getInt32(f, "color-range", &v) && v == 1;
    int32_t l, t, r, b;
    if (AMediaFormat_getRect(f, AMEDIAFORMAT_KEY_DISPLAY_CROP, &l, &t, &r, &b) && r >= l && b >= t && r < a->fw && b < a->fh) { a->cl = l; a->ct = t; a->cr = r; a->cb = b; }
    else { a->cl = a->ct = 0; a->cr = a->fw - 1; a->cb = a->fh - 1; }
    LOG("[movie] AVC: output %dx%d stride %d slice height %d colour format %#x crop %d,%d-%d,%d color-standard %d full-range %d", a->fw, a->fh, a->fstride, a->fslice, a->fcolor, a->cl, a->ct, a->cr, a->cb, a->fstandard, a->ffull);
    AMediaFormat_delete(f);
}
// one output buffer → a compact NV12 picture of the crop rectangle, queued in presentation order
static void keep_picture(mavc *a, const uint8_t *src, size_t n, int64_t pts) {
    int w = (a->cr - a->cl + 1) & ~1, h = (a->cb - a->ct + 1) & ~1, s = a->fstride;
    size_t uvoff = (size_t)s * a->fslice;
    int planar = a->fcolor == COLOR_YUV420_PLANAR;
    size_t need = planar ? uvoff + uvoff / 2 : uvoff + (size_t)s * (a->fslice / 2);
    if (w <= 0 || h <= 0 || n < need - (size_t)(s - a->fw)) { if (!a->red++) LOG("[RED] movie: AVC output buffer of %zu bytes is smaller than its %dx%d picture (stride %d, slice height %d)", n, a->fw, a->fh, s, a->fslice); return; }
    uint8_t *o = malloc((size_t)w * h * 3 / 2); if (!o) die("movie: AVC picture allocation failed");
    for (int r = 0; r < h; r++) memcpy(o + (size_t)r * w, src + (size_t)(a->ct + r) * s + a->cl, (size_t)w);
    uint8_t *ouv = o + (size_t)w * h;
    if (planar) {   // I420: U plane then V plane, half stride
        const uint8_t *u = src + uvoff, *v = u + (size_t)(s / 2) * (a->fslice / 2);
        for (int r = 0; r < h / 2; r++) {
            const uint8_t *ur = u + (size_t)(a->ct / 2 + r) * (s / 2) + a->cl / 2, *vr = v + (size_t)(a->ct / 2 + r) * (s / 2) + a->cl / 2; uint8_t *d = ouv + (size_t)r * w;
            for (int x = 0; x < w / 2; x++) { d[2 * x] = ur[x]; d[2 * x + 1] = vr[x]; }
        }
    } else for (int r = 0; r < h / 2; r++) memcpy(ouv + (size_t)r * w, src + uvoff + (size_t)(a->ct / 2 + r) * s + (a->cl & ~1), (size_t)w);
    if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC picture queue full, dropping the oldest picture"); free(a->q[0].nv12); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
    for (int m = 0; m < a->npmap; m++) if (a->pmap[m].c == pts) { pts = a->pmap[m].g; a->pmap[m] = a->pmap[--a->npmap]; break; }   // the game's own timestamp
    int k = a->nq; while (!a->synth && k > 0 && a->q[k - 1].pts > pts) { a->q[k] = a->q[k - 1]; k--; }
    a->q[k].nv12 = o; a->q[k].w = w; a->q[k].h = h; a->q[k].pts = pts;
    // Metadata travels with the picture through sorting and later format changes.
    int standard = a->fstandard;
    a->q[k].standard = standard == 1 || standard == 2 || standard == 4 || standard == 6 ? standard : (h >= 720 ? 1 : 4);
    a->q[k].full = a->ffull; a->nq++;
}
// collect the pictures the codec has ready, waiting up to wait_us for the first; 1 = the end-of-stream output came
static int drain(mavc *a, int64_t wait_us) {
    for (;;) {
        AMediaCodecBufferInfo bi; ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &bi, wait_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { read_format(a); continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return 0;   // AMEDIACODEC_INFO_TRY_AGAIN_LATER
        if (!a->fw) read_format(a);
        size_t n = 0; uint8_t *b = AMediaCodec_getOutputBuffer(a->mc, (size_t)i, &n);
        if (b && bi.size > 0) { keep_picture(a, b + bi.offset, (size_t)bi.size, bi.presentationTimeUs); a->nout++; }
        AMediaCodec_releaseOutputBuffer(a->mc, (size_t)i, false);
        if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return 1;
        wait_us = 0;
    }
}
typedef struct { mavc *a; size_t len; } au_ctx;
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx; mavc *a = c->a; int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(a, nal, n); return; }
    if (t == 9) return;   // access unit delimiter
    if (c->len + 4 + n > a->aucap) { a->aucap = (c->len + 4 + n) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) die("movie: AVC buffer allocation failed"); }
    uint8_t *o = a->au + c->len; o[0] = o[1] = o[2] = 0; o[3] = 1; memcpy(o + 4, nal, n); c->len += 4 + n;
}
static ssize_t input_slot(mavc *a) {   // a free input buffer; while none is free, the pictures holding the codec up are collected
    for (int tries = 0; tries < 50; tries++) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 2000);
        if (i >= 0) return i;
        drain(a, 0);
    }
    return -1;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0}; each_nal(p, (size_t)n, au_add, &c);
    if (!c.len) return 0;
    if ((a->dirty || !a->mc || a->eos_sent) && !open_codec(a)) return -1;
    ssize_t i = input_slot(a);
    if (i < 0) { static long nd; long k = ++nd; if (!(k & (k - 1))) LOG("[RED] movie: MediaCodec AVC decoder has no free input buffer, a packet is dropped (%ld so far)", k); return -1; }
    size_t cap = 0; uint8_t *b = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap);
    if (!b || cap < c.len) { if (!a->red++) LOG("[RED] movie: AVC access unit of %zu bytes does not fit MediaCodec's %zu-byte input buffer", c.len, cap); AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, (uint64_t)pts_us, 0); return -1; }
    memcpy(b, a->au, c.len);
    int64_t cpts = pts_us > a->last_in ? pts_us : a->last_in + 1; if (cpts != pts_us) a->synth = 1; a->last_in = cpts;
    if (a->npmap == 64) a->pmap[0] = a->pmap[--a->npmap];   // (outputs the codec never gave)
    a->pmap[a->npmap].c = cpts; a->pmap[a->npmap].g = pts_us; a->npmap++;
    if (AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, c.len, (uint64_t)cpts, 0) != AMEDIA_OK) { if (!a->red++) LOG("[RED] movie: MediaCodec refused an AVC access unit (first case)"); return -1; }
    a->nin++;
    drain(a, 0);
    return 0;
}
// the end of the stream: an end-of-stream input, then every picture still in the codec; the codec is opened afresh for the next stream (a loop restarts with Flush and new packets)
static void finish(mavc *a) {
    if (!a->mc || a->eos_sent) return;
    ssize_t i = input_slot(a);
    if (i >= 0) AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    a->eos_sent = 1;
    for (int k = 0; i >= 0 && k < 100 && !drain(a, 10000); k++) {}
    close_codec(a); a->eos_sent = 1;
}
// NV12 → RGBA using the picture's matrix/range. Android standards: 1=709, 2/4=601, 6=2020.
static void nv12_rgba(const uint8_t *y, const uint8_t *uv, int pw, int ph, uint8_t *dst, int w, int h, int standard, int full) {
    int cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    int rv = 409, gu = 100, gv = 208, bu = 516;   // ×256, 8-bit video range (luma × 298)
    if (standard == 1) { rv = 459; gu = 55; gv = 136; bu = 541; }
    if (standard == 6) { rv = 430; gu = 48; gv = 167; bu = 548; }
    if (full) { rv = rv * 224 / 255; gu = gu * 224 / 255; gv = gv * 224 / 255; bu = bu * 224 / 255; }
    if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    for (int r = 0; r < chh; r++) {
        const uint8_t *yr = y + (size_t)r * pw, *cr = uv + (size_t)(r / 2) * pw; uint8_t *o = dst + (size_t)r * w * 4;
        for (int x = 0; x < cw; x++) {
            int c = full ? 256 * yr[x] : 298 * (yr[x] - 16), u = cr[x & ~1] - 128, v = cr[(x & ~1) + 1] - 128;
            int R = (c + rv * v + 128) >> 8, G = (c - gu * u - gv * v + 128) >> 8, B = (c + bu * u + 128) >> 8;
            o[4 * x] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R); o[4 * x + 1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G); o[4 * x + 2] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B); o[4 * x + 3] = 255;
        }
    }
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (flush) finish(a);
    else if (!a->nq && a->mc && a->nin > a->nout) drain(a, 4000);   // a picture is in the codec's pipeline: give it a few milliseconds
    if (!a->nq) return 0;
    uint8_t *pic = a->q[0].nv12; int pw = a->q[0].w, ph = a->q[0].h; if (pts_us) *pts_us = a->q[0].pts;
    int standard = a->q[0].standard, full = a->q[0].full;
    memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    const uint8_t *uv = pic + (size_t)pw * ph;
    if (a->rgba) nv12_rgba(pic, uv, pw, ph, dst, w, h, standard, full);
    else {
        int cw = pw < w ? pw : w, chh = ph < h ? ph : h;
        if (cw < w || chh < h) { memset(dst, full ? 0 : 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, pic + (size_t)r * pw, (size_t)cw);
        for (int r = 0; r < chh / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, uv + (size_t)r * pw, (size_t)cw);
    }
    free(pic);
    return 1;
}
