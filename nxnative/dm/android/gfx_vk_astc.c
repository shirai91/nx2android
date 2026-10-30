// ASTC LDR decoding on the CPU (2D blocks of every size, to RGBA8), for a device that cannot sample ASTC itself: none at all, or one whose ASTC reads back wrong
// (vk_astc_check in gfx_vk_core.c). The Android emulator on a Mac (37.2.12, gfxstream's "ASTC LDR emulation mode: Gpu" over MoltenVK) reports ASTC and samples every
// ASTC texture as (0, 0, 0, 0): Link's Awakening's hearts (ASTC 4x4 sRGB) drew nothing, and its menus' ASTC icons drew noise.
// Follows the Khronos Data Format Specification's ASTC chapter: block mode, integer sequence encoding, colour endpoint modes 0-13 (the HDR ones and reserved encodings
// give the error colour, magenta), weight infill, partitions, dual planes, void-extent blocks. sRGB formats decode to their sRGB bytes (the image is an sRGB format).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { Q2, Q3, Q4, Q5, Q6, Q8, Q10, Q12, Q16, Q20, Q24, Q32, Q40, Q48, Q64, Q80, Q96, Q128, Q160, Q192, Q256, NQ };   // quantization levels
static const uint8_t ISE_T[NQ] = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};   // a trit per value
static const uint8_t ISE_Q[NQ] = {0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0};   // a quint per value
static const uint8_t ISE_B[NQ] = {1, 0, 2, 0, 1, 3, 1, 2, 4, 2, 3, 5, 3, 4, 6, 4, 5, 7, 5, 6, 8};   // plain bits per value
static int ise_bits(int n, int q) { return n * ISE_B[q] + (ISE_T[q] ? (8 * n + 4) / 5 : 0) + (ISE_Q[q] ? (7 * n + 2) / 3 : 0); }

typedef unsigned __int128 u128;
typedef struct { u128 v; int pos, end; } rd_t;   // bits from pos upwards; past end they read 0 (a sequence's last group is cut short)
static unsigned rd(rd_t *r, int n) {
    unsigned x = 0;
    if (n && r->pos < r->end) { int k = r->end - r->pos < n ? r->end - r->pos : n; x = (unsigned)(r->v >> r->pos) & ((1u << k) - 1); }
    r->pos += n; return x;
}
static unsigned bits(u128 v, int pos, int n) { return (unsigned)(v >> pos) & ((1u << n) - 1); }

static void trits_of(unsigned T, unsigned t[5]) {
    unsigned C;
    if (((T >> 2) & 7) == 7) { C = ((T >> 5) & 7) << 2 | (T & 3); t[4] = 2; t[3] = 2; }
    else { C = T & 31; if (((T >> 5) & 3) == 3) { t[4] = 2; t[3] = T >> 7 & 1; } else { t[4] = T >> 7 & 1; t[3] = T >> 5 & 3; } }
    if ((C & 3) == 3) { t[2] = 2; t[1] = C >> 4 & 1; t[0] = (C >> 3 & 1) << 1 | (C >> 2 & 1 & ~(C >> 3) & 1); }
    else if ((C >> 2 & 3) == 3) { t[2] = 2; t[1] = 2; t[0] = C & 3; }
    else { t[2] = C >> 4 & 1; t[1] = C >> 2 & 3; t[0] = (C >> 1 & 1) << 1 | (C & 1 & ~(C >> 1) & 1); }
}
static void quints_of(unsigned Q, unsigned q[3]) {
    unsigned C;
    if ((Q >> 1 & 3) == 3 && (Q >> 5 & 3) == 0) { unsigned a = Q & 1; q[2] = a << 2 | (Q >> 4 & 1 & ~a & 1) << 1 | (Q >> 3 & 1 & ~a & 1); q[1] = 4; q[0] = 4; return; }
    if ((Q >> 1 & 3) == 3) { q[2] = 4; C = (Q >> 3 & 3) << 3 | (~(Q >> 5) & 3) << 1 | (Q & 1); }
    else { q[2] = Q >> 5 & 3; C = Q & 31; }
    if ((C & 7) == 5) { q[1] = 4; q[0] = C >> 3 & 3; } else { q[1] = C >> 3 & 3; q[0] = C & 7; }
}
// n values of level q from bit pos of v; each out[i] = (trit or quint) << bits | bits
static void ise_decode(int q, int n, u128 v, int pos, uint8_t *out) {
    rd_t r = {v, pos, pos + ise_bits(n, q)}; int nb = ISE_B[q];
    for (int i = 0; i < n;) {
        if (ISE_T[q]) {
            unsigned m[5], T, t[5];
            m[0] = rd(&r, nb); T = rd(&r, 2); m[1] = rd(&r, nb); T |= rd(&r, 2) << 2; m[2] = rd(&r, nb); T |= rd(&r, 1) << 4; m[3] = rd(&r, nb); T |= rd(&r, 2) << 5; m[4] = rd(&r, nb); T |= rd(&r, 1) << 7;
            trits_of(T, t); for (int k = 0; k < 5 && i < n; k++, i++) out[i] = (uint8_t)(t[k] << nb | m[k]);
        } else if (ISE_Q[q]) {
            unsigned m[3], Q, u[3];
            m[0] = rd(&r, nb); Q = rd(&r, 3); m[1] = rd(&r, nb); Q |= rd(&r, 2) << 3; m[2] = rd(&r, nb); Q |= rd(&r, 2) << 5;
            quints_of(Q, u); for (int k = 0; k < 3 && i < n; k++, i++) out[i] = (uint8_t)(u[k] << nb | m[k]);
        } else out[i++] = (uint8_t)rd(&r, nb);
    }
}
static unsigned replicate(unsigned v, int nb, int to) { unsigned x = 0; int have = 0; while (have < to) { x = x << nb | v; have += nb; } return x >> (have - to); }
static uint8_t unq_color(int q, unsigned v) {   // to 0..255
    int nb = ISE_B[q];
    if (!ISE_T[q] && !ISE_Q[q]) return (uint8_t)replicate(v, nb, 8);
    unsigned m = v & ((1u << nb) - 1), D = v >> nb, A = m & 1 ? 0x1FF : 0, B = 0, C = 0, b = m >> 1;
    if (ISE_T[q]) switch (nb) {
        case 1: C = 204; break;
        case 2: B = (b & 1) << 8 | (b & 1) << 4 | (b & 1) << 2 | (b & 1) << 1; C = 93; break;
        case 3: B = (b & 3) << 7 | (b & 3) << 2 | (b & 3); C = 44; break;
        case 4: B = (b & 7) << 6 | (b & 7); C = 22; break;
        case 5: B = (b & 15) << 5 | (b & 15) >> 2; C = 11; break;
        case 6: B = (b & 31) << 4 | (b & 31) >> 4; C = 5; break;
    } else switch (nb) {
        case 1: C = 113; break;
        case 2: B = (b & 1) << 8 | (b & 1) << 3 | (b & 1) << 2; C = 54; break;
        case 3: B = (b & 3) << 7 | (b & 3) << 1 | (b & 3) >> 1; C = 26; break;
        case 4: B = (b & 7) << 6 | (b & 7) >> 1; C = 13; break;
        case 5: B = (b & 15) << 5 | (b & 15) >> 3; C = 6; break;
    }
    unsigned T = (D * C + B) ^ A; return (uint8_t)((A & 0x80) | T >> 2);
}
static uint8_t unq_weight(int q, unsigned v) {   // to 0..64
    int nb = ISE_B[q]; unsigned r;
    if (!ISE_T[q] && !ISE_Q[q]) r = replicate(v, nb, 6);
    else if (!nb) { static const uint8_t t3[3] = {0, 32, 63}, q5[5] = {0, 16, 32, 47, 63}; r = ISE_T[q] ? t3[v] : q5[v]; }
    else {
        unsigned m = v & ((1u << nb) - 1), D = v >> nb, A = m & 1 ? 0x7F : 0, B = 0, C = 0, b = m >> 1;
        if (ISE_T[q]) switch (nb) { case 1: C = 50; break; case 2: B = (b & 1) << 6 | (b & 1) << 2 | (b & 1); C = 23; break; case 3: B = (b & 3) << 5 | (b & 3); C = 11; break; }
        else switch (nb) { case 1: C = 28; break; case 2: B = (b & 1) << 6 | (b & 1) << 1; C = 13; break; }
        r = (A & 0x20) | ((D * C + B) ^ A) >> 2;
    }
    return (uint8_t)(r > 32 ? r + 1 : r);
}

// weight grid size, dual plane and weight quantization of a 2D block mode; 0 = reserved
static int block_mode(unsigned m, int *gw, int *gh, int *dual, int *wq) {
    unsigned r = m >> 4 & 1, h = m >> 9 & 1, d = m >> 10 & 1, a = m >> 5 & 3;
    if (m & 3) {
        r |= (m & 3) << 1; unsigned b = m >> 7 & 3;
        switch (m >> 2 & 3) {
        case 0: *gw = b + 4; *gh = a + 2; break;
        case 1: *gw = b + 8; *gh = a + 2; break;
        case 2: *gw = a + 2; *gh = b + 8; break;
        default: b &= 1; if (m & 0x100) { *gw = b + 2; *gh = a + 2; } else { *gw = a + 2; *gh = b + 6; } break;
        }
    } else {
        r |= (m >> 2 & 3) << 1; if (!(m >> 2 & 3)) return 0;
        unsigned b = m >> 9 & 3;
        switch (m >> 7 & 3) {
        case 0: *gw = 12; *gh = a + 2; break;
        case 1: *gw = a + 2; *gh = 12; break;
        case 2: *gw = a + 6; *gh = b + 6; d = 0; h = 0; break;
        default: if (a == 0) { *gw = 6; *gh = 10; } else if (a == 1) { *gw = 10; *gh = 6; } else return 0; break;
        }
    }
    *dual = (int)d; *wq = (int)(r - 2 + 6 * h); return 1;
}
static uint32_t hash52(uint32_t x) { x ^= x >> 15; x *= 0xEEDE0891u; x ^= x >> 5; x += x << 16; x ^= x >> 7; x ^= x >> 3; x ^= x << 6; x ^= x >> 17; return x; }
static int partition_of(int seed, int x, int y, int np, int small) {
    if (small) { x <<= 1; y <<= 1; }
    seed += (np - 1) * 1024; uint32_t rn = hash52((uint32_t)seed);
    unsigned s[8]; for (int i = 0; i < 8; i++) { s[i] = rn >> (4 * i) & 15; s[i] *= s[i]; }
    int sh1, sh2; if (seed & 1) { sh1 = seed & 2 ? 4 : 5; sh2 = np == 3 ? 6 : 5; } else { sh1 = np == 3 ? 6 : 5; sh2 = seed & 2 ? 4 : 5; }
    for (int i = 0; i < 8; i++) s[i] >>= i & 1 ? sh2 : sh1;
    int a = (int)((s[0] * x + s[1] * y + (rn >> 14)) & 63), b = (int)((s[2] * x + s[3] * y + (rn >> 10)) & 63);
    int c = np < 3 ? 0 : (int)((s[4] * x + s[5] * y + (rn >> 6)) & 63), d = np < 4 ? 0 : (int)((s[6] * x + s[7] * y + (rn >> 2)) & 63);
    return a >= b && a >= c && a >= d ? 0 : b >= c && b >= d ? 1 : c >= d ? 2 : 3;
}
static int clamp8(int x) { return x < 0 ? 0 : x > 255 ? 255 : x; }
static void bts(int *a, int *b) { *b >>= 1; *b |= *a & 0x80; *a >>= 1; *a &= 0x3F; if (*a & 0x20) *a -= 0x40; }   // bit_transfer_signed(a, b)
static void set4(int *e, int r, int g, int b, int a) { e[0] = r; e[1] = g; e[2] = b; e[3] = a; }
static void blue(int *e) { e[0] = (e[0] + e[2]) >> 1; e[1] = (e[1] + e[2]) >> 1; }
static int endpoints(int cem, const uint8_t *u, int *e0, int *e1) {   // 0 = an HDR mode (the error colour in LDR)
    int v[8]; for (int i = 0; i < 8; i++) v[i] = u[i];
    switch (cem) {
    case 0: set4(e0, v[0], v[0], v[0], 255); set4(e1, v[1], v[1], v[1], 255); break;
    case 1: { int l0 = v[0] >> 2 | (v[1] & 0xC0), l1 = l0 + (v[1] & 0x3F); if (l1 > 255) l1 = 255; set4(e0, l0, l0, l0, 255); set4(e1, l1, l1, l1, 255); break; }
    case 4: set4(e0, v[0], v[0], v[0], v[2]); set4(e1, v[1], v[1], v[1], v[3]); break;
    case 5: bts(&v[1], &v[0]); bts(&v[3], &v[2]); set4(e0, v[0], v[0], v[0], v[2]); set4(e1, v[0] + v[1], v[0] + v[1], v[0] + v[1], v[2] + v[3]); break;
    case 6: set4(e0, v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8, 255); set4(e1, v[0], v[1], v[2], 255); break;
    case 8: case 12: {
        int a0 = cem == 12 ? v[6] : 255, a1 = cem == 12 ? v[7] : 255;
        if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) { set4(e0, v[0], v[2], v[4], a0); set4(e1, v[1], v[3], v[5], a1); }
        else { set4(e0, v[1], v[3], v[5], a1); blue(e0); set4(e1, v[0], v[2], v[4], a0); blue(e1); }
        break; }
    case 9: case 13: {
        bts(&v[1], &v[0]); bts(&v[3], &v[2]); bts(&v[5], &v[4]); if (cem == 13) bts(&v[7], &v[6]); else { v[6] = 255; v[7] = 0; }
        if (v[1] + v[3] + v[5] >= 0) { set4(e0, v[0], v[2], v[4], v[6]); set4(e1, v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7]); }
        else { set4(e0, v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7]); blue(e0); set4(e1, v[0], v[2], v[4], v[6]); blue(e1); }
        break; }
    case 10: set4(e0, v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8, v[4]); set4(e1, v[0], v[1], v[2], v[5]); break;
    default: return 0;
    }
    for (int i = 0; i < 4; i++) { e0[i] = clamp8(e0[i]); e1[i] = clamp8(e1[i]); }
    return 1;
}
static uint8_t rev8(uint8_t b) { b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4); b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2); return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1); }

// one block of bw x bh texels into out (row-major, 4 bytes a texel)
static void decode_block(const uint8_t *blk, int bw, int bh, int srgb, uint8_t (*out)[4]) {
    int n = bw * bh; u128 v; memcpy(&v, blk, 16);
    unsigned mode = bits(v, 0, 11);
    if ((mode & 0x1FF) == 0x1FC) {   // void extent: one colour, 16 bits a component
        if (mode & 0x200) goto error;   // HDR
        for (int i = 0; i < n; i++) for (int c = 0; c < 4; c++) out[i][c] = blk[9 + 2 * c];
        return;
    }
    int gw, gh, dual, wq; if (!block_mode(mode, &gw, &gh, &dual, &wq) || gw > bw || gh > bh) goto error;
    int np = (int)bits(v, 11, 2) + 1, nw = gw * gh * (dual + 1), wbits = ise_bits(nw, wq);
    if ((np == 4 && dual) || nw > 64 || wbits < 24 || wbits > 96) goto error;
    int cem[4], below = 128 - wbits, seed = 0;
    if (np == 1) cem[0] = (int)bits(v, 13, 4);
    else {
        seed = (int)bits(v, 13, 10); unsigned enc = bits(v, 23, 6); int base = (int)(enc & 3);
        if (!base) for (int i = 0; i < np; i++) cem[i] = (int)(enc >> 2 & 15);
        else {
            int extra = 3 * np - 4; below -= extra; enc |= bits(v, below, extra) << 6; base--;
            for (int i = 0; i < np; i++) cem[i] = (int)(((enc >> (2 + i) & 1) + (unsigned)base) << 2 | (enc >> (2 + np + 2 * i) & 3));
        }
    }
    int ccs = 0; if (dual) { below -= 2; ccs = (int)bits(v, below, 2); }
    int nv = 0; for (int i = 0; i < np; i++) nv += ((cem[i] >> 2) + 1) * 2;
    int cstart = np == 1 ? 17 : 29, cq = -1;
    if (nv > 18 || below < cstart) goto error;
    for (int q = Q256; q >= Q6; q--) if (ise_bits(nv, q) <= below - cstart) { cq = q; break; }
    if (cq < 0) goto error;
    uint8_t cv[18]; ise_decode(cq, nv, v, cstart, cv); for (int i = 0; i < nv; i++) cv[i] = unq_color(cq, cv[i]);
    int e0[4][4], e1[4][4];
    for (int p = 0, o = 0; p < np; o += ((cem[p] >> 2) + 1) * 2, p++) if (!endpoints(cem[p], cv + o, e0[p], e1[p])) goto error;
    uint8_t rb[16]; for (int k = 0; k < 16; k++) rb[k] = rev8(blk[15 - k]);
    u128 rv; memcpy(&rv, rb, 16);
    uint8_t wv[2 * (144 + 13)]; memset(wv, 0, sizeof wv);   // padded: the infill reads one past a row's and the grid's end with a zero factor
    ise_decode(wq, nw, rv, 0, wv); for (int i = 0; i < nw; i++) wv[i] = unq_weight(wq, wv[i]);
    int ds = (1024 + bw / 2) / (bw - 1), dt = (1024 + bh / 2) / (bh - 1), np1 = dual + 1;
    for (int t = 0; t < bh; t++) for (int s = 0; s < bw; s++) {
        int gs = (ds * s * (gw - 1) + 32) >> 6, gt = (dt * t * (gh - 1) + 32) >> 6, js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
        int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11, i0 = js + jt * gw, w[2];
        for (int pl = 0; pl < np1; pl++)
            w[pl] = (wv[i0 * np1 + pl] * w00 + wv[(i0 + 1) * np1 + pl] * w01 + wv[(i0 + gw) * np1 + pl] * w10 + wv[(i0 + gw + 1) * np1 + pl] * w11 + 8) >> 4;
        int p = np > 1 ? partition_of(seed, s, t, np, n < 31) : 0;
        for (int c = 0; c < 4; c++) {
            int x = dual && c == ccs ? w[1] : w[0];
            int c0 = srgb ? e0[p][c] << 8 | 0x80 : e0[p][c] * 257, c1 = srgb ? e1[p][c] << 8 | 0x80 : e1[p][c] * 257;
            out[t * bw + s][c] = (uint8_t)(((c0 * (64 - x) + c1 * x + 32) >> 6) >> 8);
        }
    }
    return;
error:
    for (int i = 0; i < n; i++) { out[i][0] = 255; out[i][1] = 0; out[i][2] = 255; out[i][3] = 255; }
}

// blocks: (w / bw rounded up) x (h / bh rounded up) blocks of 16 bytes per slice, row after row, `depth` slices; rgba: w x h texels of 4 bytes, rows pitch bytes apart
void vk_astc_decode(const uint8_t *blocks, unsigned bw, unsigned bh, unsigned w, unsigned h, unsigned depth, int srgb, uint8_t *rgba, size_t pitch) {
    if (bw < 4 || bh < 4 || bw > 12 || bh > 12) return;
    unsigned bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh; uint8_t px[144][4];
    for (unsigned z = 0; z < depth; z++) for (unsigned y = 0; y < by; y++) for (unsigned x = 0; x < bx; x++) {
        decode_block(blocks + (((size_t)z * by + y) * bx + x) * 16, (int)bw, (int)bh, srgb, px);
        for (unsigned t = 0; t < bh && y * bh + t < h; t++) {
            unsigned cols = w - x * bw < bw ? w - x * bw : bw;
            memcpy(rgba + ((size_t)z * h + y * bh + t) * pitch + (size_t)x * bw * 4, px[t * bw], (size_t)cols * 4);
        }
    }
}
