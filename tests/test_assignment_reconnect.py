import pathlib
import shutil
import subprocess
import tempfile
import unittest


class AssignmentReconnect(unittest.TestCase):
    def test_reconnect_before_watcher_and_identical_live_devices(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the controller check')
        root = pathlib.Path(__file__).resolve().parents[1]
        front = (root / 'nxnative/dm/android/front_android.c').read_text(encoding='utf-8')
        actual = front[front.index('static int assignment_device('):front.index('static void assignment_axes(')]
        harness = r'''#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "pad_assignment.h"
static pad_assignment_runtime g_assign;
static struct { int id, present; char key[64]; } g_assign_devices[PA_SOURCES];
static uint64_t g_assign_keys[PA_SOURCES], g_assign_axis_buttons[PA_SOURCES];
static _Atomic int g_assign_active;
static int gone;
static int pad_dev_kind(int id) { return id == gone ? -1 : 1; }
static void assignment_save(void) {}
''' + actual + r'''
int main(void) {
    pa_runtime_init(&g_assign); atomic_store(&g_assign_active, 1);
    int a = assignment_attach(10, "identical");
    assert(a == 2 && g_assign.slots.source[0][0] == a);
    int b = assignment_attach(11, "identical");
    assert(b != a && g_assign.slots.source[1][0] == b);
    g_assign.input[a-1].buttons = 7; g_assign_keys[a-1] = 7;
    gone = 10;
    int reconnected = assignment_attach(12, "identical");
    assert(reconnected == a && g_assign.slots.source[0][0] == a);
    assert(!assignment_device(10) && assignment_device(12) == a);
    assert(!g_assign.input[a-1].buttons && !g_assign_keys[a-1]);
    assert(g_assign.slots.source[1][0] == b);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory); source = tmp / 'reconnect.c'; exe = tmp / 'reconnect'
            source.write_text(harness, encoding='utf-8')
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                            '-I', str(root / 'nxnative/dm/android'), str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
