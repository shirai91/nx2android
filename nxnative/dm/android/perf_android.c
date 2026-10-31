// Android port: display rate and CPU performance hints for the presents (gfx_vk_present.c calls dm_perf_frame before each present).
// - Display rate: Android's FIFO swapchain follows the display, which on many devices runs at 90, 120 or 144 Hz; the NVN layer paces
//   presents to the game's period (hle_nvn.c acquire), and the window asks the display for a rate that period divides (dm_perf_window).
// - ADPF (APerformanceHint, Android 13): the presenting thread reports how long each frame's work took against the frame period, so the
//   CPU governor raises clocks before a frame runs late instead of after (the hint is ignored where the API is missing).
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <sched.h>
#include "dm.h"

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef struct AWorkDuration AWorkDuration;
static struct {
    APerformanceHintManager *(*get)(void);
    APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    int (*update)(APerformanceHintSession *, int64_t);
    int (*report)(APerformanceHintSession *, int64_t);
    int (*set_threads)(APerformanceHintSession *, const int32_t *, size_t);
    // Android 15: the frame's GPU time beside its CPU time (the GPU's clock follows it too)
    int (*report2)(APerformanceHintSession *, AWorkDuration *);
    AWorkDuration *(*wd_create)(void);
    void (*wd_start)(AWorkDuration *, int64_t); void (*wd_total)(AWorkDuration *, int64_t); void (*wd_cpu)(AWorkDuration *, int64_t); void (*wd_gpu)(AWorkDuration *, int64_t);
    // Android 16: a load that rises now, for the clocks to follow at once rather than over the next frames
    int (*increase)(APerformanceHintSession *, bool cpu, bool gpu, const char *name);
} H;
static APerformanceHintSession *g_hint; static int64_t g_hint_target; static int32_t g_hint_tids[8]; static int g_hint_n;
static AWorkDuration *g_wd;   // made once, its fields set for each frame's report

static int64_t perf_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }

static void hint_init(int64_t target) {
    static int tried; if (tried) return; tried = 1;
    if (getenv("DM_NO_ADPF")) return;
    void *lib = dlopen("libandroid.so", RTLD_NOW); if (!lib) return;
    H.get = dlsym(lib, "APerformanceHint_getManager"); H.create = dlsym(lib, "APerformanceHint_createSession");
    H.update = dlsym(lib, "APerformanceHint_updateTargetWorkDuration"); H.report = dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    H.set_threads = dlsym(lib, "APerformanceHint_setThreads");   // Android 14
    H.report2 = dlsym(lib, "APerformanceHint_reportActualWorkDuration2");   // Android 15
    H.wd_create = dlsym(lib, "AWorkDuration_create");
    H.wd_start = dlsym(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos"); H.wd_total = dlsym(lib, "AWorkDuration_setActualTotalDurationNanos");
    H.wd_cpu = dlsym(lib, "AWorkDuration_setActualCpuDurationNanos"); H.wd_gpu = dlsym(lib, "AWorkDuration_setActualGpuDurationNanos");
    if (!H.wd_create || !H.wd_start || !H.wd_total || !H.wd_cpu || !H.wd_gpu || !(H.report2 && (g_wd = H.wd_create()))) H.report2 = NULL;
    H.increase = dlsym(lib, "APerformanceHint_notifyWorkloadIncrease");   // Android 16
    if (!H.get || !H.create || !H.report) { LOG("[perf] no APerformanceHint on this Android: CPU hints off"); return; }
    APerformanceHintManager *m = H.get(); if (!m) return;
    g_hint_tids[0] = (int32_t)gettid(); g_hint_n = 1;
    g_hint = H.create(m, g_hint_tids, (size_t)g_hint_n, target); g_hint_target = target;
    LOG("[perf] ADPF session %s (target %.2f ms, thread %d; GPU time %s, load increases %s)", g_hint ? "open" : "refused", target / 1e6, g_hint_tids[0],
        H.report2 ? "reported" : "not reported", H.increase ? "signalled" : "not signalled");
}
// The other threads that do frame work: the presenting thread is the game's render thread, and its main thread (the simulation) and job workers set
// the pace as much. Every 300 frames the threads that used at least 15% of a core since the last look join the session (up to 5 besides the
// presenting one), the ones that went idle leave it. Without them NEO's main thread ran on mid cores at 1.9 of 3.5 GHz and its bursts made
// 45 to 80 ms frames. Needs APerformanceHint_setThreads (Android 14)
static uint64_t task_ticks(int tid) {
    char p[64], b[512]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    char *c = strrchr(b, ')'); if (!c) return 0;
    unsigned long long ut = 0, st = 0; int field = 2;
    for (char *t = c + 1; *t && field < 15; t++) if (*t == ' ') { field++; if (field == 14) ut = strtoull(t + 1, NULL, 10); else if (field == 15) st = strtoull(t + 1, NULL, 10); }
    return ut + st;
}
static void hint_threads_scan(int32_t self) {
    static struct { int tid; uint64_t ticks; } prev[512]; static int np; static int64_t last;
    int64_t now = perf_now(); double secs = last ? (now - last) / 1e9 : 0; last = now;
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    struct { int tid; uint64_t used; } cur[512]; int nc = 0; struct dirent *e;
    while ((e = readdir(d)) && nc < 512) { int tid = atoi(e->d_name); if (tid <= 0) continue; cur[nc].tid = tid; cur[nc].used = task_ticks(tid); nc++; }
    closedir(d);
    int32_t pick[8]; int npick = 0; pick[npick++] = self;
    if (secs > 0) {
        long hz = sysconf(_SC_CLK_TCK); double need = 0.15 * secs * (double)(hz > 0 ? hz : 100);
        for (int k = 0; k < 5; k++) {   // the busiest first
            int best = -1; uint64_t bu = 0;
            for (int i = 0; i < nc; i++) {
                if (cur[i].tid == self) continue;
                int taken = 0; for (int j = 0; j < npick; j++) taken |= pick[j] == cur[i].tid; if (taken) continue;
                uint64_t was = 0; for (int j = 0; j < np; j++) if (prev[j].tid == cur[i].tid) { was = prev[j].ticks; break; }
                uint64_t u = cur[i].used > was ? cur[i].used - was : 0;
                if (was && u >= need && u > bu) { bu = u; best = i; }
            }
            if (best < 0) break;
            pick[npick++] = cur[best].tid;
        }
    }
    np = 0; for (int i = 0; i < nc; i++) { prev[np].tid = cur[i].tid; prev[np].ticks = cur[i].used; np++; }
    if (!g_hint || !H.set_threads) return;
    int same = npick == g_hint_n; for (int i = 0; same && i < npick; i++) { int in = 0; for (int j = 0; j < g_hint_n; j++) in |= g_hint_tids[j] == pick[i]; same = in; }
    if (same) return;
    if (H.set_threads(g_hint, pick, (size_t)npick)) { LOG("[perf] ADPF: the session refused threads"); return; }
    memcpy(g_hint_tids, pick, sizeof(int32_t) * (size_t)npick); g_hint_n = npick;
    char l[160]; size_t o = 0; for (int i = 0; i < npick; i++) o += snprintf(l + o, sizeof l - o, " %d", pick[i]);
    LOG("[perf] ADPF session threads:%s", l);
}

// DM_PIN_REPLAY (start-up): the replay thread (DM_NVN_ASYNC, hle_nvn.c replay_main) kept to the fastest cores. prime: the cores of the highest maximum clock
// (a Snapdragon 8+ Gen 1's Cortex-X2), big: every core above the lowest maximum clock. Left to the scheduler, Unicorn Overlord's battle map replay took
// 13 to 20 ms a frame from run to run with the same ~3,500 draws
void dm_perf_replay_thread(void) {
    const char *e = getenv("DM_PIN_REPLAY"); if (!e || !*e || !strcmp(e, "0")) return;
    int prime = !strcmp(e, "prime"); long n = sysconf(_SC_NPROCESSORS_CONF); if (n > 32) n = 32;
    long mhz[32] = {0}, hi = 0, lo = 0;
    for (long c = 0; c < n; c++) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", c); FILE *f = fopen(p, "r");
        if (f) { if (fscanf(f, "%ld", &mhz[c]) != 1) mhz[c] = 0; fclose(f); } mhz[c] /= 1000; if (mhz[c] > hi) hi = mhz[c]; if (mhz[c] && (!lo || mhz[c] < lo)) lo = mhz[c]; }
    if (!hi || hi == lo) { LOG("[perf] DM_PIN_REPLAY: the cores' clocks are unknown or all alike, not pinned"); return; }
    cpu_set_t set; CPU_ZERO(&set); char l[96]; size_t o = 0;
    for (long c = 0; c < n; c++) if (prime ? mhz[c] == hi : mhz[c] > lo) { CPU_SET(c, &set); o += snprintf(l + o, sizeof l - o, " %ld", c); }
    if (sched_setaffinity(0, sizeof set, &set)) { LOG("[perf] DM_PIN_REPLAY: sched_setaffinity refused"); return; }
    LOG("[perf] replay thread on CPUs%s (%s, up to %ld MHz)", l, prime ? "prime" : "big", hi);
}

static int64_t g_work_start;
// ---- adaptive frame rate: when the frames do not fit the period the game asks for, presentation steps to a longer one the display divides evenly
// (60 → 40 → 30 fps; 40 asks the display for 120 Hz), and back when they fit the shorter one with room; a steady rate instead of alternating
// one-vsync and two-vsync frames. DM_ADAPTIVE_FPS=0 keeps the game's own period.
static const int64_t STEPS[] = {16666667, 25000000, 33333333};   // 60, 40, 30 fps
static int g_step = 0, g_adapt = -1; static int64_t g_work_hist[64]; static int g_nwork;
static int64_t g_last_change, g_last_up, g_up_hold = 2000000000; static int g_ok_frames; static uint64_t g_game_period;   // the period the game asks for, before the adaptive step
static int64_t work_pct(int q) {   // the q-th percentile of the last 64 frames' work
    int n = g_nwork < 64 ? g_nwork : 64; if (n < 16) return 0;
    int64_t v[64]; memcpy(v, g_work_hist, sizeof(int64_t) * (size_t)n);
    for (int i = 1; i < n; i++) { int64_t x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    return v[(n * q) / 100];
}
static int64_t work_p95(void) { return work_pct(95); }
uint64_t dm_present_period_adapt(uint64_t period) {
    if (g_adapt < 0) { const char *e = getenv("DM_ADAPTIVE_FPS"); g_adapt = !(e && !atoi(e)); }
    if (!g_adapt || !period) return period;
    g_game_period = period;
    int64_t want = STEPS[g_step];
    return (uint64_t)want > period ? (uint64_t)want : period;   // never faster than the game asks
}
static int fps_pin(void) {   // a DM_CTL "env DM_FPS_PIN=40" line holds that step (60, 40 or 30; anything else lets the rate adapt again): tests
    static unsigned k; static int pin = -1; if (k++ % 30) return pin;
    const char *e = getenv("DM_FPS_PIN"); int v = e ? atoi(e) : 0, st = v == 60 ? 0 : v == 40 ? 1 : v == 30 ? 2 : -1;
    if (st != pin) { pin = st; LOG("[perf] adaptive frame rate %s", st < 0 ? "free again" : "pinned"); }
    return pin;
}
// The frame's replay and GPU time (gfx_vk_present.c, before dm_perf_frame): the rate is judged on the largest of them and the presenting thread's work. With the
// replay on a thread of its own (DM_NVN_ASYNC) Xenoblade Chronicles 3's presenting thread worked ~8 ms a frame, the rate stepped up to 40 fps for frames
// of ~24 ms GPU time, and the display's switch to 120 Hz and the missed frames made 80-550 ms frames
static int64_t g_frame_cost, g_frame_gpu;
void dm_perf_frame_cost(int64_t replay_ns, int64_t gpu_ns) { g_frame_cost = replay_ns > gpu_ns ? replay_ns : gpu_ns; g_frame_gpu = gpu_ns; }
// The frame's work to the CPU hint: with the GPU's time where Android 15 takes it, and a load increase signalled (Android 16) when 3 frames in a row run over
// the period, at most every 0.5 s: the clocks then follow the whole frame and not only its CPU half. (Super Mario Odyssey on the Red Magic Astra fell from 60
// to 30-45 fps with the GPU at 222-443 MHz in the same scene; that device's GPU clock moved only with its FULL performance mode or its game menu, hints or not)
// pq: the wait for the present thread, left out of the CPU time where the GPU's time goes beside it (blocked, not CPU work); pace: the part of it that is a
// longer step's pacing, left out of the work judged over the period (counted, every frame at 40 or 30 fps was over the game's period and signalled)
static void hint_report(int64_t w, int64_t pq, int64_t pace, int64_t now, int64_t target) {
    if (H.report2) {
        AWorkDuration *d = g_wd;
        H.wd_start(d, now - w); H.wd_total(d, w); H.wd_cpu(d, w > pq ? w - pq : 0); H.wd_gpu(d, g_frame_gpu > 0 ? g_frame_gpu : 0); if (H.report2(g_hint, d)) H.report(g_hint, w);
    } else H.report(g_hint, w);
    static int over; static int64_t last;
    int64_t worst = w - pace > g_frame_gpu ? w - pace : g_frame_gpu;
    over = worst > target * 105 / 100 ? over + 1 : 0;
    if (H.increase && over >= 3 && now - last > 500000000ll) {
        last = now; int r = H.increase(g_hint, true, g_frame_gpu > target * 70 / 100, "frame over its period");
        static int n; if (n++ < 5) LOG("[perf] ADPF load increase signalled (work %.1f ms, GPU %.1f ms, target %.1f ms)%s", w / 1e6, g_frame_gpu / 1e6, target / 1e6, r ? " refused" : "");
    }
}
// The frames' time on screen (hle_nvn.c, 8 presents late; ms, -1 = discarded by the system, 0 = no reading): a frame shown longer than 1.5 periods of the step,
// or discarded, missed the display. Work over the period alone does not mean that: the swapchain holds a frame or two, so a frame a little over the period
// still reaches its vsync. Viractal at 60 fps (FPSLocker's unlock) stepped down to 40 on p90 work of 17.5-17.7 ms while it dropped 0-10 of 300 frames on
// screen at 60 with the rate held (DM_ADAPTIVE_FPS=0; work p50 6 ms, GPU 5-11 ms), and the player saw the counter fall into the 30s whenever the pieces moved
static uint8_t g_late[64]; static int g_nshown; static unsigned g_held_back;
// With display times (vk_refresh_ms) the intervals are whole vsyncs, and 1.5 periods sat on a vsync boundary at 60 fps on 120 Hz (25.0 ms against 25.0): the limit
// moves a quarter refresh past it, so the same frames count as with the hand-over times before. A stricter period + half a refresh counted every one-vsync miss:
// Unicorn Overlord's opening then showed 70-157 of 300 frames at 25 ms and stepped to 40 fps, where it had held 60 (about 50-54 fps on screen)
__attribute__((weak)) double vk_refresh_ms(void) { return 0; }
// A frame the game itself asked to hold longer (its present interval) is judged against that hold. Super Mario Bros. Wonder sets interval 2 for the frame after one
// that ran long (its own controller, main+0xcfd24) and 1 again: 133 times in 50 s of its first level, and those 33.3 ms frames counted as late (29-36 of 64) stepped
// the rate down to 40 fps, where the paced frames ran longer still in the game's own measure and it asked for interval 2 more often
__attribute__((weak)) double dm_nvn_present_hold_ms(void) { return 0; }
void dm_perf_present(double iv_ms) {
    if (iv_ms == 0) return;
    double r = vk_refresh_ms(), hold = dm_nvn_present_hold_ms(), per = STEPS[g_step] / 1e6, lim = (hold > per ? hold : per) * 1.5 + r / 4;
    g_late[g_nshown++ % 64] = iv_ms < 0 || iv_ms > lim;
}
static int shown_late(void) {   // of the last 64 frames on screen, how many missed the display; -1 without readings (a device that gives no present times)
    int n = g_nshown < 64 ? g_nshown : 64, m = 0; if (n < 32) return -1;
    for (int i = 0; i < n; i++) m += g_late[i];
    return m * 64 / n;
}
static void adapt_frame(int64_t work, int64_t now) {
    // A long frame in which the replay and the GPU did next to nothing is a load (a loading screen, the game reading files on its main thread), not a frame the
    // rate could fit: it is not judged. Hades II's room changes gave ~10 frames of 28-680 ms with 0.3-0.4 ms of GPU work, the rate stepped down to 40 fps,
    // and each fight began at 40 until it climbed back 4 s later
    if (g_frame_cost > 0 && g_frame_cost < 1500000 && work > STEPS[g_step]) return;
    if (g_frame_cost > work) work = g_frame_cost;
    if (!g_adapt) return;
    g_work_hist[g_nwork++ % 64] = work;
    { static unsigned k; if (++k % 600 == 0) LOG("[perf] adaptive frame rate: %.0f fps, frame work p50 %.1f / p90 %.1f / p95 %.1f ms (last %.1f ms); late on screen %d of 64, step-downs held back %u", 1e9 / STEPS[g_step], work_pct(50) / 1e6, work_pct(90) / 1e6, work_p95() / 1e6, work / 1e6, shown_late(), g_held_back); }
    int pin = fps_pin(); if (pin >= 0) { if (g_step != pin) { g_step = pin; g_last_change = now; g_nshown = 0; } return; }
    // Judged on the 90th percentile of the last 64 frames' work, from 4 s after a change: the GPU and CPU clocks take a few seconds to rise for a shorter
    // period, and with the 95th percentile against 95% of the period NEO's crowd fell back from 40 to 30 fps within 2 s every time, though pinned at
    // 40 it held 24.4 ms frames (p90 work 24.9 ms, p95 frame time 34-36 ms: no worse than its 30 fps p95 of 35-37 ms)
    if (now - g_last_change < 4000000000ll) return;
    int64_t p90 = work_pct(90); if (!p90) return;
    int nsteps = (int)(sizeof STEPS / sizeof *STEPS);
    if (g_step + 1 < nsteps && p90 > STEPS[g_step] * 105 / 100) {
        // only when the frames miss the display too: at least 1 in 10 of the last 64 on screen (as the 90th percentile), or no readings to tell
        int late = shown_late(); if (late >= 0 && late < 7) g_held_back++;   // veto only the downshift: the chosen upshift percentile may still fit
        else {
            // back down soon after a step up: the next try waits twice as long (up to a minute), so the rate does not swing between two steps
            if (g_last_up && now - g_last_up < 10000000000ll) { g_up_hold = g_up_hold * 2 > 60000000000ll ? 60000000000ll : g_up_hold * 2; }
            g_step++; g_last_change = now; g_nwork = 0; g_nshown = 0; g_ok_frames = 0; LOG("[perf] adaptive frame rate: %.0f fps (frames p90 %.1f ms, late on screen %d of 64)", 1e9 / STEPS[g_step], p90 / 1e6, late); return; }
    }
    // Back up when the frames would fit the shorter period even at the clocks they get now: those clocks drop with the slack of the longer one (Viractal's board
    // took 14-16 ms a frame at 40 fps, p90 15.6, and 10-11 ms with 0 frames late at 60). A step up that does not fit shows on screen and steps back down (above)
    // DM_ADAPT_UP_PCT (start-up, default 90): the percentile of frame work judged for a step up. At the longer period the clocks drop and the work grows:
    // Unicorn Overlord's opening held 60 fps with 1-2 of 64 frames late, but once a load stepped it down to 40 its p90 stayed at 21-25 ms and it never went back
    // up (p50 14-15 ms). With 50 it tries the shorter period again; a try that does not fit misses the display and steps back down, and the next waits twice as long
    static int up_pct = -1; if (up_pct < 0) { const char *e = getenv("DM_ADAPT_UP_PCT"); up_pct = e && atoi(e) > 0 && atoi(e) < 100 ? atoi(e) : 90; }
    if (g_step > 0 && (up_pct == 90 ? p90 : work_pct(up_pct)) < STEPS[g_step - 1]) { if (++g_ok_frames > 120 && now - g_last_change >= g_up_hold) { g_step--; g_last_change = g_last_up = now; g_nwork = 0; g_nshown = 0; g_ok_frames = 0; LOG("[perf] adaptive frame rate: %.0f fps (frames p90 %.1f ms)", 1e9 / STEPS[g_step], p90 / 1e6); } }
    else g_ok_frames = 0;
}
// hle_nvn.c, the first acquire after a present, right after its pacing wait. A frame's work is the time between two presents less that wait: what the
// render thread waited for besides the pacing (the game's main thread, its own syncs) is the frame's work too, or the hint would never ask for
// the clock a slow main thread needs
// pq_ns: the wait for the present thread (hle_nvn.c), left out of the work the adaptive rate judges (at 40 or 30 fps it is the longer period's pace, and counted
// it held the rate down for good) but kept in the work the CPU hint gets (with the replay on its own thread it is the replay's and the GPU's time as well:
// left out, Super Mario Odyssey's hint saw light frames and the clocks stayed at 1-2 GHz). At 60 fps it is not a pace but the display or the swapchain
// holding the frames back, and it stays in the work, or the rate could not step down for it
static int64_t g_prev_present, g_pace, g_pq;
void dm_perf_frame_start(uint64_t pace_ns, uint64_t pq_ns) { g_work_start = perf_now(); g_pace = (int64_t)pace_ns; g_pq = (int64_t)pq_ns; }
// Called on the presenting thread right before a present; period in seconds (1/60, 1/30, ...).
void dm_perf_frame(double period) {
    int64_t p = period > 0 && period < 1 ? (int64_t)(period * 1e9) : 16666667, t = perf_now();
    // the CPU hint's target is the game's own period, not the adaptive one: with the longer target the CPU clocks drop until the work fills it,
    // and the work then never shows the room to step back up (NEO stayed at 30 fps with frames of 31 to 48 ms)
    if (g_game_period && (int64_t)g_game_period < p) p = (int64_t)g_game_period;
    hint_init(p);
    if (g_work_start) {   // (a longer span: the game was held, by the pause menu or the background)
        int64_t w = g_prev_present ? t - g_prev_present - g_pace : t - g_work_start;
        int64_t pq = g_prev_present ? g_pq : 0, pace = g_step > 0 ? pq : 0;   // (t - g_work_start starts after the wait already)
        if (w > 0 && w < 250000000) { adapt_frame(w - pace > 0 ? w - pace : 0, t); if (g_hint) hint_report(w, pq, pace, t, g_hint_target); }
        g_work_start = 0;
    }
    g_prev_present = t;
    static unsigned frames; if (frames++ % 300 == 0) hint_threads_scan((int32_t)gettid());
    if (!g_hint) return;
    if (p != g_hint_target && H.update) { H.update(g_hint, p); g_hint_target = p; }
}

// The window's frame rate: the game's rate, so the display can pick a mode it divides (60 / 120 Hz rather than 144 Hz for a 60 fps game).
void dm_perf_window(ANativeWindow *w, double fps) {
    if (!w || fps <= 0) return;
    int r = ANativeWindow_setFrameRate(w, (float)fps, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
    LOG("[perf] window frame rate %.0f fps%s", fps, r ? " refused" : "");
}

