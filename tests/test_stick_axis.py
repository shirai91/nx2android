"""Check a controller stick's value for the game (nxnative/dm/android/stick_axis.h) without a device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


DM = Path(__file__).resolve().parents[1] / 'nxnative/dm'


class StickAxis(unittest.TestCase):
    def test_clamp_and_round_gate(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the stick check')
        harness = r'''
#include <assert.h>
#include <stdlib.h>
#include "stick_axis.h"
static void xy(float x, float y, int *ox, int *oy) { int32_t a, b; dm_stick_xy(x, y, &a, &b); *ox = a; *oy = b; }
int main(void) {
    int x, y;
    // the dead zone, and a value inside the round gate is only scaled
    assert(dm_stick_axis(0.11f) == 0 && dm_stick_axis(-0.11f) == 0);
    assert(dm_stick_axis(0.5f) == (int32_t)(0.5f * DM_STICK_MAX));
    xy(0.5f, -0.5f, &x, &y); assert(x == (int32_t)(0.5f * DM_STICK_MAX) && y == -x);
    // a value past 1 never wraps: the host packs the axes into 16 bits, 1.2 * 32767 would arrive as -26216
    for (float v = 1.0f; v <= 1.5f; v += 0.01f) { assert(dm_stick_axis(v) == DM_STICK_MAX && dm_stick_axis(-v) == -DM_STICK_MAX); }
    // a square gate's corner (1, 1), up to 1.41: scaled together, so the diagonal is magnitude 1 and the proportions stay
    xy(1.0f, 1.0f, &x, &y); assert(x == y && x > 23000 && x < 23200);
    xy(-1.0f, 1.0f, &x, &y); assert(x == -y && abs(x) > 23000 && abs(x) < 23200);
    // a push mostly along one edge that reads 1.35 does not turn into the diagonal
    xy(1.35f, 0.2f, &x, &y); assert(x > 32000 && x <= DM_STICK_MAX && y > 3000 && y < 5500);
    // a stick on the rim of a round gate is unchanged, and the result never leaves the Switch's range
    xy(0.6f, 0.8f, &x, &y); assert(x == (int32_t)(0.6f * DM_STICK_MAX) && y == (int32_t)(0.8f * DM_STICK_MAX));
    for (float a = -1.5f; a <= 1.5f; a += 0.05f) for (float b = -1.5f; b <= 1.5f; b += 0.05f) {
        xy(a, b, &x, &y); assert(abs(x) <= DM_STICK_MAX && abs(y) <= DM_STICK_MAX);
        assert((x == 0 || (x > 0) == (a > 0)) && (y == 0 || (y > 0) == (b > 0)));   // never the opposite direction
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'stick.c', Path(tmp) / 'stick'
            src.write_text(harness)
            subprocess.run([cc, '-std=c11', '-Wall', '-Werror', f'-I{DM / "android"}', f'-I{DM / "host"}', str(src), '-o', str(exe), '-lm'], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
