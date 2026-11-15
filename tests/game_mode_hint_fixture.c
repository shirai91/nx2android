// Production Game Mode pacing and ADPF reporting with deterministic clock/API boundaries.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define LOG(...) ((void)0)
#ifdef _WIN32
static int unsetenv(const char *k) { char s[256]; snprintf(s, sizeof s, "%s=", k); return _putenv(s); }
#endif
typedef int AWorkDuration;
static AWorkDuration *g_wd;
static void *g_hint = (void *)1;
static int64_t g_hint_target = 16666667, g_frame_gpu = 6000000;
static int64_t g_work_start, g_prev_present, g_pace, g_pq, now, observed_work;
static int increases, reports;
static int report(void *session, int64_t work) { (void)session; assert(work == 33333333); reports++; return 0; }
static int increase(void *session, bool cpu, bool gpu, const char *reason) {
    (void)session; (void)gpu; (void)reason; assert(cpu); increases++; return 0;
}
static struct {
    int (*report2)(void *, AWorkDuration *);
    void (*wd_start)(AWorkDuration *, int64_t), (*wd_total)(AWorkDuration *, int64_t);
    void (*wd_cpu)(AWorkDuration *, int64_t), (*wd_gpu)(AWorkDuration *, int64_t);
    int (*report)(void *, int64_t);
    int (*increase)(void *, bool, bool, const char *);
    void (*update)(void *, int64_t);
} H = {.report = report, .increase = increase};
static int64_t perf_now(void) { return now; }
static void hint_init(int64_t period) { assert(period == 16666667); }
static void gpu_turbo(void) {}
static void hint_threads_scan(int32_t tid) { (void)tid; }
static int32_t gettid(void) { return 1; }
static void adapt_frame(int64_t work, int64_t time) { (void)time; observed_work = work; }
/* PRODUCTION */
int main(void) {
    unsetenv("DM_ADAPTIVE_FPS"); unsetenv("DM_FPS_PIN");
    dm_perf_game_mode(2); game_mode_sync();
    g_prev_present = 1000000000;
    for (int i = 0; i < 60; i++) {
        uint64_t period = dm_present_period_adapt(16666667);
        assert(period == (PROFILE_CEILING == 30 ? 33333333 : 16666667));
        g_pq = PROFILE_CEILING == 30 ? 25000000 : 0;
        now = g_prev_present + 33333333;
        g_work_start = g_prev_present + g_pq;
        dm_perf_frame((double)period / 1e9);
        assert(observed_work == 33333333 - g_pq);
    }
    assert(reports == 60);
    // A cap's present-thread wait is pacing. Real uncapped work must still signal load.
    assert(PROFILE_CEILING == 30 ? increases == 0 : increases > 0);
    return 0;
}
