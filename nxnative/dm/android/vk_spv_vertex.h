// Retype integer vertex inputs to the fetch format, then bitcast loads back to the guest register type.
// UINT8 -> SINT32 must zero-extend before reinterpretation; changing the fetch to SINT8 would sign-extend.
// Scaled inputs: a float input whose USCALED / SSCALED format the device cannot fetch (Adreno 740 has no vertex-buffer support for R8G8B8A8_USCALED,
// the D3D9 UBYTE4 bone indices of Toy Story 3 Complete Edition's skinned characters: without this every character pipeline was skipped) is fetched as
// UINT / SINT of the same layout and converted on load (OpConvertUToF / OpConvertSToF), which gives exactly the values the scaled fetch would.
// The mask: bits 0..15 flip signedness (int inputs), 16..31 float input fetched as uint, 32..47 float input fetched as sint (bit = location).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t op, a, b; } spv_vtype;
// to: 0 flip an int's signedness; 1 / 2 a 32-bit float becomes uint / int
static uint32_t spv_vflip_to(uint32_t t, spv_vtype *ty, uint32_t *bound, uint32_t *dec, uint32_t *nd, int to) {
    spv_vtype k = ty[t];
    if (!to && k.op == 21 && k.a == 32) k.b ^= 1;
    else if (to && k.op == 22 && k.a == 32) k = (spv_vtype){21, 32, to == 2};
    else if (k.op == 23) { k.a = spv_vflip_to(k.a, ty, bound, dec, nd, to); if (!k.a) return 0; }
    else if (k.op == 32 && k.a == 1) { k.b = spv_vflip_to(k.b, ty, bound, dec, nd, to); if (!k.b) return 0; }
    else return 0;
    for (uint32_t i = 1; i < *bound; i++) if (ty[i].op == k.op && ty[i].a == k.a && ty[i].b == k.b) return i;
    uint32_t id = (*bound)++; ty[id] = k;
    // OpTypeInt has 2 operands, OpTypeVector / OpTypePointer 2: 4 words each
    dec[(*nd)++] = 4u << 16 | k.op; dec[(*nd)++] = id; dec[(*nd)++] = k.a; dec[(*nd)++] = k.b;
    return id;
}
static uint32_t *spv_vertex_signedness(const uint32_t *w, uint32_t nw, uint64_t mask, uint32_t *onw) {
    if (nw < 5 || !mask || !w[3] || w[3] > nw * 4) return NULL;
    uint32_t old = w[3], bound = old, fn = nw, nd = 0, ng = 0, no = 5;
    uint32_t mflip = (uint32_t)(mask & 0xffff), mu = (uint32_t)(mask >> 16 & 0xffff), ms = (uint32_t)(mask >> 32 & 0xffff);
    spv_vtype *ty = calloc(old * 4, sizeof *ty);
    uint8_t *cvt = calloc(old, 1);   // per changed id: 0 bitcast, 1 / 2 convert uint / int to float
    uint32_t *loc = calloc(old, 4), *changed = calloc(old, 4), *dec = malloc(old * 32), *globals = malloc(nw * 4), *out = malloc((nw * 4 + old * 16) * 4);
    for (uint32_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i + n > nw) goto fail;
        if (op == 54) { fn = i; break; }
        if ((op == 21 || op == 23 || op == 32) && n == 4 && w[i+1] < old) ty[w[i+1]] = (spv_vtype){op,w[i+2],w[i+3]};
        if (op == 22 && n >= 3 && w[i+1] < old) ty[w[i+1]] = (spv_vtype){op,w[i+2],0};   // OpTypeFloat: width (and an optional encoding)
        if (op == 71 && n == 4 && w[i+2] == 30 && w[i+1] < old && w[i+3] < 32) loc[w[i+1]] = 1u << w[i+3];
        i += n;
    }
    memcpy(out, w, 20);
    for (uint32_t i = 5; i < fn;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (op == 59) {
            memcpy(globals + ng, w + i, n * 4);
            if (w[i+3] == 1 && (loc[w[i+2]] & (mflip | mu | ms))) {
                int to = loc[w[i+2]] & mu ? 1 : loc[w[i+2]] & ms ? 2 : 0;
                uint32_t p = spv_vflip_to(w[i+1], ty, &bound, dec, &nd, to);
                if (!p) goto fail;
                globals[ng+1] = p; changed[w[i+2]] = p; cvt[w[i+2]] = (uint8_t)to;
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
            uint32_t p = spv_vflip_to(w[i+1], ty, &bound, dec, &nd, cvt[w[i+3]]); if (!p) goto fail;
            if (w[i+2] < old) { changed[w[i+2]] = p; cvt[w[i+2]] = cvt[w[i+3]]; }
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
            // back to the type the shader loads: OpBitcast (124) for a flipped int, OpConvertUToF (112) / OpConvertSToF (111) for a scaled float
            uint32_t c = cvt[w[i+3]] == 1 ? 112 : cvt[w[i+3]] == 2 ? 111 : 124;
            out[no++] = 4u << 16 | c; out[no++] = w[i+1]; out[no++] = w[i+2]; out[no++] = value;
        }
        i += n;
    }
    out[3] = bound; *onw = no;
    free(ty); free(cvt); free(loc); free(changed); free(dec); free(globals); return out;
fail:
    free(ty); free(cvt); free(loc); free(changed); free(dec); free(globals); free(out); return NULL;
}
