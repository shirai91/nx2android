// Geometry memory effects separated from rasterization, without Vulkan dependencies.
// Adreno executes a GS once per possible output vertex. An index builder that emits
// three vertices consequently appended each triangle three times on both Turnip and
// Qualcomm's driver. A one-output, discarded pass does the writes; a pure GS retains
// the original rasterization. Only split when mutable memory cannot affect outputs
// or control flow. The caller also excludes aliased user UBOs, images and XFB.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { GS_BUFFER = 1, GS_OUTPUT = 2 };
typedef struct {
    uint32_t parent, type, pointee, sc, flags, first, last, next;
    uint8_t pointer, block, taint, memory, returns;
} gsid;

static uint32_t gs_root(gsid *t, uint32_t v) {
    uint32_t r = v;
    while (t[r].parent != r) r = t[r].parent;
    while (v != r) { uint32_t n = t[v].parent; t[v].parent = r; v = n; }
    return r;
}
static void gs_join(gsid *t, uint32_t a, uint32_t b) {
    a = gs_root(t, a); b = gs_root(t, b);
    if (a != b) { t[a].parent = b; t[b].flags |= t[a].flags; }
}
static int gs_tainted(gsid *t, uint32_t bound, uint32_t id) { return id < bound && t[id].taint; }
static int gs_mark(uint8_t *v) { int changed = !*v; *v = 1; return changed; }
static int gs_result(uint32_t op) {
    return op == 12 || op == 54 || op == 55 || op == 57 || op == 59 || op == 61 ||
           (op >= 65 && op <= 68) || (op >= 77 && op <= 215 && op != 99) ||
           (op >= 227 && op <= 242 && op != 228) || op == 245 ||
           (op >= 261 && op <= 271) || (op >= 333 && op <= 366);
}

// OutputPoints requires a PointSize store even when the pipeline discards it.
// Reuse a standalone builtin or gl_PerVertex member; never declare it twice.
static uint32_t *gs_point_size(uint32_t *w, uint32_t *nw) {
    uint32_t f32 = 0, u32 = 0, ptr = 0, one = 0, var = 0, member = 0, block = 0;
    uint32_t firsttype = 0, firstfn = 0, cap = 0, next = w[3], n = *nw;
    for (uint32_t i = 5; i < n; i += w[i] >> 16) {
        const uint32_t *x = w + i; uint32_t op = x[0] & 65535, wc = x[0] >> 16;
        if (op == 17 && x[1] == 24) cap = 1; // GeometryPointSize
        if (!firsttype && op >= 19 && op <= 39) firsttype = i;
        if (!firstfn && op == 54) firstfn = i;
        if (op == 22 && wc == 3 && x[2] == 32) f32 = x[1];
        if (op == 21 && wc == 4 && x[2] == 32 && !x[3]) u32 = x[1];
        if (op == 71 && wc == 4 && x[2] == 11 && x[3] == 1) var = x[1];
        if (op == 72 && wc == 5 && x[3] == 11 && x[4] == 1) { block = x[1]; member = x[2]; }
    }
    if (!firsttype || !firstfn || (var && block)) return NULL;
    // If present in a block, resolve its output variable and scalar type.
    if (block) {
        uint32_t bptr = 0, field = 0;
        for (uint32_t i = 5; i < firstfn; i += w[i] >> 16) {
            const uint32_t *x = w + i; uint32_t op = x[0] & 65535;
            if (op == 30 && x[1] == block && member + 2 < x[0] >> 16) field = x[member + 2];
            if (op == 32 && x[2] == 3 && x[3] == block) bptr = x[1];
        }
        if (!f32 || field != f32 || !bptr) return NULL;
        for (uint32_t i = 5; i < firstfn; i += w[i] >> 16) {
            const uint32_t *x = w + i;
            if ((x[0] & 65535) == 59 && x[1] == bptr && x[3] == 3) { if (var) return NULL; var = x[2]; }
        }
        if (!var) return NULL;
    }
    uint32_t decl[32], nd = 0, index = 0; int addvar = !var;
#define GD(op, ...) do { const uint32_t a[] = {__VA_ARGS__}; decl[nd++] = (uint32_t)(sizeof a / 4 + 1) << 16 | (op); memcpy(decl + nd, a, sizeof a); nd += sizeof a / 4; } while (0)
    if (!f32) { f32 = next++; GD(22, f32, 32); }
    for (uint32_t i = 5; i < firstfn; i += w[i] >> 16) {
        const uint32_t *x = w + i; uint32_t op = x[0] & 65535;
        if (op == 32 && x[2] == 3 && x[3] == f32) ptr = x[1];
        if (op == 43 && x[1] == f32 && x[0] >> 16 == 4 && x[3] == 0x3f800000) one = x[2];
    }
    if (!ptr) { ptr = next++; GD(32, ptr, 3, f32); }
    if (!one) { one = next++; GD(43, f32, one, 0x3f800000); }
    if (block) {
        if (!u32) { u32 = next++; GD(21, u32, 32, 0); }
        index = next++; GD(43, u32, index, member);
    } else if (addvar) { var = next++; GD(59, ptr, var, 3); }
    else {
        int valid = 0;
        for (uint32_t i = 5; i < firstfn; i += w[i] >> 16)
            if ((w[i] & 65535) == 59 && w[i + 2] == var && w[i + 1] == ptr && w[i + 3] == 3) valid = 1;
        if (!valid) return NULL;
    }
#undef GD
    uint32_t *out = malloc(((size_t)n * 3 + 80) * 4), no = 5;
    if (!out) return NULL;
    memcpy(out, w, 20);
    if (!cap) { out[no++] = 2u << 16 | 17; out[no++] = 24; }
    for (uint32_t i = 5; i < n; i += w[i] >> 16) {
        const uint32_t *x = w + i; uint32_t op = x[0] & 65535, wc = x[0] >> 16;
        if (i == firsttype && addvar) { out[no++] = 4u << 16 | 71; out[no++] = var; out[no++] = 11; out[no++] = 1; }
        if (i == firstfn) { memcpy(out + no, decl, nd * 4); no += nd; }
        if (op == 218) {
            uint32_t dest = var;
            if (block) { dest = next++; out[no++] = 5u << 16 | 65; out[no++] = ptr; out[no++] = dest; out[no++] = var; out[no++] = index; }
            out[no++] = 3u << 16 | 62; out[no++] = dest; out[no++] = one;
        }
        memcpy(out + no, x, wc * 4);
        if (op == 15 && addvar) { out[no] += 1u << 16; out[no + wc] = var; no++; }
        no += wc;
    }
    out[3] = next; *nw = no; return out;
}

// mode 1: one output point per invocation (run with rasterizer discard), 2: original
// outputs with buffer stores/atomic writes removed. NULL is a conservative refusal;
// why describes it. The caller must use both variants, or neither.
uint32_t *spv_gs_split(const uint32_t *w, uint32_t nw, int mode, uint32_t *onw, char *why, size_t nwhy) {
    gsid *t = NULL; uint32_t *out = NULL, *zero = NULL;
    const char *reason = "invalid module"; uint32_t bound = 0, entry = 0, fn = 0, maxverts = 0, topology = 0, firstfn = 0, writes = 0;
    if (onw) *onw = 0;
    if (why && nwhy) why[0] = 0;
    if (!w || nw < 5 || w[0] != 0x07230203 || (mode != 1 && mode != 2) || !onw) goto fail;
    bound = w[3]; if (!bound || bound > 1048576) goto fail;
    t = calloc(bound, sizeof *t); if (!t) { reason = "out of memory"; goto fail; }
    for (uint32_t v = 0; v < bound; v++) t[v].parent = v;
    for (uint32_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535; const uint32_t *x = w + i;
        if (!n || n > nw - i) goto fail;
        // Unknown extension instructions may hide a memory effect or a data flow.
        if (op > 366 || op == 259 || op == 318 || op == 319) { reason = "unsupported memory or extension instruction"; goto fail; }
        if (gs_result(op)) {
            if (n < 3 || x[1] >= bound || x[2] >= bound) goto fail;
            t[x[2]].type = x[1];
        }
        if (op == 15) { if (entry || n < 4 || x[1] != 3 || x[2] >= bound) { reason = "not one geometry entry point"; goto fail; } entry = x[2]; }
        if (op == 16 && n >= 3) {
            if (x[2] == 11) { reason = "transform feedback"; goto fail; }
            if (x[2] == 26 && n == 4) maxverts = x[3];
            if (x[2] >= 27 && x[2] <= 29) topology = x[2];
        }
        if (op == 71 && n == 3 && x[2] == 3) { if (x[1] >= bound) goto fail; t[x[1]].block = 1; } // BufferBlock
        if (op == 32) { if (n != 4 || x[1] >= bound || x[3] >= bound) goto fail; t[x[1]].pointer = 1; t[x[1]].sc = x[2]; t[x[1]].pointee = x[3]; }
        if (op == 54) { if (n != 5) goto fail; fn = x[2]; if (!firstfn) firstfn = i; }
        if (op == 55) {
            if (n != 3 || !fn) goto fail;
            if (t[fn].last) t[t[fn].last].next = x[2]; else t[fn].first = x[2];
            t[fn].last = x[2];
        }
        if (op == 56) fn = 0;
        if (op == 220 || op == 221 || op == 99 || op == 63 || op == 64) { reason = "streams, image writes or memory copies"; goto fail; }
        i += n;
    }
    if (!entry || !firstfn || !maxverts || !topology) { reason = "missing geometry execution modes"; goto fail; }
    if (maxverts == 1) { reason = "single-output geometry needs no split"; goto fail; }
    // Resolve pointer aliases, including helper-function in/out parameters. Sharing
    // a helper parameter across callers is conservative: it can reject a safe split.
    for (uint32_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535; const uint32_t *x = w + i;
        if (op == 59) {
            if (n < 4 || !t[x[1]].pointer) goto fail;
            if (x[3] == 12 || (x[3] == 2 && t[t[x[1]].pointee].block)) t[x[2]].flags |= GS_BUFFER;
            if (x[3] == 3) t[x[2]].flags |= GS_OUTPUT;
        }
        if (op == 65 || op == 66) { if (n < 4 || x[3] >= bound) goto fail; gs_join(t, x[2], x[3]); }
        if (op == 83 && t[x[1]].pointer) { if (n != 4 || x[3] >= bound) goto fail; gs_join(t, x[2], x[3]); }
        if ((op == 169 || op == 245) && t[x[1]].pointer) {
            uint32_t start = op == 169 ? 4 : 3, step = op == 169 ? 1 : 2;
            for (uint32_t j = start; j < n; j += step) { if (x[j] >= bound) goto fail; gs_join(t, x[2], x[j]); }
        }
        if (op == 57) {
            if (n < 4 || x[3] >= bound) goto fail;
            if (t[x[1]].pointer) { reason = "pointer-returning helper"; goto fail; }
            uint32_t p = t[x[3]].first;
            for (uint32_t j = 4; j < n; j++) {
                if (!p || x[j] >= bound) goto fail;
                if (t[t[p].type].pointer) gs_join(t, p, x[j]);
                p = t[p].next;
            }
            if (p) goto fail;
        }
        if (op == 61 && t[x[1]].pointer) { reason = "pointer loads"; goto fail; }
        if (gs_result(op) && t[x[1]].pointer && op != 55 && op != 59 && op != 65 && op != 66 && op != 83 && op != 169 && op != 245) {
            reason = "unsupported pointer alias"; goto fail;
        }
        i += n;
    }
    // Mark data read from mutable buffers and results of atomics, propagating
    // through local memory, SSA values, helper arguments and helper returns.
    int changed;
    do {
        changed = 0; fn = 0;
        for (uint32_t i = 5; i < nw;) {
            uint32_t n = w[i] >> 16, op = w[i] & 65535; const uint32_t *x = w + i;
            if (op == 54) fn = x[2];
            if (op == 56) fn = 0;
            if (op == 61) {
                if (n < 4 || x[3] >= bound) goto fail;
                gsid *r = &t[gs_root(t, x[3])];
                if ((r->flags & GS_BUFFER) || r->memory || t[x[3]].taint) changed |= gs_mark(&t[x[2]].taint);
            } else if (op == 62) {
                if (n < 3 || x[1] >= bound || x[2] >= bound) goto fail;
                if (t[x[2]].taint || t[x[1]].taint) changed |= gs_mark(&t[gs_root(t, x[1])].memory);
            } else if (op >= 227 && op <= 242) {
                uint32_t p = op == 228 ? (n > 1 ? x[1] : bound) : (n > 3 ? x[3] : bound);
                if (p >= bound || !(t[gs_root(t, p)].flags & GS_BUFFER)) { reason = "non-buffer atomic"; goto fail; }
                if (op != 228) changed |= gs_mark(&t[x[2]].taint);
            } else if (op == 57) {
                uint32_t p = t[x[3]].first;
                for (uint32_t j = 4; j < n; j++, p = t[p].next)
                    if (t[x[j]].taint) changed |= gs_mark(&t[p].taint);
                if (t[x[3]].returns) changed |= gs_mark(&t[x[2]].taint);
            } else if (op == 254) {
                if (n != 2 || !fn || x[1] >= bound) goto fail;
                if (t[x[1]].taint) changed |= gs_mark(&t[fn].returns);
            } else if (gs_result(op) && op != 54 && op != 55 && op != 59) {
                uint32_t start = op == 12 ? 5 : 3, end = n;
                if (op == 79 || op == 82) end = n < 5 ? n : 5; // vector shuffle / composite insert: later operands are literals
                if (op == 81 || op == 68) end = n < 4 ? n : 4; // composite extract / array length
                for (uint32_t j = start; j < end; j++) if (gs_tainted(t, bound, x[j])) { changed |= gs_mark(&t[x[2]].taint); break; }
            }
            i += n;
        }
    } while (changed);
    for (uint32_t v = 0; v < bound; v++) if (t[v].parent == v && (t[v].flags & GS_OUTPUT) && t[v].memory) { reason = "mutable memory drives raster outputs"; goto fail; }
    for (uint32_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535; const uint32_t *x = w + i;
        if ((op == 250 || op == 251) && (n < 3 || gs_tainted(t, bound, x[1]))) { reason = "mutable memory drives control flow"; goto fail; }
        if (op == 62 && (t[gs_root(t, x[1])].flags & GS_BUFFER)) writes++;
        if (op >= 228 && op <= 242) writes++;
        i += n;
    }
    if (!writes) { reason = "no buffer writes"; goto fail; }
    // Each atomic result in the raster-only variant is an unused placeholder.
    // Constants precede functions, and all original IDs remain stable.
    zero = calloc(bound, sizeof *zero); if (!zero) { reason = "out of memory"; goto fail; }
    uint32_t next = bound, nzero = 0;
    if (mode == 2) for (uint32_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (op >= 229 && op <= 242 && !zero[w[i + 1]]) { zero[w[i + 1]] = next++; nzero++; }
        i += n;
    }
    out = malloc(((size_t)nw * 3 + (size_t)nzero * 3 + 16) * sizeof *out);
    if (!out) { reason = "out of memory"; goto fail; }
    memcpy(out, w, 20); out[3] = next; uint32_t no = 5; fn = 0;
    for (uint32_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535; const uint32_t *x = w + i;
        if (i == firstfn && mode == 2) for (uint32_t ty = 0; ty < bound; ty++) if (zero[ty]) { out[no++] = 3u << 16 | 46; out[no++] = ty; out[no++] = zero[ty]; }
        if (op == 54) fn = x[2];
        if (op == 56) fn = 0;
        if (mode == 1 && (op == 218 || op == 219)) { i += n; continue; }
        if (mode == 2 && ((op == 62 && (t[gs_root(t, x[1])].flags & GS_BUFFER)) || op == 228)) { i += n; continue; }
        if (mode == 2 && op >= 229 && op <= 242) {
            out[no++] = 4u << 16 | 83; out[no++] = x[1]; out[no++] = x[2]; out[no++] = zero[x[1]];
        } else {
            if (mode == 1 && op == 253 && fn == entry) { out[no++] = 1u << 16 | 218; out[no++] = 1u << 16 | 219; }
            memcpy(out + no, x, (size_t)n * 4);
            if (mode == 1 && op == 16 && n >= 3 && x[1] == entry) {
                if (x[2] >= 27 && x[2] <= 29) out[no + 2] = 27; // OutputPoints
                if (x[2] == 26 && n == 4) out[no + 3] = 1;
            }
            no += n;
        }
        i += n;
    }
    if (mode == 1) {
        uint32_t *points = gs_point_size(out, &no);
        if (!points) { reason = "unsupported PointSize output"; goto fail; }
        free(out); out = points;
    }
    *onw = no; free(zero); free(t); return out;
fail:
    if (why && nwhy) snprintf(why, nwhy, "%s", reason);
    free(out); free(zero); free(t); return NULL;
}
