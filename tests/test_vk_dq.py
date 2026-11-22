"""The two-stage replay's queue (nxnative/dm/android/gfx_vk_dq.c, DM_VK_MT_REPLAY): the real source compiled on the host with stand-in Vulkan entry points
and driven by two producer threads (as the replay thread and the present thread), against a recorder made slow at times. Every queued call must be made once,
in each producer's order, and every call that needs a drain (vkEndCommandBuffer, vkQueuePresentKHR) must find all the calls queued before it made. Long
phases with the recorder held fill the 32 MB ring, so the wrap and the full-ring wait run too. Each waiting mode of the queue runs once."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

HEADERS = csrc.REPO / 'nxnative/dm/third_party/vulkan-headers/include'


@unittest.skipUnless(sys.platform.startswith('linux') and (shutil.which('cc') or shutil.which('clang')), 'Linux and a C compiler (pthreads, spinlocks)')
class ReplayQueue(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = csrc.read('nxnative/dm/android/gfx_vk_dq.c')
        cmds = re.search(r'#define DQ_CMDS\(X\) \\\n(.*?)\n#define DQ_SYNC', src, re.S)
        sync = re.search(r'#define DQ_SYNC\(X\) ([^\n]*)', src)
        if not cmds or not sync: raise csrc.CutError('gfx_vk_dq.c: DQ_CMDS / DQ_SYNC not found; update this test')
        cls.temp = tempfile.TemporaryDirectory(prefix='nx2a-vk-dq-'); cls.addClassCleanup(cls.temp.cleanup)
        d = Path(cls.temp.name)
        # the stand-in gfx_vk.h: the device's entry points as the variables gfx_vk.h declares, the log and die
        (d / 'gfx_vk.h').write_text(r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define VKLOG(...) do { fprintf(stderr, "[vk] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(...) do { fprintf(stderr, "FATAL " __VA_ARGS__); abort(); } while (0)
#define STUB_CMDS(X) \
''' + cmds.group(1) + '\n#define STUB_SYNC(X) ' + sync.group(1) + r'''
#define STUB_DECL(f) PFN_##f f;
STUB_CMDS(STUB_DECL) STUB_SYNC(STUB_DECL)
''', encoding='utf-8')
        (d / 'gfx_vk_dq.c').write_text(src, encoding='utf-8')
        harness = r'''
#define _GNU_SOURCE
#include "gfx_vk_dq.c"
#include <assert.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
// a call's arguments carry (producer, sequence): the recorder's stand-ins check each producer's order and count what they made
enum { NPROD = 2 };
static uint64_t made[NPROD], vbmade;            // the recorder's counts (only it writes them)
static _Atomic uint64_t queued[NPROD];          // what each producer queued so far
static _Atomic int hold;                        // the recorder is held (a slow driver call)
static _Atomic uint64_t bad;
static void check(uint32_t prod, uint64_t seq) {
    if (prod >= NPROD || seq != made[prod]) { fprintf(stderr, "out of order: producer %u seq %llu, expected %llu\n", prod, (unsigned long long)seq, (unsigned long long)(prod < NPROD ? made[prod] : 0)); bad++; }
    made[prod < NPROD ? prod : 0]++;
    while (atomic_load(&hold)) usleep(1000);
}
static VKAPI_ATTR void VKAPI_CALL s_draw(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t e) { (void)cb; (void)e; check(a, (uint64_t)b << 32 | c); }
static VKAPI_ATTR void VKAPI_CALL s_vb(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    (void)cb; (void)b; uint32_t prod = first >> 24;   // the offsets hold the sequence, the last one its complement: the copy of both arrays checked whole
    if (!n || o[n - 1] != ~o[0]) { fprintf(stderr, "vertex buffer arrays damaged (n %u)\n", n); bad++; return; }
    check(prod, o[0]); vbmade++;
}
// calls that drain: everything a producer queued before must have been made
static VKAPI_ATTR VkResult VKAPI_CALL s_end(VkCommandBuffer cb) {
    uint32_t prod = (uint32_t)(uintptr_t)cb; uint64_t q = atomic_load(&queued[prod]);
    if (made[prod] < q) { fprintf(stderr, "drain: producer %u had %llu queued, %llu made\n", prod, (unsigned long long)q, (unsigned long long)made[prod]); bad++; }
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL s_present(VkQueue q, const VkPresentInfoKHR *pi) { (void)pi; return s_end((VkCommandBuffer)q); }
static void put_call(uint32_t prod, uint64_t seq) {   // the next call of a producer: a draw or a vertex-buffer bind with 1-40 buffers
    VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)prod;
    if (seq % 7 == 3) {
        uint32_t n = 2 + (uint32_t)(seq % 39); VkBuffer b[40] = {0}; VkDeviceSize o[40];
        for (uint32_t i = 0; i < n; i++) o[i] = seq;
        o[n - 1] = ~seq;
        atomic_fetch_add(&queued[prod], 1);
        vkCmdBindVertexBuffers(cb, prod << 24, n, b, o);
    } else { atomic_fetch_add(&queued[prod], 1); vkCmdDraw(cb, prod, (uint32_t)(seq >> 32), (uint32_t)seq, 0); }
}
static uint64_t N0, N1;
static void *release_main(void *a) { (void)a; usleep(200000); atomic_store(&hold, 0); return NULL; }
static void *present_main(void *a) {   // the present thread: a few calls and a present every so often
    (void)a;
    for (uint64_t i = 0; i < N1; i++) { put_call(1, i); if (i % 50 == 49) { vkQueuePresentKHR((VkQueue)(uintptr_t)1, NULL); usleep(200); } }
    vkQueuePresentKHR((VkQueue)(uintptr_t)1, NULL);
    return NULL;
}
int main(int argc, char **argv) {
    (void)argc; N0 = strtoull(argv[1], NULL, 10); N1 = N0 / 20;
    setenv("DM_VK_MT_REPLAY", "1", 1);
    vkCmdDraw = s_draw; vkCmdBindVertexBuffers = s_vb; vkEndCommandBuffer = s_end; vkQueuePresentKHR = s_present;
    vk_dq_init();
    vk_dq_report();   /* the first call only notes the time */
    assert(vkCmdDraw != s_draw && vkEndCommandBuffer != s_end);   /* the wrappers are in */
    pthread_t pt; pthread_create(&pt, NULL, present_main, NULL);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint64_t i = 0; i < N0; i++) {
        put_call(0, i);
        if (i % 128 == 127) vkEndCommandBuffer((VkCommandBuffer)(uintptr_t)0);   // a cut
        if (i == N0 / 3 || i == 2 * N0 / 3) {   // a long-held recorder (released by another thread after 200 ms): the ring fills (32 MB) and the producer waits for room
            atomic_store(&hold, 1); pthread_t rt; pthread_create(&rt, NULL, release_main, NULL);
            for (uint64_t j = 0; j < 800000; j++) put_call(0, ++i);
            pthread_join(rt, NULL);
        }
        if (i % 20000 == 0) usleep(300);   // gaps, as between frames: the recorder goes idle and sleeps
    }
    vkEndCommandBuffer((VkCommandBuffer)(uintptr_t)0);
    pthread_join(pt, NULL);
    vk_dq_report();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("made %llu + %llu (%llu vertex binds), %llu drains, %.2f s\n", (unsigned long long)made[0], (unsigned long long)made[1], (unsigned long long)vbmade,
           (unsigned long long)g_dq_drains, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    if (made[0] != atomic_load(&queued[0]) || made[1] != atomic_load(&queued[1])) { fprintf(stderr, "lost calls\n"); return 2; }
    return bad ? 1 : 0;
}
'''
        c = d / 'test.c'; c.write_text(harness, encoding='utf-8'); cls.exe = d / 'test'
        cc = shutil.which('cc') or shutil.which('clang')
        r = subprocess.run([cc, '-std=gnu11', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function', '-I', str(d), '-I', str(HEADERS),
                            str(c), '-o', str(cls.exe)], capture_output=True, text=True)
        if r.returncode: raise AssertionError(r.stderr)

    def run_mode(self, env):
        e = dict(os.environ); e.pop('DM_VK_DQ_SPIN', None); e.update(env)
        r = subprocess.run([str(self.exe), '3000000'], capture_output=True, text=True, timeout=120, env=e)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr[-4000:])
        self.assertIn('made ', r.stdout)
        self.assertIn('replay queue per frame', r.stderr)
        return r.stdout

    def test_every_call_made_in_order_sleeping(self):
        self.run_mode({})

    def test_every_call_made_in_order_spinning(self):
        self.run_mode({'DM_VK_DQ_SPIN': '1'})


if __name__ == '__main__':
    unittest.main()
