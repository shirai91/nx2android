// Synthetic geometry index builder: the atomic result addresses another buffer
// store, while raster output is independent. Variants route it through helpers,
// control flow or raster output to check the production independence analysis.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint32_t *spv_gs_split(const uint32_t *, uint32_t, int, uint32_t *, char *, size_t);

static uint32_t w[4096], nw;
static void emit(uint32_t op, const uint32_t *a, size_t n) { w[nw++] = (uint32_t)(n + 1) << 16 | op; for (size_t i = 0; i < n; i++) w[nw++] = a[i]; }
#define E(op, ...) do { const uint32_t a[] = {__VA_ARGS__}; emit(op, a, sizeof a / sizeof *a); } while (0)
#define Z(op) emit(op, NULL, 0)

static void fixture(int kind) {
    uint32_t header[] = {0x07230203, 0x10300, 0, 128, 0}; memcpy(w, header, sizeof header); nw = 5;
    E(17, 1); E(17, 2); E(14, 0, 1); // Shader, Geometry; Logical GLSL450
    if (kind == 12) { nw -= 3; E(17, 24); E(14, 0, 1); }
    if (kind >= 16) { nw -= 3; E(17, 61); E(17, 63); E(14, 0, 1); }
    if (kind == 12) E(15, 3, 21, 0x6e69616d, 0, 19, 49);
    else E(15, kind == 7 ? 0 : 3, 21, 0x6e69616d, 0, 19);
    E(16, 21, 22); E(16, 21, 29); E(16, 21, 26, kind == 19 ? 1 : 3); E(16, 21, 0, 1);
    if (kind == 6) E(16, 21, 11); // Xfb is intentionally outside this path.
    E(71, 6, 6, 4); E(71, 7, 3); E(72, 7, 0, 35, 0);
    E(71, 18, 34, 0); E(71, 18, 33, 0);
    if (kind == 11) { E(71, 49, 2); E(72, 49, 0, 11, 0); E(72, 49, 1, 11, 1); }
    else E(71, 19, 11, 0);
    if (kind == 12) E(71, 49, 11, 1);
    E(19, 1); E(20, 2); E(21, 3, 32, 0); E(22, 4, 32); E(23, 5, 4, 4);
    E(29, 6, 3); E(30, 7, 6); E(32, 8, 2, 7); E(32, 9, 2, 3);
    E(32, 10, 3, 5); E(32, 11, 6, 3); E(33, 12, 1);
    if (kind == 11) { E(30, 49, 5, 4); E(32, 50, 3, 49); }
    if (kind == 12) E(32, 50, 3, 4);
    E(32, 37, 7, 3); E(33, 38, 3, 37);
    if (kind >= 13) E(33, 52, 3, 3);
    E(43, 3, 13, 0); E(43, 3, 14, 1); E(43, 4, 15, 0); E(43, 4, 16, 0x3f800000);
    if (kind >= 16) E(43, 3, 57, 3); // Subgroup scope.
    E(44, 5, 17, 15, 15, 15, 16);
    E(59, 8, 18, 2); E(59, kind == 11 ? 50 : 10, 19, 3); E(59, 11, 20, 6);
    if (kind == 12) E(59, 50, 49, 3);
    E(54, 1, 21, 0, 12); E(248, 22); E(59, 37, 39, 7);
    E(57, 1, 47, 46); // A void helper's return must not emit another point.
    E(62, 20, 14); E(65, 9, 23, 18, 13, 13);
    if (kind == 5) E(83, 3, 24, 13); else E(234, 3, 24, 23, 14, 13, 14);
    uint32_t value = 24;
    if (kind == 3 || kind == 4 || kind == 8) {
        E(62, 39, 24); E(57, 3, 40, 42, 39); value = 40;
    }
    if (kind >= 13) { E(57, 3, 40, 55, 24); value = 40; }
    if (kind >= 16) { E(349, 3, 56, 57, 0, value); value = 56; }
    E(65, 9, 25, 18, 13, value);
    if (kind != 5) E(62, 25, value);
    if (kind == 9) E(63, 25, 23);
    if (kind == 10) E(61, 3, 32, 23);
    uint32_t position = 17;
    if (kind == 1 || kind == 3 || kind == 10 || kind == 13 || kind == 16) {
        E(112, 4, 29, kind == 10 ? 32 : value); E(80, 5, 30, 15, 15, 29, 16); position = 30;
    }
    E(170, 2, 26, (kind == 2 || kind == 4 || kind == 14 || kind == 17) ? value : 14, 14);
    E(247, 28, 0); E(250, 26, 27, 28); E(248, 27);
    uint32_t dest = 19;
    if (kind == 11) { E(65, 10, 51, 19, 13); dest = 51; }
    for (int i = 0; i < (kind == 19 ? 1 : 3); i++) { E(62, dest, position); Z(218); }
    Z(219); E(249, 28); E(248, 28); Z(253); Z(56);
    E(54, 1, 46, 0, 12); E(248, 48); Z(253); Z(56);
    if (kind == 3 || kind == 4 || kind == 8) {
        E(54, 3, 42, 0, 38); E(55, 37, 43); E(248, 44); E(61, 3, 45, 43); E(254, 45); Z(56);
    }
    if (kind >= 13) { E(54, 3, 55, 0, 52); E(55, 3, 53); E(248, 54); E(254, 53); Z(56); }
}

static int count(const uint32_t *p, uint32_t n, unsigned op) {
    int c = 0; for (uint32_t i = 5; i < n; i += p[i] >> 16) { assert(p[i] >> 16); c += (p[i] & 65535) == op; } return c;
}
static unsigned mode_value(const uint32_t *p, uint32_t n, unsigned mode) {
    for (uint32_t i = 5; i < n; i += p[i] >> 16) if ((p[i] & 65535) == 16 && p[i + 2] == mode) return p[i] >> 16 == 4 ? p[i + 3] : 1;
    return 0;
}
static void write_module(const char *prefix, int kind, int mode, const uint32_t *p, uint32_t n) {
    if (!prefix) return; char path[1024]; snprintf(path, sizeof path, "%s-%d-%d.spv", prefix, kind, mode);
    FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(p, 4, n, f) == n); fclose(f);
}
int main(int argc, char **argv) {
    char why[128]; int checks = 0; const char *prefix = argc > 1 ? argv[1] : NULL;
    for (int kind = 0; kind <= 19; kind++) {
        fixture(kind); uint32_t original[4096]; memcpy(original, w, nw * 4);
        int accepted = kind == 0 || kind == 8 || kind == 11 || kind == 12 || kind == 15 || kind == 18;
        for (int mode = 1; mode <= 2; mode++) {
            uint32_t n = 123, *p = spv_gs_split(w, nw, mode, &n, why, sizeof why);
            assert((p != NULL) == accepted); assert(!memcmp(original, w, nw * 4));
            if (!accepted) { assert(n == 0 && why[0]); checks++; continue; }
            assert(!why[0]);
            if (mode == 1) {
                assert(count(p, n, 234) == 1); assert(count(p, n, 218) == 1);
                assert(mode_value(p, n, 26) == 1 && mode_value(p, n, 27));
                assert(count(p, n, 17) == (kind >= 16 ? 5 : 3)); // PointSize capability exactly once.
            } else {
                assert(count(p, n, 234) == 0); assert(count(p, n, 218) == 3);
                assert(mode_value(p, n, 26) == 3 && mode_value(p, n, 29));
                // The original counter result still has exactly one definition.
                int definitions = 0;
                for (uint32_t i = 5; i < n; i += p[i] >> 16) {
                    if ((p[i] & 65535) == 83 && p[i + 2] == 24) definitions++;
                    assert(!((p[i] & 65535) == 62 && p[i + 1] == 25));
                }
                assert(definitions == 1);
            }
            write_module(prefix, kind, mode, p, n); free(p); checks++;
        }
        if (accepted) write_module(prefix, kind, 0, w, nw);
    }
    fixture(0); uint32_t n;
    uint32_t saved = w[5]; w[5] = 0; assert(!spv_gs_split(w, nw, 1, &n, why, sizeof why)); w[5] = saved; checks++;
    assert(!spv_gs_split(w, 4, 1, &n, why, sizeof why)); checks++;
    assert(!spv_gs_split(w, nw, 3, &n, why, sizeof why)); checks++;
    puts("geometry split: 43 cases passed"); assert(checks == 43); return 0;
}
