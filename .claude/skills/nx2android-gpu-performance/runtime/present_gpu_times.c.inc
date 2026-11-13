// Diagnostic-only GPU intervals for the presentation copy, overlay and rotation.
// Called under g_present_mu. Results are read only after the slot's successful fence wait.
enum { PGT_SLOTS = 8, PGT_POINTS = 5 };
static VkQueryPool g_pgt_pool;
static int g_pgt_on = -1, g_pgt_used[PGT_SLOTS], g_pgt_active[PGT_SLOTS];
static uint64_t g_pgt_ticket[PGT_SLOTS], g_pgt_ns[PGT_POINTS - 1], g_pgt_count;
static int present_gpu_enabled(void) {
    if (g_pgt_on < 0) { const char *a = getenv("DM_VK_PASS_TIMES"), *b = getenv("DM_VK_PASS_FULL"); g_pgt_on = a && atoi(a) && b && atoi(b); }
    return g_pgt_on;
}
static void present_gpu_recycle(unsigned k) {
    if (k >= PGT_SLOTS || !g_pgt_used[k]) return;
    g_pgt_used[k] = 0;
    uint64_t t[PGT_POINTS];
    if (vkGetQueryPoolResults(V.dev, g_pgt_pool, k * PGT_POINTS, PGT_POINTS, sizeof t, t, sizeof *t, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        VKLOG("[RED] present GPU timing results unavailable for ticket %llu", (unsigned long long)g_pgt_ticket[k]); return;
    }
    for (int i = 1; i < PGT_POINTS; i++) if (t[i] < t[i - 1]) { VKLOG("[RED] present GPU timing rejected nonmonotonic results"); return; }
    if (!g_pgt_ticket[k]) return;   // overlay-only redraws are not game frames
    for (int i = 0; i < PGT_POINTS - 1; i++) g_pgt_ns[i] += (uint64_t)((double)(t[i + 1] - t[i]) * V.props.limits.timestampPeriod);
    if (++g_pgt_count == 300) {
        double scale = 1e-6 / g_pgt_count;
        VKLOG("present GPU steps (%llu game frames): initial transitions/clear %.6f ms, final image blit %.6f ms, overlay preparation/render %.6f ms, rotation/final transition %.6f ms; total %.6f ms",
            (unsigned long long)g_pgt_count, g_pgt_ns[0] * scale, g_pgt_ns[1] * scale, g_pgt_ns[2] * scale, g_pgt_ns[3] * scale,
            (g_pgt_ns[0] + g_pgt_ns[1] + g_pgt_ns[2] + g_pgt_ns[3]) * scale);
        memset(g_pgt_ns, 0, sizeof g_pgt_ns); g_pgt_count = 0;
    }
}
static void present_gpu_begin(VkCommandBuffer cb, unsigned k, uint64_t ticket) {
    if (k >= PGT_SLOTS) return;
    g_pgt_active[k] = g_pgt_used[k] = 0;
    if (!present_gpu_enabled()) return;
    if (!g_pgt_pool) {
        if (!V.props.limits.timestampComputeAndGraphics) { g_pgt_on = 0; return; }
        VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, PGT_SLOTS * PGT_POINTS};
        if (vkCreateQueryPool(V.dev, &ci, NULL, &g_pgt_pool) != VK_SUCCESS) { g_pgt_pool = VK_NULL_HANDLE; g_pgt_on = 0; VKLOG("[RED] present GPU timing query allocation failed; capture disabled"); return; }
    }
    vkCmdResetQueryPool(cb, g_pgt_pool, k * PGT_POINTS, PGT_POINTS);
    g_pgt_active[k] = 1; g_pgt_ticket[k] = ticket;
    // BOTTOM markers avoid charging the game's preceding GPU tail to a TOP-start presentation span.
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pgt_pool, k * PGT_POINTS);
}
static void present_gpu_mark(VkCommandBuffer cb, unsigned k, unsigned point) {
    if (k < PGT_SLOTS && point && point < PGT_POINTS && g_pgt_active[k])
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pgt_pool, k * PGT_POINTS + point);
}
static void present_gpu_submitted(unsigned k, int success) {
    if (k < PGT_SLOTS) { g_pgt_used[k] = success && g_pgt_active[k]; g_pgt_active[k] = 0; }
}
