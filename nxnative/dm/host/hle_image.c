// nn::image::JpegDecoder (Burnout Paradise Remastered, SDK 9): on the device the game decodes its loading-screen JPEG through this class (main+0x410b0c..0x410b54: ctor, SetImageData, SetPixelFormat(0), Analyze, GetAnalyzedDimension, Decode(dst, 0x40000, 4, work, 0x40000)).
// In the port the SDK's own Analyze fails: it forwards through a function-pointer table thunk (sdk+0x39398 bl 0x521320 -> [0x11d8d88] -> 0x520680 -> [0x11d8740]) whose entries the runtime never fills, so the helper returns an error, Analyze leaves the status at 1 (sdk+0x3939c: cbz w0, ... else ret with status unchanged), and Decode then hits its assert on the status — sdk+0x3948c/0x39494: "ldr w9,[x0,#8]; cmp w9,#1; b.le <nn::diag::Abort>" — which is the observed Break at sdk+0xfc4d0 with the game's return address main+0x410b58.
// Host: decode with stb_image (third_party/flip/tool). The guest object keeps the SDK's layout, only the host keeps extra state (objmap, keyed by the game's decoder pointer, like hle_codec's software Opus state):
//   +0x00 vtable (ctor stores _ZTV+0x10: sdk+0x392bc "add x8,x8,#0x10; str x8,[x0]"), +0x08 u32 status (0 unregistered / 1 registered / 2 analyzed),
//   +0x10 image ptr, +0x18 image size (SetImageData: "stp x1,x2,[x0,#0x10]"), +0x20 u32 resolution denominator (ctor stores the u64 1 there), +0x24 u32 pixel format,
//   +0x28 u64 analyzed dimension = width | height<<32 (Analyze: "stp x9,x8,[x19,#0x28]"; GetAnalyzedDimension: "ldr x0,[x0,#0x28]" — a { s32 width; s32 height; } returned packed in x0),
//   +0x30 u64 analyzed work buffer size (GetAnalyzedWorkBufferSize: "ldr x0,[x0,#0x30]"), +0x38 u8 has-exif.
// Signatures fixed by the sdk's code: ctor/dtor/SetImageData/SetPixelFormat return void (their stubs never write w0); Analyze and Decode return Result in w0; SetPixelFormat takes the format in w1 and accepts only values < 2 (sdk+0x39320 "cmp w1,#2; b.hs <Abort>"): 0 = 4 bytes/pixel with alpha 0xff appended, 1 = 3 bytes/pixel (decode_impl sdk+0x39670: fmt==1 -> 3*pitch rows, fmt==0 -> 4*pitch rows with "strb w24(0xff)" as the 4th byte, sdk+0x39790); Decode(self, void* dst, size_t dstSize, int stride, void* work, size_t workSize) — mangled EPvmiS2_m, and the stub (sdk+0x3949c..0x394bc) drops dstSize and forwards the int as the inner w2. That int is a row-pitch granularity in pixels, not bytes: decode_impl does "ldr w9,[sp,#0xc]; sdiv w8,w20,w9; msub w8,w8,w9,w20; ... csel" (sdk+0x396d8) computing pitch = width rounded up to a multiple of stride, then rows at dst + row*pitch*bpp (sdk+0x3973c "madd x12,x21,x12,x19" with x21 = 4*pitch for fmt 0). The game passes stride 4, format 0.
#include "hle.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "../third_party/flip/tool/stb_image.h"

// The game's JpegDecoder object, overlaid on the guest memory (the sdk's own field use above)
typedef struct {
    void *vt;                    // +0x00
    uint32_t status;             // +0x08
    uint32_t _pad0;
    const uint8_t *img;          // +0x10
    uint64_t img_size;           // +0x18
    uint32_t denom;              // +0x20
    uint32_t fmt;                // +0x24
    uint64_t dim;                // +0x28: width | height<<32
    uint64_t work;               // +0x30
    uint8_t has_exif;            // +0x38
    uint8_t _pad1[7];
    uint64_t exif_ptr;           // +0x40
    uint16_t exif_size;          // +0x48
    uint16_t _pad2;
} jdec_guest;
_Static_assert(sizeof(jdec_guest) == 0x50, "nn::image::JpegDecoder is 0x50 bytes");

// Host-side state per game decoder object
typedef struct {
    const uint8_t *img;
    size_t img_size;
    int fmt;
    int width, height;
    uint8_t *rgba;               // stb's decode, width*height*4, filled on the first Decode
} jdec_state;

static jdec_state *jdec_st(const void *self) {
    void *s = objmap_get(self);
    if (!s) die("JpegDecoder %p was not constructed by the host", self);
    return s;
}
static void jdec_write(const void *addr, size_t len) { dm_guest_write_prepare((void *)addr, len); }

HLE_FORCE("_ZN2nn5image11JpegDecoderC1Ev", "nn::image::JpegDecoder: the sdk's analyze/decode path is dead under the port (unfilled function-pointer table); the host decodes with stb_image",
          void, h_img_Ctor, void *self) {
    void *vt = guest_sym("_ZTVN2nn5image11JpegDecoderE");
    if (!vt) die("sdk lacks _ZTVN2nn5image11JpegDecoderE");
    jdec_state *old = objmap_del(self);
    if (old) { free(old->rgba); free(old); }
    jdec_guest *d = self;
    jdec_write(&d->vt, 0x50);
    d->vt = (char *)vt + 0x10;   // sdk+0x392bc: add x8,x8,#0x10; str x8,[x0]
    d->status = 0;               // sdk+0x392b8: str wzr,[x0,#8]
    d->denom = 1; d->fmt = 0;    // sdk+0x392c4: str x8(1),[x0,#0x20]
    d->img = NULL; d->img_size = 0; d->dim = 0; d->work = 0; d->has_exif = 0; d->exif_ptr = 0; d->exif_size = 0;
    jdec_state *s = calloc(1, sizeof *s);
    if (!s) die("JpegDecoder state for %p", self);
    objmap_put(self, s);
}

HLE_FORCE("_ZN2nn5image11JpegDecoderD1Ev", "nn::image::JpegDecoder: host state released (the sdk's dtor is a bare ret)", void, h_img_Dtor, void *self) {
    jdec_state *s = objmap_del(self);
    if (s) { free(s->rgba); free(s); }
}

HLE_FORCE("_ZN2nn5image11JpegDecoder12SetImageDataEPKvm", "nn::image::JpegDecoder: image bytes kept in the host's per-decoder state", void, h_img_SetImageData,
          void *self, const void *img, size_t size) {
    jdec_guest *d = self;
    jdec_state *s = jdec_st(self);
    jdec_write(d, sizeof *d);           // the whole object: the status at +0x08 is written below too
    d->img = img; d->img_size = size;   // sdk+0x392f4: stp x1,x2,[x0,#0x10]
    d->status = 1;                      // sdk+0x392f8: str w8(1),[x0,#8]
    free(s->rgba); s->rgba = NULL; s->img = img; s->img_size = size; s->width = s->height = 0;
    d->dim = 0; d->work = 0;
}

HLE_FORCE("_ZN2nn5image11JpegDecoder14SetPixelFormatENS0_11PixelFormatE", "nn::image::JpegDecoder: pixel format 0 = RGBA8888 (alpha 0xff), 1 = RGB888, as decode_impl writes them", void, h_img_SetPixelFormat,
          void *self, int fmt) {
    if ((unsigned)fmt >= 2) {   // the sdk aborts here (sdk+0x39324 b.hs); report and decode as RGBA
        static int once;
        if (!once++) LOG("JpegDecoder::SetPixelFormat(%d): the sdk accepts only 0 and 1; decoding as 0 (RGBA8888)", fmt);
        fmt = 0;
    }
    jdec_guest *d = self;
    jdec_state *s = jdec_st(self);
    jdec_write(d, sizeof *d);           // the whole object: the status at +0x08 is written below too
    d->fmt = (uint32_t)fmt;             // sdk+0x39334: str w1,[x0,#0x24]
    d->status = d->img ? 1 : 0;         // sdk+0x3932c..0x39338: cset w8,ne on [x0,#0x10]; str w8,[x0,#8]
    s->fmt = fmt;
}

HLE_FORCE("_ZN2nn5image11JpegDecoder7AnalyzeEv", "nn::image::JpegDecoder: header parsed with stb_image; status set to 2 (analyzed) and the dimension stored at +0x28", Result, h_img_Analyze,
          void *self) {
    jdec_guest *d = self;
    jdec_state *s = jdec_st(self);
    if (d->status <= 0 || !s->img) {   // the sdk aborts (sdk+0x39378 b.le); report the error instead
        LOG("JpegDecoder::Analyze: status %u, no image data", d->status);
        return RES_FAIL;
    }
    if (s->img_size > 0x7fffffff) return RES_FAIL;
    int w, h, comp;
    if (!stbi_info_from_memory(s->img, (int)s->img_size, &w, &h, &comp)) {
        LOG("JpegDecoder::Analyze: %zu bytes are not a JPEG", s->img_size);
        return RES_FAIL;
    }
    s->width = w; s->height = h;
    jdec_write(d, sizeof *d);           // the whole object: exif (+0x40, +0x48) and status (+0x08) are written below too
    d->dim = (uint64_t)(uint32_t)w | ((uint64_t)(uint32_t)h << 32);   // sdk+0x393bc: stp x9,x8,[x19,#0x28]
    // sdk+0x393c8..0x393dc: work size = analyzed size (w*h*3) + 24*width for format 0, unchanged for format 1
    d->work = (uint64_t)w * (uint64_t)h * 3 + (s->fmt == 0 ? 24ull * (uint64_t)w : 0);
    d->has_exif = 0; d->exif_ptr = 0; d->exif_size = 0;
    d->status = 2;                                                    // sdk+0x393fc: str w8(2),[x19,#8]
    LOG("JpegDecoder::Analyze: %dx%d (%d components, format %d, work %llu)", w, h, comp, s->fmt, (unsigned long long)d->work);
    return RES_OK;
}

HLE_FORCE("_ZN2nn5image11JpegDecoder6DecodeEPvmiS2_m", "nn::image::JpegDecoder::Decode(dst, dstSize, stride, work, workSize): stb_image decode, rows at dst + row*pitch*bpp with pitch = width rounded up to a multiple of stride pixels", Result, h_img_Decode,
          void *self, void *dst, size_t dst_size, int stride, void *work, size_t work_size) {
    (void)work; (void)work_size;   // stb decodes into its own buffer; the sdk's work buffer is only for its libjpeg rows
    jdec_guest *d = self;
    jdec_state *s = jdec_st(self);
    if (d->status <= 1) {   // the sdk's assert (sdk+0x39494 b.le Abort) — report the error instead of breaking
        LOG("JpegDecoder::Decode: status %u, image not analyzed", d->status);
        return RES_FAIL;
    }
    if (!s->rgba) {
        if (s->img_size > 0x7fffffff) return RES_FAIL;
        int w, h, comp;
        s->rgba = stbi_load_from_memory(s->img, (int)s->img_size, &w, &h, &comp, 4);
        if (!s->rgba) { LOG("JpegDecoder::Decode: stb_image failed on %zu bytes", s->img_size); return RES_FAIL; }
        s->width = w; s->height = h;
        jdec_write(&d->dim, 8);
        d->dim = (uint64_t)(uint32_t)w | ((uint64_t)(uint32_t)h << 32);
    }
    int width = s->width, height = s->height;
    uint64_t pitch = (uint64_t)width;   // sdk+0x396dc..0x396f0: pitch = (width % stride == 0) ? width : width + stride - width % stride
    if (stride > 0 && width % stride) pitch = (uint64_t)width + (uint64_t)stride - (uint64_t)(width % stride);   // 64-bit: a guest stride near INT_MAX must not overflow
    else if (stride <= 0 && width) { static int once; if (!once++) LOG("JpegDecoder::Decode: stride %d <= 0, using pitch = width", stride); }
    int bpp = (s->fmt == 1) ? 3 : 4;
    size_t row_bytes = (size_t)(pitch * (uint64_t)bpp);
    size_t need = row_bytes * (size_t)height;
    size_t rows = height;
    if (dst_size < need) {   // the sdk's stub drops dstSize; honour it here and write only the rows that fit
        static int once;
        if (!once++) LOG("JpegDecoder::Decode: dst size %zu < %zu needed (%dx%d, pitch %llu, %d bpp); writing the rows that fit", dst_size, need, width, height, (unsigned long long)pitch, bpp);
        rows = dst_size / row_bytes;
    }
    uint8_t *out = dst;
    for (size_t r = 0; r < rows; r++) {
        uint8_t *row = out + r * row_bytes;
        jdec_write(row, (size_t)width * (size_t)bpp);
        for (int x = 0; x < width; x++) {
            const uint8_t *px = s->rgba + (r * (size_t)width + (size_t)x) * 4;
            uint8_t *o = row + (size_t)x * (size_t)bpp;
            o[0] = px[0]; o[1] = px[1]; o[2] = px[2];   // stb gives R,G,B(,A)
            if (bpp == 4) o[3] = 0xff;                   // sdk+0x39790: strb w24(0xff)
        }
    }
    LOG("JpegDecoder::Decode: %dx%d -> %p (%zu of %zu bytes, pitch %llu px, format %d)", width, height, dst, rows * row_bytes, need, (unsigned long long)pitch, s->fmt);
    return RES_OK;
}

HLE_FORCE("_ZNK2nn5image11JpegDecoder20GetAnalyzedDimensionEv", "nn::image::JpegDecoder: the packed dimension at +0x28 (width | height<<32) returned in x0", uint64_t, h_img_GetDim,
          const void *self) {
    const jdec_guest *d = self;
    return d->dim;   // sdk+0x39440: ldr x0,[x0,#0x28]; ret
}
