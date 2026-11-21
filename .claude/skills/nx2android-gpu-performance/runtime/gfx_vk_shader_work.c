// Bounded per-draw pipeline statistics. Diagnostic counts are not timing or utilization.
#include "gfx_vk.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

enum { SW_SLOTS = 6, SW_QUERIES = 4096, SW_VALUES = 6 };
typedef struct { uint64_t vs, fs; uint32_t pass, draw, w, h, dt, dw, df; } sw_meta;
static VkQueryPool g_sw_pool;
static int g_sw_failed, g_sw_current = -1;
static uint64_t g_sw_target, g_sw_serial;
static uint32_t g_sw_first = 0, g_sw_last = 0;
static struct {
    VkCommandBuffer cb;
    uint64_t frame, serial;
    uint32_t count, active, dropped, capture;
    sw_meta meta[SW_QUERIES];
} g_sw[SW_SLOTS];

static int sw_init(void) {
    if (g_sw_pool) return 1;
    if (g_sw_failed) return 0;
    if (!V.has_shader_work) { g_sw_failed = 1; VKLOG("shader workload unavailable: pipelineStatisticsQuery unsupported"); return 0; }
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_PIPELINE_STATISTICS,
        SW_SLOTS * SW_QUERIES, VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
        VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};
    if (vkCreateQueryPool(V.dev, &qi, NULL, &g_sw_pool) != VK_SUCCESS) {
        g_sw_pool = VK_NULL_HANDLE; g_sw_failed = 1; VKLOG("shader workload unavailable: query pool allocation failed"); return 0;
    }
    return 1;
}

// Called just after command-buffer begin, outside every render pass. Slot reuse has waited for completion.
void vk_sw_part(VkCommandBuffer cb, int slot, int part, uint64_t frame) {
    if (!cb || slot < 0 || slot >= SW_SLOTS || part < 0 || part >= 32) return;
    const char *e = getenv("DM_VK_SHADER_WORK");
    if (e && atoi(e) > 0) {
        setenv("DM_VK_SHADER_WORK", "0", 1);
        if (sw_init()) {
            const char *lo = getenv("DM_VK_SHADER_WORK_FIRST"), *hi = getenv("DM_VK_SHADER_WORK_LAST");
            int first = lo ? atoi(lo) : 0, last = hi ? atoi(hi) : first;
            if (first < 0 || last < first || last > 511) { VKLOG("shader workload unavailable: invalid pass range"); return; }
            g_sw_first = (uint32_t)first; g_sw_last = (uint32_t)last;
            g_sw_target = frame + 1; g_sw_serial++; VKLOG("shader workload armed: capture %" PRIu64 " frame %" PRIu64, g_sw_serial, g_sw_target); }
    }
    g_sw_current = slot;
    if (!part) {
        g_sw[slot].count = g_sw[slot].active = g_sw[slot].dropped = 0;
        g_sw[slot].frame = frame; g_sw[slot].serial = g_sw_serial;
        g_sw[slot].capture = g_sw_pool && g_sw_target && frame == g_sw_target;
        if (g_sw[slot].capture) vkCmdResetQueryPool(cb, g_sw_pool, (uint32_t)slot * SW_QUERIES, SW_QUERIES);
    }
    g_sw[slot].cb = cb;
}

uint32_t vk_sw_begin(VkCommandBuffer cb, uint64_t vs, uint64_t fs, uint32_t pass, uint32_t draw,
                     uint32_t w, uint32_t h, uint32_t dt, uint32_t dw, uint32_t df) {
    if (!g_sw_pool || g_sw_current < 0 || g_sw_current >= SW_SLOTS) return 0;
    int slot = g_sw_current;
    if (!g_sw[slot].capture || g_sw[slot].cb != cb || g_sw[slot].active) return 0;
    if (pass < g_sw_first || pass > g_sw_last) return 0;
    uint32_t q = g_sw[slot].count;
    if (q >= SW_QUERIES) { g_sw[slot].dropped++; return 0; }
    g_sw[slot].meta[q] = (sw_meta){vs, fs, pass, draw, w, h, dt, dw, df};
    vkCmdBeginQuery(cb, g_sw_pool, (uint32_t)slot * SW_QUERIES + q, 0);
    g_sw[slot].active = q + 1; g_sw[slot].count++;
    return q + 1;
}

void vk_sw_end(VkCommandBuffer cb, uint32_t query) {
    if (g_sw_current < 0 || g_sw_current >= SW_SLOTS || !query) return;
    int slot = g_sw_current;
    if (g_sw[slot].cb != cb || g_sw[slot].active != query) return;
    vkCmdEndQuery(cb, g_sw_pool, (uint32_t)slot * SW_QUERIES + query - 1);
    g_sw[slot].active = 0;
}

// Completion thread only, after the submission fence. Failed/dropped work must not call this.
void vk_sw_complete(uint64_t frame, int slot) {
    if (!g_sw_pool || slot < 0 || slot >= SW_SLOTS || !g_sw[slot].capture ||
        g_sw[slot].frame != frame || g_sw[slot].active || !g_sw[slot].count) return;
    uint32_t n = g_sw[slot].count;
    uint64_t values[SW_QUERIES][SW_VALUES];
    VkResult r = vkGetQueryPoolResults(V.dev, g_sw_pool, (uint32_t)slot * SW_QUERIES, n,
        (size_t)n * sizeof values[0], values, sizeof values[0], VK_QUERY_RESULT_64_BIT);
    g_sw[slot].capture = 0;
    if (r != VK_SUCCESS) { VKLOG("shader workload unavailable: frame %" PRIu64 " slot %d result %d", frame, slot, r); return; }
    VKLOG("shader workload begin: capture %" PRIu64 " frame %" PRIu64 " slot %d queries %u dropped %u (counts, not timings)",
        g_sw[slot].serial, frame, slot, n, g_sw[slot].dropped);
    for (uint32_t q = 0; q < n; q++) {
        sw_meta *m = &g_sw[slot].meta[q];
        VKLOG("shader workload row: capture %" PRIu64 " frame %" PRIu64 " slot %d query %u pass %u draw %u %ux%u vs %016" PRIx64 " fs %016" PRIx64
            " depth %u/%u/%u IA %" PRIu64 "/%" PRIu64 " VS %" PRIu64 " CLIP %" PRIu64 "/%" PRIu64 " FS %" PRIu64,
            g_sw[slot].serial, frame, slot, q, m->pass, m->draw, m->w, m->h, m->vs, m->fs, m->dt, m->dw, m->df,
            values[q][0], values[q][1], values[q][2], values[q][3], values[q][4], values[q][5]);
    }
    VKLOG("shader workload end: capture %" PRIu64 " frame %" PRIu64 " slot %d queries %u", g_sw[slot].serial, frame, slot, n);
}
