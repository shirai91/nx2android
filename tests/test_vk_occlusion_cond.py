"""Render conditions on a samples-passed report (gfx_vk_exec_render_cond.inc): a counted report's answer becomes known once the submission that recorded it is
done, a report written as a constant stays unknown, and a timestamp report reads its known 0 until then. With DM_VK_OCCLUSION=0 the rule of the builds before
the counter applies. No Vulkan and no guest: the fence and the report memory are stood in for, so the table's decisions are checked without a phone."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402
import host_cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(host_cc.posix(), 'a POSIX host C compiler is required')
class OcclusionRenderCond(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = csrc.unit((ROOT / 'nxnative/dm/android/gfx_vk_exec.c'))
        begin = source.index('static int g_rc_skip;')
        end = source.index('static void exec_begin(', begin)
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "''' + str(ROOT / 'nxnative/dm/gfx/nvn/nvn_hmap.h') + r'''"
#define VKLOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
static uint64_t g_last_fence, g_done;
static uint64_t mtl_last_fence(void) { return g_last_fence; }
uint64_t vk_fence_done(void) { return g_done; }
static int g_x_tracepass;
static int g_oq = 1;
static int vk_oq_enabled(void) { return g_oq; }
typedef struct { uint32_t mode; uint64_t addr; } p_render_cond;
static uint64_t mem[8];
''' + source[begin:end] + r'''
static uint64_t addr(int i) { return (uint64_t)(uintptr_t)&mem[i]; }
static int known(uint64_t a, uint64_t *v) { return rc_value(a, v); }
int main(void) {
    uint64_t v = 0;
    // no report at an address: memory is the answer
    mem[0] = 7;
    assert(known(addr(0), &v) && v == 7);

    // a counted samples-passed report (kind 2): unknown while its submission is recorded or on the GPU, memory once it is done
    // (the submission being recorded commits with mtl_last_fence() + 1)
    g_last_fence = 10; g_done = 10; mem[1] = 0;
    rc_note(addr(1), 2);
    assert(!known(addr(1), &v));
    g_done = 11;
    assert(known(addr(1), &v) && v == 0);
    mem[1] = 4242;
    assert(known(addr(1), &v) && v == 4242);

    // a timestamp report (kind 1): its 0 is known before the submission is done, memory after
    g_last_fence = 11; g_done = 11; mem[2] = 999;
    rc_note(addr(2), 1);
    assert(known(addr(2), &v) && v == 0);
    g_done = 12;
    assert(known(addr(2), &v) && v == 999);

    // a report that measured nothing (kind 3): unknown for good, even once its submission is done
    g_last_fence = 12; g_done = 13; mem[3] = 1u << 20;
    rc_note(addr(3), 3);
    assert(!known(addr(3), &v));
    g_done = 100;
    assert(!known(addr(3), &v));

    // the latest report at an address wins: a count then a constant is unknown again
    g_last_fence = 13; g_done = 14; mem[4] = 5;
    rc_note(addr(4), 2);
    assert(known(addr(4), &v) && v == 5);
    rc_note(addr(4), 3);
    assert(!known(addr(4), &v));

    // a condition on two counted reports decides once both are done. mode 0 draws while the two are equal, mode 1 while they differ.
    // The condition reads addr and addr + 16: two report records, 16 bytes apart, not two fields of one
    g_last_fence = 20; g_done = 21; mem[5] = 0; mem[7] = 0;
    rc_note(addr(5), 2); rc_note(addr(7), 2);
    p_render_cond rc = {0, addr(5)};
    render_cond(&rc);
    assert(g_rc_skip == 0);
    mem[7] = 3;
    render_cond(&rc);
    assert(g_rc_skip == 1);
    rc.mode = 1;
    render_cond(&rc);
    assert(g_rc_skip == 0);
    mem[7] = 0;
    render_cond(&rc);
    assert(g_rc_skip == 1);
    // while one of them is still in flight the condition is unknown and draws
    g_last_fence = 21; g_done = 21;
    rc_note(addr(7), 2);
    rc.mode = 0;
    render_cond(&rc);
    assert(g_rc_skip == 0);
    // SetRenderEnable(true/false) overrides
    rc.mode = 3; render_cond(&rc); assert(g_rc_skip == 1);
    rc.mode = 2; render_cond(&rc); assert(g_rc_skip == 0);

    // counting off (DM_VK_OCCLUSION=0): the rule of the builds before the counter. A report that wrote no timestamp is unknown for good, even when a
    // timestamp report comes later at the same address (the counting table above would decide again there), and a timestamp reads 0 inside its own submission only
    g_oq = 0;
    g_last_fence = 30; g_done = 31; mem[6] = 77;
    rc_note(addr(6), 3);
    assert(!known(addr(6), &v));
    g_last_fence = 31; g_done = 32;
    assert(!known(addr(6), &v));          // still unknown after its submission is done
    rc_note(addr(6), 1);                  // a timestamp at the same address: the old rule keeps it unknown
    assert(!known(addr(6), &v));
    rc_note(addr(0), 1);                  // this submission's timestamp: its 0 is known
    assert(known(addr(0), &v) && v == 0);
    g_last_fence = 32; g_done = 33;       // a new submission: an earlier timestamp reads memory
    assert(known(addr(0), &v) && v == 7);
    printf("ok\n");
    return 0;
}
'''
        cls.temp = tempfile.TemporaryDirectory(prefix='nx2a-oq-cond-')
        cls.addClassCleanup(cls.temp.cleanup)
        src = Path(cls.temp.name) / 'test.c'
        src.write_text(code, encoding='utf-8')
        cls.exe = src.with_suffix('')
        built = subprocess.run(['cc', '-std=c11', '-O1', '-Wall', str(src), '-o', str(cls.exe)],
                               capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def test_conditions_on_counted_reports(self):
        r = subprocess.run([str(self.exe)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()