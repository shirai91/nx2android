// Pure range-hazard helpers for batching separated geometry-shader effects draws
// (gfx_vk_exec_xfb.inc). Kept Vulkan-free so host C tests compile it directly.
#pragma once
#include <stdint.h>

enum { GS_BATCH_MAX = 1024, GS_WR_MAX = 256 };
typedef struct { uint64_t addr, size; } gs_range_t;
typedef struct { gs_range_t r[GS_WR_MAX]; int n; } gs_wr_set_t;

static inline int gs_ranges_overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return a && b && n && m && (a >= b ? a - b < m : b - a < n);
}

static inline int gs_wr_overlaps(const gs_wr_set_t *w, uint64_t a, uint64_t n) {
    if (!a || !n) return 0;
    for (int i = 0; i < w->n; i++)
        if (gs_ranges_overlap(w->r[i].addr, w->r[i].size, a, n)) return 1;
    return 0;
}

// Add [a, a + n) to the deduplicated/coalesced write set.
// Returns 1 on success, 0 if invalid or full (caller flushes the batch first).
static inline int gs_wr_add(gs_wr_set_t *w, uint64_t a, uint64_t n) {
    if (!a || !n || n > UINT64_MAX - a) return 0;
    uint64_t end = a + n;
    for (int i = 0; i < w->n; i++) {
        uint64_t ra = w->r[i].addr, re = ra + w->r[i].size;
        if (ra <= a && end <= re) return 1;
        if (a <= re && ra <= end) {
            uint64_t lo = a < ra ? a : ra, hi = end > re ? end : re;
            w->r[i].addr = lo; w->r[i].size = hi - lo;
            // Coalesce any later entries now overlapping or adjacent to [lo, hi).
            for (int j = i + 1; j < w->n; ) {
                uint64_t ja = w->r[j].addr, je = ja + w->r[j].size;
                if (lo <= je && ja <= hi) {
                    if (ja < lo) lo = ja;
                    if (je > hi) hi = je;
                    w->r[i].addr = lo; w->r[i].size = hi - lo;
                    w->r[j] = w->r[--w->n];
                } else j++;
            }
            return 1;
        }
    }
    if (w->n >= GS_WR_MAX) return 0;
    w->r[w->n++] = (gs_range_t){a, n};
    return 1;
}
