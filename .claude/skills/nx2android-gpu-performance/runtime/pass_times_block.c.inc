// DM_VK_PASS_TIMES=1: a timestamp before and after every render pass (the replay's per-frame line then lists the passes that cost the most GPU time)
enum { PASSQ = 512 };   // passes (and dispatches) timed per submission
static VkQueryPool g_pq; static int g_pq_on = -1;
typedef struct { uint32_t w, h, fmt, draws, fidx; } pass_meta;   // fidx: separate render/dispatch ordinals, across submission cuts
// Compute dispatch counts vary even in a fixed view. Counting them with render passes shifted the material/depth rows between frames (1440p Metro).
enum { FPASSQ = 1024, DPASSQ = FPASSQ / 2 }; static int g_fpass, g_fdispatch;
void vk_pass_frame(void) { g_fpass = g_fdispatch = 0; }   // a present: restart both ordinals
static pass_meta g_pmeta[NSLOT][PASSQ]; static int g_npass[NSLOT];
static double g_pacc[FPASSQ]; static uint32_t g_pcnt[FPASSQ]; static pass_meta g_plast[FPASSQ]; static int g_pmax;   // by the pass's place in its frame (the completion thread adds)
static pthread_mutex_t g_pass_mu = PTHREAD_MUTEX_INITIALIZER;
static int pass_times_on(void) { if (g_pq_on < 0) { const char *e = getenv("DM_VK_PASS_TIMES"); g_pq_on = e && atoi(e); } return g_pq_on && g_pq; }
void vk_pass_ts(int end, uint32_t w, uint32_t h, uint32_t fmt, uint32_t draws) {
    if (!pass_times_on() || g_cur < 0) return; int *n = &g_npass[g_cur]; if (*n >= PASSQ) return;
    vkCmdWriteTimestamp(g_slot_main_cb(), end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_pq, (uint32_t)(g_cur * PASSQ * 2 + *n * 2 + end));
    if (end) { int dispatch = fmt == PT_DISPATCH, ordinal = dispatch ? g_fdispatch++ : g_fpass++;
        uint32_t fidx = (uint32_t)(ordinal < DPASSQ - 1 ? ordinal : DPASSQ - 1) + (dispatch ? DPASSQ : 0);
        g_pmeta[g_cur][*n] = (pass_meta){w, h, fmt, draws, fidx}; (*n)++; }
}
static void pass_accumulate(double ms, const pass_meta *meta) {
    pthread_mutex_lock(&g_pass_mu);
    int f = (int)meta->fidx; g_pacc[f] += ms; g_pcnt[f]++; g_plast[f] = *meta; if (f + 1 > g_pmax) g_pmax = f + 1;
    pthread_mutex_unlock(&g_pass_mu);
}
void vk_pass_report(void) {   // the most expensive passes since the last report, then the counters start again
    if (!pass_times_on()) return;
    pthread_mutex_lock(&g_pass_mu);
    static int idx[FPASSQ]; int m = 0; double tot = 0;
    for (int i = 0; i < g_pmax; i++) if (g_pcnt[i]) { idx[m++] = i; tot += g_pacc[i] / g_pcnt[i]; }
    for (int i = 0; i < m; i++) for (int j = i + 1; j < m; j++) if (g_pacc[idx[j]] / g_pcnt[idx[j]] > g_pacc[idx[i]] / g_pcnt[idx[i]]) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    char b[3000]; int o = snprintf(b, sizeof b, "passes by GPU time (%d passes, %.2f ms in passes per frame):", m, tot);
    for (int i = 0; i < m && i < 20 && o < (int)sizeof b - 80; i++) { int k = idx[i]; if (!g_pcnt[k]) continue;
        if (g_plast[k].fmt == PT_DISPATCH) o += snprintf(b + o, sizeof b - (size_t)o, " dispatch#%d %.2f ms (%ux%ux%u);", k - DPASSQ, g_pacc[k] / g_pcnt[k], g_plast[k].w, g_plast[k].h, g_plast[k].draws);
        else o += snprintf(b + o, sizeof b - (size_t)o, " #%d %.2f ms (%ux%u fmt %u, %u draws over %u px wide);", k, g_pacc[k] / g_pcnt[k], g_plast[k].w, g_plast[k].h, g_plast[k].fmt, g_plast[k].draws & 0xffff, g_plast[k].draws >> 16); }
    { extern uint64_t g_nvn_discards[2]; static uint64_t pd0, pd1; o += snprintf(b + o, sizeof b - (size_t)o, " discards since: colour %llu, depth %llu", (unsigned long long)(g_nvn_discards[0] - pd0), (unsigned long long)(g_nvn_discards[1] - pd1)); pd0 = g_nvn_discards[0]; pd1 = g_nvn_discards[1]; }
    { const char *full = getenv("DM_VK_PASS_FULL"); if (full && atoi(full)) {
        for (int k = 0; k < g_pmax; k++) if (g_pcnt[k]) {
            const pass_meta *p = &g_plast[k];
            VKLOG("GPU full pass row: %s#%d mean %.6f ms samples %u size %ux%u depth_or_format %u draws_or_z %u coverage_width %u",
                p->fmt == PT_DISPATCH ? "dispatch" : "render", p->fmt == PT_DISPATCH ? k - DPASSQ : k,
                g_pacc[k] / g_pcnt[k], g_pcnt[k], p->w, p->h, p->fmt, p->fmt == PT_DISPATCH ? p->draws : p->draws & 0xffff, p->draws >> 16);
        }
    } }
    VKLOG("%s", b);
    memset(g_pacc, 0, sizeof g_pacc); memset(g_pcnt, 0, sizeof g_pcnt); g_pmax = 0;
    pthread_mutex_unlock(&g_pass_mu);
}
