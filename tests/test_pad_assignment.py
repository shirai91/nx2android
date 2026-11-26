import pathlib
import subprocess
import tempfile
import unittest


class PadAssignmentTest(unittest.TestCase):
    def test_move_swap_capacity_and_removal(self):
        root = pathlib.Path(__file__).resolve().parents[1]
        code = r'''#include <assert.h>
#include "pad_assignment.h"
int main(void) {
    pad_assignment a = {0}; int p, c;
    assert(pa_move(&a, 1, 3, -1)); /* phone -> player 4 */
    assert(pa_move(&a, 2, 0, -1)); assert(pa_move(&a, 3, 1, -1));
    assert(pa_move(&a, 4, 2, -1));
    assert(pa_move(&a, 4, 1, 0)); /* swap player 3 and 2 */
    assert(a.source[1][0] == 4 && a.source[2][0] == 3);
    assert(pa_move(&a, 1, 1, -1)); /* two controllers for player 2 */
    assert(!pa_move(&a, 5, 1, -1));
    assert(pa_move(&a, 5, 1, 0)); assert(!pa_find(&a, 4, &p, &c));
    assert(pa_move(&a, 4, 1, 0)); /* restore from pool, displacing 5 */
    assert(pa_find(&a, 1, &p, &c) && p == 1 && c == 1);
    assert(pa_move(&a, 1, -1, -1)); assert(!pa_find(&a, 1, &p, &c));
    assert(!pa_move(&a, 0, 0, 0)); assert(!pa_move(&a, 1, 4, 0));
    for (int n = 2; n <= 4; n++) assert(pa_move(&a, n, -1, -1));
    for (int i = 0; i < PA_PLAYERS; i++) for (int j = 0; j < PA_CELLS; j++) assert(!a.source[i][j]);
    pad_assignment_input left = {.buttons=1, .moved={1,3}, .axis={100,200,0,300}};
    pad_assignment_input right = {.buttons=2, .moved={2,4}, .axis={-400,0,0,0}};
    pad_assignment_input both = pa_combine(&left, &right);
    assert(both.buttons == 3 && both.axis[0] == -400 && both.axis[1] == 0 && both.axis[3] == 300);
    right.axis[0] = 0; both = pa_combine(&left, &right);
    assert(both.axis[0] == 100 && both.axis[1] == 200); /* released stick restores the held one */
    pad_assignment_input s = {0}; uint64_t clock = 0;
    int32_t held[] = {100,0,0,0}, idle[] = {0,0,0,0};
    pa_axes(&s, held, &clock); assert(s.moved[0] == 1 && clock == 1);
    pa_axes(&s, held, &clock); assert(clock == 1); /* idle reports do not steal the stick */
    pa_axes(&s, idle, &clock); assert(clock == 1 && !s.axis[0]);
    pa_axes(&s, held, &clock); assert(s.moved[0] == 2);
    pad_assignment_runtime r; pa_runtime_init(&r);
    assert(r.connected[0] && !pa_runtime_move(&r, 2, 0, -1));
    assert(pa_runtime_move(&r, 1, 3, -1)); pa_runtime_attach(&r, 2);
    assert(pa_runtime_move(&r, 2, 0, -1)); r.input[1].buttons = 1;
    pa_runtime_detach(&r, 2); assert(!r.input[1].buttons && !pa_find(&r.slots, 2, &p, &c));
    pa_runtime_attach(&r, 2); assert(pa_find(&r.slots, 2, &p, &c) && p == 0);
    pa_runtime_detach(&r, 2); pa_runtime_attach(&r, 3); assert(pa_runtime_move(&r, 3, 0, 0));
    pa_runtime_attach(&r, 2); assert(!pa_find(&r.slots, 2, &p, &c)); /* no eviction on reconnect */
    pad_assignment_runtime auto_r; pa_runtime_init(&auto_r);
    assert(pa_runtime_move(&auto_r, 1, 0, -1));
    pa_runtime_attach_new(&auto_r, 2, 0);
    assert(pa_find(&auto_r.slots, 2, &p, &c) && p == 1); /* first entirely empty player */
    assert(pa_runtime_move(&auto_r, 2, -1, -1));
    pa_runtime_detach(&auto_r, 2); pa_runtime_attach_new(&auto_r, 2, 1);
    assert(!pa_find(&auto_r.slots, 2, &p, &c)); /* explicit removal survives reconnect */
    pa_runtime_attach_new(&auto_r, 3, 0); assert(pa_find(&auto_r.slots, 3, &p, &c) && p == 1);
    pa_runtime_attach_new(&auto_r, 4, 0); pa_runtime_attach_new(&auto_r, 5, 0);
    pa_runtime_attach_new(&auto_r, 6, 0); assert(!pa_find(&auto_r.slots, 6, &p, &c)); /* full: stays in pool */
    unsigned random = 7;
    for (int step = 0; step < 10000; step++) {
        random = random * 1664525u + 1013904223u; int source = 1 + (random % 12);
        random = random * 1664525u + 1013904223u; int player = (int)(random % 5) - 1;
        random = random * 1664525u + 1013904223u; int cell = (int)(random % 3) - 1;
        pa_move(&a, source, player, cell);
        for (int i = 0; i < PA_PLAYERS * PA_CELLS; i++) for (int j = i + 1; j < PA_PLAYERS * PA_CELLS; j++) {
            int x = a.source[i / PA_CELLS][i % PA_CELLS], y = a.source[j / PA_CELLS][j % PA_CELLS];
            assert(!x || x != y);
        }
    }
}'''
        with tempfile.TemporaryDirectory() as tmp:
            c = pathlib.Path(tmp) / 'check.c'; c.write_text(code)
            exe = pathlib.Path(tmp) / 'check'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'nxnative/dm/android'), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__': unittest.main()
