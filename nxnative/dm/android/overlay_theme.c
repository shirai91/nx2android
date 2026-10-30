// Android port: the on-screen gamepad's theme (overlay_layout.h ov_theme), read from the APK's assets/overlay: theme.tsv (devtools/theme_render.py writes it,
// dm/android/themes/README.md) and one atlas per layout, <layout>.png. porter packs only the game's pack (dm/android/themes/themes.tsv, the ergo pack for a
// game with no row), so the app never chooses. No asset (an APK made without one, the adb shell executable): the gamepad is drawn without a theme.
// The atlases are 8-bit PNGs, RGBA or 256 colours with their alphas (what theme_render.py saves), decoded here with the system's zlib.
#include "dm.h"
#include "overlay_layout.h"
#include <android/asset_manager.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

__attribute__((weak)) AAssetManager *dm_front_assets(void) { return NULL; }

static uint8_t *asset(const char *name, size_t *n) {   // the whole asset, NUL-terminated (malloc), NULL when the APK has none
    AAssetManager *am = dm_front_assets(); AAsset *a = am ? AAssetManager_open(am, name, AASSET_MODE_BUFFER) : NULL;
    if (!a) return NULL;
    off_t len = AAsset_getLength(a); uint8_t *b = malloc((size_t)len + 1);
    if (b && AAsset_read(a, b, (size_t)len) != len) { free(b); b = NULL; }
    AAsset_close(a); if (b) { b[len] = 0; *n = (size_t)len; }
    return b;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint8_t *png_rgba(const uint8_t *f, size_t n, int *pw, int *ph) {   // RGBA texels (malloc), NULL for a PNG of another kind
    if (n < 8 || memcmp(f, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    uint32_t w = 0, h = 0; int type = -1; uint8_t pal[256][4]; memset(pal, 255, sizeof pal);
    uint8_t *z = NULL; size_t zn = 0;
    for (size_t i = 8; i + 12 <= n;) {
        uint32_t len = be32(f + i); const uint8_t *d = f + i + 8, *t = f + i + 4;
        if (i + 12 + len > n) break;
        if (!memcmp(t, "IHDR", 4) && len >= 13) {
            w = be32(d); h = be32(d + 4); type = d[9];
            if (d[8] != 8 || (type != 6 && type != 3) || d[12] || w > 8192 || h > 8192) { free(z); return NULL; }   // 8 bits, RGBA or palette, not interlaced
        } else if (!memcmp(t, "PLTE", 4)) { for (uint32_t k = 0; k < len / 3 && k < 256; k++) { pal[k][0] = d[3 * k]; pal[k][1] = d[3 * k + 1]; pal[k][2] = d[3 * k + 2]; } }
        else if (!memcmp(t, "tRNS", 4)) { for (uint32_t k = 0; k < len && k < 256; k++) pal[k][3] = d[k]; }
        else if (!memcmp(t, "IDAT", 4)) { uint8_t *nz = realloc(z, zn + len); if (!nz) { free(z); return NULL; } z = nz; memcpy(z + zn, d, len); zn += len; }
        else if (!memcmp(t, "IEND", 4)) break;
        i += 12 + len;
    }
    if (type < 0 || !w || !h || !z) { free(z); return NULL; }
    size_t bpp = type == 6 ? 4 : 1, stride = w * bpp; uLongf rn = (uLongf)((stride + 1) * h);
    uint8_t *raw = malloc(rn), *out = malloc((size_t)w * h * 4);
    if (!raw || !out || uncompress(raw, &rn, z, (uLong)zn) != Z_OK || rn != (stride + 1) * h) { free(z); free(raw); free(out); return NULL; }
    free(z);
    for (uint32_t y = 0; y < h; y++) {   // the filters (PNG 9.2), in place, each row against the one above it
        uint8_t *r = raw + y * (stride + 1) + 1, *u = y ? r - (stride + 1) : NULL; int ft = r[-1];
        for (size_t x = 0; x < stride; x++) {
            int a = x >= bpp ? r[x - bpp] : 0, b = u ? u[x] : 0, c = u && x >= bpp ? u[x - bpp] : 0;
            int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
            r[x] += ft == 1 ? a : ft == 2 ? b : ft == 3 ? (a + b) / 2 : ft == 4 ? (pa <= pb && pa <= pc ? a : pb <= pc ? b : c) : 0;
        }
        uint8_t *o = out + (size_t)y * w * 4;
        if (type == 6) memcpy(o, r, stride); else for (uint32_t x = 0; x < w; x++) memcpy(o + 4 * x, pal[r[x]], 4);
    }
    free(raw); *pw = (int)w; *ph = (int)h;
    return out;
}

static int ctl_index(const char *name) {   // theme.tsv's name of a control (overlay_default.tsv's) → its index in the layouts
    for (int i = 0; i < OV_N; i++) if (!strcmp(name, OV_LAYOUTS[0][i].name)) return i;
    return -1;
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static ov_theme g_th[2]; static int g_tried[2];
const ov_theme *dm_overlay_theme(int layout) {
    static const char *const LAYOUT[2] = {"tablet", "phone"};
    if (layout < 0 || layout > 1) return NULL;
    pthread_mutex_lock(&g_mu);
    ov_theme *t = &g_th[layout];
    if (!g_tried[layout]) {
        g_tried[layout] = 1;
        size_t n = 0; char *tsv = (char *)asset("overlay/theme.tsv", &n), png[64] = "";
        if (!tsv) { LOG("[overlay] the APK has no gamepad theme (assets/overlay/theme.tsv): the gamepad is drawn without one"); pthread_mutex_unlock(&g_mu); return NULL; }
        int cap = 0, bad = 0; t->alpha = 1;
        for (char *line = strtok(tsv, "\n"); line; line = strtok(NULL, "\n")) {
            char lay[16], ctl[16], dir[16], st[16]; float f[6]; int dw, dh;
            if (line[0] == '#') continue;
            if (sscanf(line, "alpha\t%f", &f[0]) == 1) t->alpha = f[0];
            else if (sscanf(line, "layout\t%15s\t%d\t%d\t%63s\t%f\t%f", lay, &dw, &dh, png, &f[0], &f[1]) == 6) {
                if (strcmp(lay, LAYOUT[layout])) { png[0] = 0; continue; }
                t->dh = (float)dh; t->scale = f[0]; t->travel = f[1];
                char nm[80]; snprintf(nm, sizeof nm, "overlay/%s", png); size_t pn;
                uint8_t *pf = asset(nm, &pn); t->px = pf ? png_rgba(pf, pn, &t->w, &t->h) : NULL; free(pf);
                if (!t->px) { LOG("[RED] [overlay] the gamepad theme's %s is missing or not an 8-bit RGBA / palette PNG: the gamepad is drawn without a theme", nm); bad = 1; break; }
            } else if (sscanf(line, "sprite\t%15s\t%15s\t%15s\t%15s\t%f\t%f\t%f\t%f\t%f\t%f", lay, ctl, dir, st, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]) == 10) {
                if (strcmp(lay, LAYOUT[layout])) continue;
                int c = ctl_index(ctl), s = !strcmp(st, "idle") ? OVT_IDLE : !strcmp(st, "held") ? OVT_HELD : !strcmp(st, "knob") ? OVT_KNOB : !strcmp(st, "decor") ? OVT_DECOR : -1;
                int d = !strcmp(dir, "right") ? 0 : !strcmp(dir, "left") ? 1 : !strcmp(dir, "down") ? 2 : !strcmp(dir, "up") ? 3 : -1;
                if (c < 0 || s < 0) { LOG("[RED] [overlay] gamepad theme: a sprite of an unknown control or state (%s %s): left out", ctl, st); continue; }
                if (t->n == cap) { cap = cap ? 2 * cap : 64; t->sp = realloc(t->sp, (size_t)cap * sizeof *t->sp); }
                t->sp[t->n++] = (ov_tsprite){c, d, s, f[0], f[1], f[2], f[3], f[4], f[5]};
            }
        }
        free(tsv);
        if (bad || !t->px || !t->n || t->dh <= 0 || t->scale <= 0) {
            if (!bad) LOG("[RED] [overlay] the gamepad theme has no %s layout: the gamepad is drawn without a theme", LAYOUT[layout]);
            free(t->px); free(t->sp); memset(t, 0, sizeof *t);
        } else { t->layout = layout; LOG("[overlay] gamepad theme, %s layout: %d sprites, atlas %dx%d", LAYOUT[layout], t->n, t->w, t->h); }
    }
    pthread_mutex_unlock(&g_mu);
    return t->n ? t : NULL;
}
void dm_overlay_theme_done(const ov_theme *t) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < 2; i++) if (t == &g_th[i]) { free(g_th[i].px); g_th[i].px = NULL; }
    pthread_mutex_unlock(&g_mu);
}
