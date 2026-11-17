import pathlib
import shutil
import subprocess
import tempfile
import unittest

from porter.upstream import android_profile


class TouchStickResponse(unittest.TestCase):
    def test_centre_deadzone_direction_and_full_travel(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the stick response check')
        root = pathlib.Path(__file__).resolve().parents[1]
        harness = r'''#include <assert.h>
#include <math.h>
#include "overlay_layout.h"
int main(void) {
    float x, y, left, right;
    ov_touch_stick(0, 0, 100, 0, &x, &y); assert(x == 0 && y == 0);
    ov_touch_stick(3, 4, 100, 1, &x, &y); assert(x == 0 && y == 0);
    ov_touch_stick(50, 0, 100, 0, &left, &y);
    ov_touch_stick(50, 0, 100, 1, &right, &y); assert(right > 0 && right < left && y == 0);
    ov_touch_stick(-300, 400, 100, 1, &x, &y); assert(fabsf(x + .6f) < .0001f && fabsf(y - .8f) < .0001f);
    ov_touch_stick(100, 0, 0, 1, &x, &y); assert(x == 0 && y == 0);
}'''
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / 'android_profile.h').write_text(android_profile.header(None), encoding='utf-8')
            c = tmp / 'check.c'
            c.write_text(harness, encoding='utf-8')
            exe = c.with_suffix('')
            subprocess.run([cc, '-std=c11', '-I', str(tmp), '-I', str(root / 'nxnative/dm/android'),
                            '-I', str(root / 'nxnative/dm/host'), str(c), '-lm', '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
