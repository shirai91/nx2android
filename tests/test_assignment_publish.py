import pathlib
import shutil
import subprocess
import tempfile
import unittest


class AssignmentPublish(unittest.TestCase):
    def test_exact_mask_merges_keyboard_shake_and_two_controllers(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the controller check')
        root = pathlib.Path(__file__).resolve().parents[1]
        front = (root / 'nxnative/dm/android/front_android.c').read_text(encoding='utf-8')
        actual = front[front.index('static void assignment_rumble_check(void) {'):front.index('static int assignment_device(')]
        harness = r'''#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include "pad_assignment.h"
static pad_assignment_runtime g_assign;
static _Atomic int g_menu;
static struct { int id, present; } g_assign_devices[PA_SOURCES];
static int g_assign_rumble = -1, rumble = -1;
static int one_player;
static int dm_player_seen_as(int p) { return one_player ? 0 : p; }
static void dm_haptics_android_device(int id) { rumble = id; }
static uint64_t g_btn[8], g_shake_btn;
static float g_ax[8][8];
static uint32_t mask;
static uint64_t buttons[8];
static int32_t axes[8][4];
static int32_t axis(float v) { return (int32_t)(v * 1000); }
static uint64_t pad_axis_buttons(const float a[8]) { return a[4] > .5f ? 16 : 0; }
static void dm_pad_players_exact(uint32_t m) { mask = m; }
static void dm_input_set(uint64_t b, int32_t x, int32_t y, int32_t z, int32_t w) { assert(!b && !x && !y && !z && !w); }
static void dm_pad_set_player(int p, uint64_t b, int32_t x, int32_t y, int32_t z, int32_t w) {
    buttons[p] = b; axes[p][0] = x; axes[p][1] = y; axes[p][2] = z; axes[p][3] = w;
}
''' + actual + r'''
int main(void) {
    pa_runtime_init(&g_assign); pa_runtime_attach(&g_assign, 2);
    assert(pa_runtime_move(&g_assign, 1, 0, 0)); assert(pa_runtime_move(&g_assign, 2, 0, 1));
    g_assign.input[0].buttons = 1; g_assign.input[1].buttons = 2;
    g_assign.input[0].axis[0] = 200; g_assign.input[0].moved[0] = 1;
    g_btn[0] = 4; g_shake_btn = 8; g_ax[0][4] = 1;
    g_assign_devices[1].id = 42; g_assign_devices[1].present = 1;
    assignment_rumble_use(2, 42); assert(rumble == 42);
    assignment_publish(); assert(mask == 1 && buttons[0] == 31 && axes[0][0] == 200);
    atomic_store(&g_menu, 1); assignment_publish(); assert(mask == 1 && !buttons[0] && !axes[0][0]);
    atomic_store(&g_menu, 0); assert(pa_runtime_move(&g_assign, 1, -1, -1));
    assert(pa_runtime_move(&g_assign, 2, -1, -1)); assignment_publish();
    assert(mask == 0 && !buttons[0] && rumble == -1);
    one_player = 1; assert(pa_runtime_move(&g_assign, 2, 3, 0));
    assignment_publish(); assert(mask == 8 && (buttons[0] & 4));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory); source = tmp / 'publish.c'; exe = tmp / 'publish'
            source.write_text(harness, encoding='utf-8')
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                            '-I', str(root / 'nxnative/dm/android'), str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
