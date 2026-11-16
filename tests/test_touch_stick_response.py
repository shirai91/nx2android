import pathlib
import subprocess
import tempfile
import unittest


class TouchStickResponse(unittest.TestCase):
    def test_centre_deadzone_direction_and_full_travel(self):
        source = (pathlib.Path(__file__).resolve().parents[1] / 'nxnative/dm/android/overlay_layout.h').read_text()
        function = source[source.index('static inline void ov_touch_stick('):source.index('// the layout in use:')]
        harness = '''#include <math.h>
#include <assert.h>
%s
int main(void) {
    float x, y, left, right;
    ov_touch_stick(0, 0, 100, 0, &x, &y); assert(x == 0 && y == 0);
    ov_touch_stick(3, 4, 100, 1, &x, &y); assert(x == 0 && y == 0);
    ov_touch_stick(50, 0, 100, 0, &left, &y);
    ov_touch_stick(50, 0, 100, 1, &right, &y); assert(right > 0 && right < left && y == 0);
    ov_touch_stick(-300, 400, 100, 1, &x, &y); assert(fabsf(x + .6f) < .0001f && fabsf(y - .8f) < .0001f);
    ov_touch_stick(100, 0, 0, 1, &x, &y); assert(x == 0 && y == 0);
}''' % function
        with tempfile.TemporaryDirectory() as tmp:
            c = pathlib.Path(tmp) / 'check.c'
            c.write_text(harness)
            subprocess.run(['cc', '-std=c11', str(c), '-lm', '-o', str(c.with_suffix(''))], check=True)
            subprocess.run([str(c.with_suffix(''))], check=True)


if __name__ == '__main__':
    unittest.main()
