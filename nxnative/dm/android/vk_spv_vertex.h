// Retype integer vertex inputs to the fetch format, then bitcast loads back to the guest register type.
// UINT8 -> SINT32 must zero-extend before reinterpretation; changing the fetch to SINT8 would sign-extend.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t op, a, b; } spv_vtype;
static uint32_t spv_vflip(uint32_t t, spv_vtype *ty, uint32_t *bound, uint32_t *dec, uint32_t *nd) {
    spv_vtype k = ty[t];
    if (k.op == 21 && k.a == 32) k.b ^= 1;
    else if (k.op == 23) { k.a = spv_vflip(k.a, ty, bound, dec, nd); if (!k.a) return 0; }
    else if (k.op == 32 && k.a == 1) { k.b = spv_vflip(k.b, ty, bound, dec, nd); if (!k.b) return 0; }
    else return 0;
    for (uint32_t i = 1; i < *bound; i++) if (ty[i].op == k.op && ty[i].a == k.a && ty[i].b == k.b) return i;
    uint32_t id = (*bound)++; ty[id] = k;
    dec[(*nd)++] = 4u << 16 | k.op; dec[(*nd)++] = id; dec[(*nd)++] = k.a; dec[(*nd)++] = k.b;
    return id;
}
static uint32_t *spv_vertex_signedness(const uint32_t *w, uint32_t nw, uint32_t mask, uint32_t *onw) {
    if (nw < 5 || !mask || !w[3] || w[3] > nw * 4) return NULL;
    uint32_t old = w[3], bound = old, fn = nw, nd = 0, ng = 0, no = 5;
    spv_vtype *ty = calloc(old * 4, sizeof *ty);
    uint32_t *loc = calloc(old, 4), *changed = calloc(old, 4), *dec = malloc(old * 32), *globals = malloc(nw * 4), *out = malloc((nw * 4 + old * 16) * 4);
    for (uint32_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i + n > nw) goto fail;
        if (op == 54) { fn = i; break; }
        if ((op == 21 || op == 23 || op == 32) && n == 4 && w[i+1] < old) ty[w[i+1]] = (spv_vtype){op,w[i+2],w[i+3]};
        if (op == 71 && n == 4 && w[i+2] == 30 && w[i+1] < old && w[i+3] < 32) loc[w[i+1]] = 1u << w[i+3];
        i += n;
    }
    memcpy(out, w, 20);
    for (uint32_t i = 5; i < fn;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (op == 59) {
            memcpy(globals + ng, w + i, n * 4);
            if (w[i+3] == 1 && (loc[w[i+2]] & mask)) {
                uint32_t p = spv_vflip(w[i+1], ty, &bound, dec, &nd);
                if (!p) goto fail;
                globals[ng+1] = p; changed[w[i+2]] = p;
            }
            ng += n;
        } else { memcpy(out + no, w + i, n * 4); no += n; }
        i += n;
    }
    // Access-chain pointer types also need flipping; discover these before emitting the declarations.
    for (uint32_t i = fn; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i+n > nw) goto fail;
        if ((op == 65 || op == 66) && n >= 4 && w[i+3] < old && changed[w[i+3]]) {
            uint32_t p = spv_vflip(w[i+1], ty, &bound, dec, &nd); if (!p) goto fail;
            changed[w[i+2]] = p;
        }
        i += n;
    }
    memcpy(out + no, dec, nd * 4); no += nd;
    memcpy(out + no, globals, ng * 4); no += ng;
    for (uint32_t i = fn; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535, at = no;
        memcpy(out + no, w + i, n * 4); no += n;
        if ((op == 65 || op == 66) && changed[w[i+2]]) out[at+1] = changed[w[i+2]];
        if (op == 61 && n >= 4 && w[i+3] < old && changed[w[i+3]]) {
            uint32_t value = bound++, nt = ty[changed[w[i+3]]].b;
            out[at+1] = nt; out[at+2] = value;
            out[no++] = 4u << 16 | 124; out[no++] = w[i+1]; out[no++] = w[i+2]; out[no++] = value;
        }
        i += n;
    }
    out[3] = bound; *onw = no;
    free(ty); free(loc); free(changed); free(dec); free(globals); return out;
fail:
    free(ty); free(loc); free(changed); free(dec); free(globals); free(out); return NULL;
}
