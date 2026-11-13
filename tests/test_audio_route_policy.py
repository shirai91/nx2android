from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class AudioRoutePolicy(unittest.TestCase):
    def test_disconnect_does_not_override_a_new_selection(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed')
        header = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/compat'
        code = r'''
#include <assert.h>
#include "audio_route_policy.h"
int main(void) {
    _Atomic int32_t route = 3;
    assert(!audio_route_fallback(&route, 7));
    assert(atomic_load(&route) == 3);
    assert(audio_route_fallback(&route, 3));
    assert(atomic_load(&route) == 0);
    assert(!audio_route_fallback(&route, 0));
    atomic_store(&route, 9);
    assert(!audio_route_fallback(&route, 3));
    assert(atomic_load(&route) == 9);
    return 0;
}'''
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'test.c'; exe = Path(tmp) / 'test'
            src.write_text(code)
            subprocess.run([cc, '-std=c11', '-I', str(header), str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
