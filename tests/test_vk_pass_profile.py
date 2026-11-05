"""Exercise completed-frame GPU pass accounting without a Vulkan device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_core.c'


class PassProfile(unittest.TestCase):
    def test_completed_frames_and_live_threshold(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the GPU pass accounting check')
        src = SOURCE.read_text(encoding='utf-8')
        meta = next(line for line in src.splitlines() if line.startswith('typedef struct { uint32_t w, h, fmt, draws, fidx; }'))
        options = src[src.index('static int g_fpass;'):src.index('static pass_meta g_pmeta')]
        report_start = src.index('void vk_pass_report(void)')
        report = src[report_start:src.index('\n}\n', report_start) + 3]
        accounting = src[src.index('static uint64_t g_sf_frame'):src.index('static void *completion_main')]
        capacity = next(line for line in src.splitlines() if line.startswith('enum { PASSQ ='))
        query_start = src.index('        if (g_pq && pass_times_on()) {   // this submission')
        query = src[query_start:src.index('        uint64_t now = gpu_ticks()', query_start)]
        harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { PT_DISPATCH = UINT32_MAX };
static int g_pq = 1;
static char logs[32][3000]; static unsigned nlog;
static void log_line(const char *fmt, ...) {
    assert(nlog < 32); va_list ap; va_start(ap, fmt);
    vsnprintf(logs[nlog++], sizeof logs[0], fmt, ap); va_end(ap);
}
#define VKLOG log_line
uint64_t g_nvn_discards[2];
static int pass_times_on(void) { return g_pq; }
static double gpu_span = 12;
static double mtl_gpu_ms(uint64_t frame) { (void)frame; return gpu_span; }
''' + capacity + '\n' + meta + '\n' + options + report + accounting + r'''
enum { VK_SUCCESS = 0, VK_QUERY_RESULT_64_BIT = 1 };
static struct { int dev; struct { struct { double timestampPeriod; } limits; } props; } V = {.props.limits.timestampPeriod = 1000000};
typedef struct { uint64_t frame; int slot, npass; pass_meta pmeta[PASSQ]; } test_submission;
static unsigned queried;
static int vkGetQueryPoolResults(int dev, int pool, uint32_t first, uint32_t count, size_t size, void *out, size_t stride, int flags) {
    (void)dev; (void)pool; (void)first; (void)flags; queried++;
    assert(count <= PASSQ * 2 && size >= count * sizeof(uint64_t) && stride == sizeof(uint64_t));
    for (uint32_t i = 0; i < count; i++) ((uint64_t *)out)[i] = i;
    return VK_SUCCESS;
}
static void submission(test_submission *c) {
''' + query + r'''
}
static const char *last(void) { assert(nlog); return logs[nlog - 1]; }
static void threshold(const char *s) { assert(setenv("DM_VK_PASS_SPIKE", s, 1) == 0); vk_pass_frame(); }
int main(void) {
    threshold("1000"); assert(atomic_load(&g_pass_spike_ms) == 1000);
    pass_spike_flush(); assert(nlog == 0 && g_sf_frames == 0);
    pass_meta a = {10, 20, 37, 2 | (10u << 16), 0};
    g_sf_frame = 1; pass_spike_add(4, &a); a.fidx = 1; pass_spike_add(2, &a);
    pass_spike_flush(); assert(g_sf_frames == 1 && g_sf_acc == 6 && nlog == 0);

    // Ordinal 0 has another meaning in the next frame. Its metadata and cost must stay in that frame.
    pass_meta b = {100, 200, 122, 17 | (50u << 16), 0};
    g_sf_frame = 2; pass_spike_add(8, &b);
    vk_pass_report(); assert(atomic_load(&g_pass_report) == 1 && g_sf_frames == 1 && g_sf_acc == 6);
    pass_spike_flush();
    assert(strstr(last(), "7.00 ms/frame") && strstr(last(), "2 complete frames, 0 incomplete"));
    assert(strstr(last(), "frame 2, 8.00 ms in 1 timed passes") && strstr(last(), "#0 8.00 ms (100x200 fmt 122"));
    assert(!strstr(last(), "10x20") && !strstr(last(), "outside"));
    assert(g_sf_frames == 0 && g_sf_acc == 0 && atomic_load(&g_pass_report) == 0);

    // Missing or invalid timestamp data is excluded from the mean, rather than counted as a short frame.
    g_sf_frame = 3; pass_spike_add(40, &a); g_sf_bad = 1; pass_spike_flush();
    assert(g_sf_frames == 0 && g_sf_incomplete == 1 && g_sf_acc == 0);
    g_sf_frame = 4; b.fidx = 601; pass_spike_add(6, &b); vk_pass_report(); pass_spike_flush();
    assert(strstr(last(), "6.00 ms/frame") && strstr(last(), "1 complete frames, 1 incomplete"));
    assert(strstr(last(), "#601 6.00 ms"));
    g_sf_frame = 5; g_sf_bad = 1; vk_pass_report(); pass_spike_flush();
    assert(strstr(last(), "no complete frames (1 incomplete)") && strstr(last(), "incomplete timestamps"));

    // Changing the option between frames affects the next completed frame without a restart.
    threshold("1"); assert(atomic_load(&g_pass_spike_ms) == 1);
    g_sf_frame = 6; pass_meta dispatch = {40, 18, PT_DISPATCH, 1, 0};
    pass_spike_add(2, &dispatch); pass_spike_flush();
    assert(strstr(last(), "GPU spike: frame 6") && strstr(last(), "dispatch 40x18x1"));
    threshold("1000"); unsigned before = nlog;
    g_sf_frame = 7; pass_spike_add(2, &dispatch); pass_spike_flush(); assert(nlog == before);
    threshold("nan"); assert(atomic_load(&g_pass_spike_ms) == 25);
    threshold("0"); assert(atomic_load(&g_pass_spike_ms) == 25);
    threshold("-1"); assert(atomic_load(&g_pass_spike_ms) == 25);

    // Retaining the top 20 must still account for every pass, including later and zero-duration ones.
    threshold("1000"); g_sf_frame = 8;
    for (unsigned i = 0; i < 30; i++) { b.fidx = i; pass_spike_add(i + 1, &b); }
    pass_spike_add(0, &b);
    assert(g_sf_tot == 465 && g_sf_passes == 31 && g_nsf == 20);
    assert(g_sf[0].ms == 30 && g_sf[19].ms == 11);
    vk_pass_report(); vk_pass_report(); pass_spike_flush();
    assert(strstr(last(), "frame 8, 465.00 ms in 31 timed passes"));
    assert(strstr(last(), "#29 30.00 ms") && strstr(last(), "#10 11.00 ms") && !strstr(last(), "#9 10.00 ms"));
    assert(atomic_load(&g_pass_report) == 0 && g_nsf == 0 && g_sf_passes == 0);

    // Use the real completion block: reaching query capacity cannot establish a complete frame.
    test_submission c = {.frame = 100, .slot = 0, .npass = PASSQ - 1};
    for (int i = 0; i < PASSQ; i++) { c.pmeta[i] = b; c.pmeta[i].fidx = i; }
    g_sf_frame = c.frame; submission(&c);
    assert(!g_sf_bad && g_sf_passes == PASSQ - 1 && queried == 1);
    pass_spike_flush(); assert(g_sf_frames == 1 && g_sf_acc == PASSQ - 1);
    c.frame++; c.npass = PASSQ; g_sf_frame = c.frame; submission(&c);
    assert(g_sf_bad && g_sf_passes == PASSQ && queried == 2);
    vk_pass_report(); pass_spike_flush();
    assert(strstr(last(), "511.00 ms/frame") && strstr(last(), "1 complete frames, 1 incomplete"));
    assert(strstr(last(), "frame 101 (incomplete timestamps)") && strstr(last(), "512 timed passes"));
    c.frame++; c.slot = -1; c.npass = 0; g_sf_frame = c.frame; submission(&c);
    assert(g_sf_bad && queried == 2); pass_spike_flush();

    g_pq = 0; before = nlog; threshold("7"); vk_pass_report();
    assert(nlog == before && atomic_load(&g_pass_report) == 0 && atomic_load(&g_pass_spike_ms) == 1000);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_pass_profile_') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
