// The overlay's text: UTF-8 strings drawn with a bitmap font pack (dm/tools/make_pixel_font.py, from dm/third_party/fusion-pixel-font), which replaced an ASCII-only 5x7 font,
// so the title and initial text of a game's software keyboard and the names of save folders show in Chinese, Japanese and Korean.
// The pack, little-endian: "NXPF", u32 glyph count, the index sorted by code point (pf_index), then the bitmaps: h rows of (w + 7) / 8 bytes, the top bit the leftmost.
// No Vulkan or Android here: tests/test_pixel_font.py runs it on the computer.
#ifndef PIXEL_FONT_H
#define PIXEL_FONT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define PF_CAP 9   // a capital letter's height in rows: the callers' sizes and the vertical centring go by it
typedef struct { uint32_t cp_adv, off_w; int8_t h, x, y, pad; } pf_index;   // code point << 8 | advance; bitmap offset << 8 | width; height; left bearing; bottom's height over the baseline

static inline uint32_t pf_next(const char **ps) {   // the code point at *ps, which moves past it; a broken sequence is U+FFFD and one byte. Never call it at the terminating 0
    const uint8_t *s = (const uint8_t *)*ps; uint32_t c = s[0]; int n = c < 0x80 ? 0 : c >= 0xC2 && c < 0xE0 ? 1 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xF0 && c < 0xF5 ? 3 : -1;
    if (n < 0) { *ps += 1; return 0xFFFD; }
    if (n) c &= 0x3F >> n;
    for (int i = 1; i <= n; i++) { if ((s[i] & 0xC0) != 0x80) { *ps += 1; return 0xFFFD; } c = c << 6 | (s[i] & 0x3F); }
    *ps += n + 1; return c;
}
static inline size_t pf_chop(char *s, size_t n) {   // the last character of a string of n bytes taken away; the new length
    while (n && ((uint8_t)s[n - 1] & 0xC0) == 0x80) n--;
    if (n) n--;
    s[n] = 0; return n;
}
static inline void pf_trim(char *s) {   // a sequence that a byte limit cut short taken away
    size_t n = strlen(s), k = n; while (k && ((uint8_t)s[k - 1] & 0xC0) == 0x80) k--;
    if (!k) return;
    uint8_t c = (uint8_t)s[k - 1]; size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (n - (k - 1) < need) s[k - 1] = 0;
}
static inline int pf_units(const char *s) {   // the string's length in UTF-16 units, as a software keyboard's limit counts it (hle_swkbd.c)
    int n = 0; while (*s) n += pf_next(&s) >= 0x10000 ? 2 : 1;
    return n;
}
static inline const pf_index *pf_find(const uint8_t *pack, uint32_t cp) {   // a glyph; one the pack lacks draws as U+FFFD; NULL without a pack
    if (!pack) return NULL;
    uint32_t n; memcpy(&n, pack + 4, 4); const pf_index *ix = (const pf_index *)(const void *)(pack + 8);
    for (int pass = 0; pass < 2; pass++, cp = 0xFFFD) for (uint32_t lo = 0, hi = n; lo < hi;) {
        uint32_t m = lo + (hi - lo) / 2, c = ix[m].cp_adv >> 8;
        if (c == cp) return &ix[m];
        if (c < cp) lo = m + 1; else hi = m;
    }
    return NULL;
}
static inline int pf_width(const uint8_t *pack, const char *s) {   // in the font's pixels
    int w = 0; while (*s) { const pf_index *g = pf_find(pack, pf_next(&s)); if (g) w += g->cp_adv & 255; }
    return w;
}
static inline void pf_draw(const uint8_t *pack, const char *s, int x, int base, int scale, void (*plot)(void *, int, int), void *u) {   // x: the left edge; base: the baseline's row
    uint32_t n = 0; if (pack) memcpy(&n, pack + 4, 4);
    const uint8_t *bits = pack ? pack + 8 + (size_t)n * sizeof(pf_index) : NULL;
    while (*s) {
        const pf_index *g = pf_find(pack, pf_next(&s)); if (!g) continue;
        int w = g->off_w & 255, stride = (w + 7) / 8; const uint8_t *b = bits + (g->off_w >> 8);
        for (int r = 0; r < g->h; r++) for (int c = 0; c < w; c++) if (b[r * stride + c / 8] >> (7 - c % 8) & 1)
            for (int dy = 0; dy < scale; dy++) for (int dx = 0; dx < scale; dx++) plot(u, x + (g->x + c) * scale + dx, base + (r - g->h - g->y) * scale + dy);
        x += (int)(g->cp_adv & 255) * scale;
    }
}
#endif
