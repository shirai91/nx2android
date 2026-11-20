// Android port, Vulkan backend: uniform buffer loads that pick a vector component at a run-time index, rewritten as a load of the whole vector and
// OpVectorExtractDynamic (see gfx_vk.h; called by gfx_vk_shader.c vk_shader when DM_VK_UBO_COMPONENT is on).
// Plain C over the SPIR-V words, without Vulkan, so tests/test_vk_ubo_component.py builds and checks it on the PC.
//
// Maxwell reads a constant buffer at a register offset (LDC) per thread; the translator writes it as an access chain into the uniform block's vec4 array:
// row offset >> 4, then component (offset >> 2) & 3, both computed in the shader. Vulkan allows both to differ between invocations. The Turnip (Mesa) driver
// shipped as vulkan.ad07xx.so (AYN Odin 2, Adreno 740) does not give each invocation its own component when that index is not a constant: Pokémon Violet's
// deferred lighting shader 8bc9341cecc2ce7d reads material parameters at (G-buffer material id & 7) * 16 + 496, and in a wave that holds both hair and skin
// pixels the hair pixels came out white. Those waves are 8 pixel rows high, so every hair edge over skin or background was a staircase of white 8-row steps.
// NonUniform decorations and Volatile loads changed nothing; a loop over the wave's distinct indices fixed it at 9 points of GPU busy (66 % to 75 % in the
// intro's office scene); loading the row's vector at the same per-pixel row index and taking the component from it fixes it at no measurable cost.
// A load is rewritten when its access chain's base is a Uniform variable of a Block struct, the chain's last index is not a constant and selects a component of
// a vector, the chain is used by the OpLoad right after it alone, and the indices before the last walk arrays and (with constant indices) struct members.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t *spv_ubo_component(const uint32_t *w, uint32_t nw, uint32_t *onw, uint32_t *nsites);

enum { BLOCK = 1, UBO = 2, CONST = 4, VEC = 8, ARR = 16, STRUCT = 32 };

static int is_const(uint32_t op) { return op == 41 || op == 42 || op == 43 || op == 44 || op == 46 || (op >= 48 && op <= 51); }   // OpConstant*, OpSpecConstant*

// the vector type whose component the chain at w[i] (n words) picks with its last index, 0 when the chain is not that shape
// (type: a variable's pointer type; aux: an array's element type, a struct's instruction, a constant's value)
static uint32_t chain_vec(const uint32_t *w, uint32_t i, uint32_t n, uint32_t bound, const uint8_t *flag, const uint32_t *type, const uint32_t *ptr_to, const uint32_t *aux) {
    if (n < 5 || w[i + 3] >= bound || !(flag[w[i + 3]] & UBO) || (w[i + n - 1] < bound && flag[w[i + n - 1]] & CONST)) return 0;
    uint32_t t = type[w[i + 3]] < bound ? ptr_to[type[w[i + 3]]] : 0;
    for (uint32_t k = 4; k < n - 1; k++) {
        if (!t || t >= bound) return 0;
        if (flag[t] & ARR) t = aux[t];
        else if (flag[t] & STRUCT && w[i + k] < bound && flag[w[i + k]] & CONST) { uint32_t s = aux[t], m = aux[w[i + k]]; if (m + 2 >= (w[s] >> 16)) return 0; t = w[s + 2 + m]; }
        else return 0;
    }
    return t && t < bound && flag[t] & VEC ? t : 0;
}

typedef struct { uint32_t *w; uint32_t n, cap; } out_t;
static int put(out_t *o, const uint32_t *x, uint32_t n) {
    if (o->n + n > o->cap) { uint32_t c = o->cap ? o->cap : 1024; while (c < o->n + n) c *= 2; uint32_t *p = realloc(o->w, (size_t)c * 4); if (!p) return 0; o->w = p; o->cap = c; }
    memcpy(&o->w[o->n], x, (size_t)n * 4); o->n += n; return 1;
}

uint32_t *spv_ubo_component(const uint32_t *w, uint32_t nw, uint32_t *onw, uint32_t *nsites) {
    *nsites = 0;
    if (nw < 5 || w[0] != 0x07230203) return NULL;
    uint32_t bound = w[3];
    uint8_t *flag = calloc(bound, 1); uint32_t *type = calloc(bound, 4), *ptr_sc = calloc(bound, 4), *ptr_to = calloc(bound, 4), *uses = calloc(bound, 4), *aux = calloc(bound, 4), *pu = calloc(bound, 4);
    out_t o = {0}; uint32_t *res = NULL, first_fn = 0, sites = 0;
    if (!flag || !type || !ptr_sc || !ptr_to || !uses || !aux || !pu) goto done;
    for (uint32_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff; if (!n || i + n > nw) goto done;
        if (op == 71 && n >= 3 && w[i + 2] == 2 && w[i + 1] < bound) flag[w[i + 1]] |= BLOCK;   // OpDecorate Block
        else if (op == 32 && n == 4 && w[i + 1] < bound) {   // OpTypePointer; pu: the first Uniform pointer to each type
            ptr_sc[w[i + 1]] = w[i + 2]; ptr_to[w[i + 1]] = w[i + 3]; if (w[i + 2] == 2 && w[i + 3] < bound && !pu[w[i + 3]]) pu[w[i + 3]] = w[i + 1]; }
        else if (op == 23 && n == 4 && w[i + 1] < bound) flag[w[i + 1]] |= VEC;
        else if ((op == 28 || op == 29) && n >= 3 && w[i + 1] < bound) { flag[w[i + 1]] |= ARR; aux[w[i + 1]] = w[i + 2]; }
        else if (op == 30 && n >= 2 && w[i + 1] < bound) { flag[w[i + 1]] |= STRUCT; aux[w[i + 1]] = i; }
        else if (op == 59 && n >= 4 && w[i + 2] < bound && w[i + 1] < bound) {   // OpVariable: a Uniform one of a Block struct is a uniform buffer
            type[w[i + 2]] = w[i + 1];
            if (w[i + 3] == 2 && ptr_sc[w[i + 1]] == 2 && ptr_to[w[i + 1]] < bound && flag[ptr_to[w[i + 1]]] & BLOCK) flag[w[i + 2]] |= UBO; }
        else if (op == 54 && !first_fn) first_fn = i;
        if (is_const(op) && n >= 3 && w[i + 2] < bound) { flag[w[i + 2]] |= CONST; if (op == 43 && n == 4) aux[w[i + 2]] = w[i + 3]; }
        if (op == 61 && n >= 4 && w[i + 3] < bound) uses[w[i + 3]]++;   // OpLoad's pointer
        else { for (uint32_t k = op == 65 || op == 66 ? 3 : 1; k < n; k++) if (w[i + k] < bound) uses[w[i + k]] += 2; }   // a chain's base and indices, any other
                                                                                                                       // mention (literals too: only ever fewer rewrites)
        i += n;
    }
    if (!first_fn) goto done;
    // the sites: OpAccessChain / OpInBoundsAccessChain of that shape, loaded once right after; the vector types that have no Uniform pointer yet
#define SITE(i, n) (((w[i] & 0xffff) == 65 || (w[i] & 0xffff) == 66) && (n) <= 24 && (i) + (n) < nw && (w[(i) + (n)] & 0xffff) == 61 && (w[(i) + (n)] >> 16) >= 4 && \
                    (w[(i) + (n)] >> 16) <= 7 && w[(i) + (n) + 3] == w[(i) + 2] && w[(i) + 2] < bound && uses[w[(i) + 2]] == 1)
    for (uint32_t i = first_fn; i < nw;) {
        uint32_t n = w[i] >> 16, v;
        if (SITE(i, n) && (v = chain_vec(w, i, n, bound, flag, type, ptr_to, aux))) { sites++; if (!pu[v]) pu[v] = UINT32_MAX; }
        i += n;
    }
    if (!sites) goto done;
    uint32_t nb = bound;
    if (!put(&o, w, first_fn)) goto done;   // header, types and globals; then the Uniform pointers the vectors need, before the first function
    for (uint32_t t = 0; t < bound; t++) if (pu[t] == UINT32_MAX) { pu[t] = nb++; uint32_t p[4] = {4u << 16 | 32, pu[t], 2, t}; if (!put(&o, p, 4)) goto done; }
    for (uint32_t i = first_fn; i < nw;) {
        uint32_t n = w[i] >> 16, v;
        if (SITE(i, n) && (v = chain_vec(w, i, n, bound, flag, type, ptr_to, aux))) {
            // %pv = chain to the vector (the indices but the last); %vv = OpLoad vector %pv (the load's memory operands); %r = OpVectorExtractDynamic %scalar %vv %last
            uint32_t ln = w[i + n] >> 16, pv = nb++, vv = nb++, ch[24], ld[8];
            memcpy(ch, &w[i], (size_t)(n - 1) * 4); ch[0] = (n - 1) << 16 | (w[i] & 0xffff); ch[1] = pu[v]; ch[2] = pv;
            memcpy(ld, &w[i + n], (size_t)ln * 4); ld[1] = v; ld[2] = vv; ld[3] = pv;
            uint32_t x[5] = {5u << 16 | 77, w[i + n + 1], w[i + n + 2], vv, w[i + n - 1]};
            if (!put(&o, ch, n - 1) || !put(&o, ld, ln) || !put(&o, x, 5)) goto done;
            (*nsites)++; i += n + ln; continue;
        }
        if (!put(&o, &w[i], n)) goto done;
        i += n;
    }
#undef SITE
    o.w[3] = nb; *onw = o.n; res = o.w; o.w = NULL;
done:
    free(o.w); free(flag); free(type); free(ptr_sc); free(ptr_to); free(uses); free(aux); free(pu);
    if (!res) *nsites = 0;
    return res;
}
