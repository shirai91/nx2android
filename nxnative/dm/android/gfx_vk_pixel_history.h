// Pure helpers for the opt-in one-pixel render history (gfx_vk_exec.c).
#pragma once
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int vk_pixel_history_parse(const char *s, uint32_t out[4]) {
    uint32_t v[4]; const char sep[4] = {'x', ':', ',', 0};
    for (int i = 0; i < 4; i++) { if (!s || *s < '0' || *s > '9') return 0;
        char *e; errno = 0; unsigned long n = strtoul(s, &e, 10);
        if (errno || n > 16384 || *e != sep[i]) return 0; v[i] = (uint32_t)n; s = e + (i < 3); }
    if (!v[0] || !v[1] || v[2] >= v[0] || v[3] >= v[1]) return 0;
    memcpy(out, v, sizeof v); return 1;
}
static float vk_pixel_history_ufloat(uint32_t v, unsigned mantissa) {
    unsigned e = v >> mantissa & 31, m = v & ((1u << mantissa) - 1);
    if (e == 31) return m ? NAN : INFINITY;
    return e ? ldexpf(1.f + (float)m / (1u << mantissa), (int)e - 15) : ldexpf((float)m, -14 - (int)mantissa);
}
