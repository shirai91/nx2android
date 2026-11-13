// One-frame GPU range localization. Markers perturb scheduling; not hardware-unit time.
#include "gfx_vk.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { RT_SLOTS = 6, RT_MARKS = 256, RT_DRAWS = 1024 };
typedef struct { uint32_t pass, draw, global, dt, dw, df; uint64_t vs, fs; } rt_draw;
typedef struct { uint32_t pass, phase, part, w, h, draws, stage; uint64_t cpu_wall_ns, cpu_thread_ns; } rt_meta;
static VkQueryPool g_rt_pool;
static int g_rt_failed, g_rt_current = -1;
static uint64_t g_rt_target, g_rt_serial;
static uint32_t g_rt_first = 0, g_rt_last = 0, g_rt_mode;
static uint32_t g_rt_step = 64, g_rt_draw_first, g_rt_draw_last = 256;
static struct {
    VkCommandBuffer cb;
    uint64_t frame, serial;
    uint32_t count, dropped, capture, part, first, last, mode, step, draw_first, draw_last, ndraw;
    rt_meta meta[RT_MARKS];
    rt_draw draws[RT_DRAWS];
} g_rt[RT_SLOTS];

static int rt_init(void) {
    if (g_rt_pool) return 1;
    if (g_rt_failed) return 0;
    if (!V.range_timestamp_bits || V.range_timestamp_bits > 64 || !(V.props.limits.timestampPeriod > 0)) {
        g_rt_failed = 1; VKLOG("GPU range unavailable: invalid timestamp properties"); return 0;
    }
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, RT_SLOTS * RT_MARKS, 0};
    if (vkCreateQueryPool(V.dev, &ci, NULL, &g_rt_pool) != VK_SUCCESS) {
        g_rt_pool = VK_NULL_HANDLE; g_rt_failed = 1; VKLOG("GPU range unavailable: query allocation failed"); return 0;
    }
    return 1;
}

// Immediately after primary command-buffer begin. Only part0 resets a recycled slot.
void vk_rt_part(VkCommandBuffer cb, int slot, int part, uint64_t frame) {
    if (!cb || slot < 0 || slot >= RT_SLOTS || part < 0 || part >= 32) return;
    const char *s = getenv("DM_VK_RANGE_TIMES");
    int mode = s ? atoi(s) : 0;
    if (mode) {
        setenv("DM_VK_RANGE_TIMES", "0", 1);
        const char *lo = getenv("DM_VK_RANGE_FIRST"), *hi = getenv("DM_VK_RANGE_LAST");
        int first = lo ? atoi(lo) : 0, last = hi ? atoi(hi) : first;
        const char *ss = getenv("DM_VK_RANGE_DRAW_STEP"), *ds = getenv("DM_VK_RANGE_DRAW_FIRST"), *de = getenv("DM_VK_RANGE_DRAW_LAST");
        int step = ss ? atoi(ss) : 64, df = ds ? atoi(ds) : 0, dl = de ? atoi(de) : 256;
        if (mode < 1 || mode > 6 || first < 0 || last < first || last > 511 ||
            step < 1 || step > 4096 || df < 0 || dl <= df || dl > 4096) {
            VKLOG("GPU range unavailable: invalid mode/pass range");
        } else if (rt_init()) {
            g_rt_target = frame + 1; g_rt_serial++; g_rt_mode = (uint32_t)mode;
            g_rt_first = (uint32_t)first; g_rt_last = (uint32_t)last;
            g_rt_step = (uint32_t)step; g_rt_draw_first = (uint32_t)df; g_rt_draw_last = (uint32_t)dl;
            VKLOG("GPU range armed: capture %" PRIu64 " frame %" PRIu64 " mode %d passes %d..%d",
                  g_rt_serial, g_rt_target, mode, first, last);
        }
    }
    g_rt_current = slot;
    if (!part) {
        g_rt[slot].count = g_rt[slot].dropped = g_rt[slot].ndraw = 0;
        g_rt[slot].frame = frame; g_rt[slot].serial = g_rt_serial;
        g_rt[slot].capture = g_rt_pool && g_rt_target && frame == g_rt_target;
        g_rt[slot].first = g_rt_first; g_rt[slot].last = g_rt_last; g_rt[slot].mode = g_rt_mode;
        g_rt[slot].step = g_rt_step; g_rt[slot].draw_first = g_rt_draw_first; g_rt[slot].draw_last = g_rt_draw_last;
        if (g_rt[slot].capture && g_rt[slot].mode != 6) vkCmdResetQueryPool(cb, g_rt_pool, (uint32_t)slot * RT_MARKS, RT_MARKS);
    }
    g_rt[slot].cb = cb; g_rt[slot].part = (uint32_t)part;
}

//0after cut/before barrier(TOP),1after barrier,2after cut-depth touch,
//3before commit-depth touches,4before render begin,5after begin(fine only),
//6before end(fine only),7after end. All except0are BOTTOM markers.
void vk_rt_mark(VkCommandBuffer cb, uint32_t pass, uint32_t phase, uint32_t w, uint32_t h, uint32_t draws) {
    if (!g_rt_pool || g_rt_current < 0 || g_rt_current >= RT_SLOTS || phase > 8) return;
    int slot = g_rt_current;
    if (!g_rt[slot].capture || g_rt[slot].cb != cb || pass < g_rt[slot].first || pass > g_rt[slot].last) return;
    if (g_rt[slot].mode == 6) return;
    if (phase == 8 && g_rt[slot].mode < 4) return;
    if (g_rt[slot].mode == 5 && phase != 0 && phase != 7 && phase != 8 && !(phase == 5 && !g_rt[slot].draw_first)) return;
    if (g_rt[slot].mode == 3 && phase != 0 && phase != 7) return;
    if (g_rt[slot].mode == 1 && (phase == 5 || phase == 6)) return;
    uint32_t n = g_rt[slot].count;
    if (n >= RT_MARKS) { g_rt[slot].dropped++; return; }
    VkPipelineStageFlagBits stage = phase ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    g_rt[slot].meta[n] = (rt_meta){pass, phase, g_rt[slot].part, w, h, draws, (uint32_t)stage, 0, 0};
    struct timespec wall, cpu; clock_gettime(CLOCK_MONOTONIC, &wall); clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    g_rt[slot].meta[n].cpu_wall_ns = (uint64_t)wall.tv_sec * UINT64_C(1000000000) + (uint64_t)wall.tv_nsec;
    g_rt[slot].meta[n].cpu_thread_ns = (uint64_t)cpu.tv_sec * UINT64_C(1000000000) + (uint64_t)cpu.tv_nsec;
    vkCmdWriteTimestamp(cb, stage, g_rt_pool, (uint32_t)slot * RT_MARKS + n);
    g_rt[slot].count++;
}

// Actual draw command callback; preparation and pass boundaries are unchanged.
void vk_rt_draw(VkCommandBuffer cb, uint32_t pass, uint32_t draw, uint32_t global, uint32_t w, uint32_t h,
                uint64_t vs, uint64_t fs, uint32_t dt, uint32_t dw, uint32_t df) {
    if (!g_rt_pool || g_rt_current < 0 || g_rt_current >= RT_SLOTS) return;
    int slot = g_rt_current;
    if (!g_rt[slot].capture || g_rt[slot].cb != cb || g_rt[slot].mode < 4 || pass < g_rt[slot].first || pass > g_rt[slot].last) return;
    if (g_rt[slot].ndraw >= RT_DRAWS) { g_rt[slot].dropped++; return; }
    g_rt[slot].draws[g_rt[slot].ndraw++] = (rt_draw){pass, draw, global, dt, dw, df, vs, fs};
    int mark = g_rt[slot].mode == 4 ? draw && !(draw % g_rt[slot].step) :
               g_rt[slot].mode == 5 && (draw == g_rt[slot].draw_first || draw == g_rt[slot].draw_last);
    if (mark) vk_rt_mark(cb, pass, 8, w, h, draw);
}

// After the final successful submission fence, before slot completion/reuse.
void vk_rt_complete(uint64_t frame, int slot) {
    if (!g_rt_pool || slot < 0 || slot >= RT_SLOTS || !g_rt[slot].capture || g_rt[slot].frame != frame) return;
    g_rt[slot].capture = 0;
    uint32_t n = g_rt[slot].count;
    uint64_t ticks[RT_MARKS];
    VkResult result = n ? vkGetQueryPoolResults(V.dev, g_rt_pool, (uint32_t)slot * RT_MARKS, n,
        (size_t)n * sizeof(uint64_t), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) : VK_SUCCESS;
    if (result != VK_SUCCESS) { VKLOG("GPU range unavailable: frame %" PRIu64 " slot %d result %d", frame, slot, result); return; }
    uint64_t mask = V.range_timestamp_bits == 64 ? UINT64_MAX : (UINT64_C(1) << V.range_timestamp_bits) - 1;
    VKLOG("GPU range begin: capture %" PRIu64 " frame %" PRIu64 " slot %d mode %u markers %u dropped %u bits %u period %.9g step %u first_draw %u last_draw %u draw_rows %u",
          g_rt[slot].serial, frame, slot, g_rt[slot].mode, n, g_rt[slot].dropped, V.range_timestamp_bits, (double)V.props.limits.timestampPeriod, g_rt[slot].step, g_rt[slot].draw_first, g_rt[slot].draw_last, g_rt[slot].ndraw);
    for (uint32_t q = 0; q < n; q++) {
        const rt_meta *m = &g_rt[slot].meta[q];
        VKLOG("GPU range row: capture %" PRIu64 " frame %" PRIu64 " slot %d query %u pass %u phase %u part %u %ux%u draws %u stage %u ticks %" PRIu64 " cpu_wall %" PRIu64 " cpu_thread %" PRIu64,
              g_rt[slot].serial, frame, slot, q, m->pass, m->phase, m->part, m->w, m->h, m->draws, m->stage, ticks[q] & mask, m->cpu_wall_ns, m->cpu_thread_ns);
    }
    for (uint32_t i = 0; i < g_rt[slot].ndraw; i++) {
        const rt_draw *d = &g_rt[slot].draws[i];
        VKLOG("GPU range draw: capture %" PRIu64 " frame %" PRIu64 " slot %d ordinal %u pass %u local_draw %u global_draw %u vs %016" PRIx64 " fs %016" PRIx64 " depth %u/%u/%u",
              g_rt[slot].serial, frame, slot, i, d->pass, d->draw, d->global, d->vs, d->fs, d->dt, d->dw, d->df);
    }
    VKLOG("GPU range end: capture %" PRIu64 " frame %" PRIu64 " slot %d", g_rt[slot].serial, frame, slot);
}
