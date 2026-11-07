import pathlib
import shutil
import subprocess
import tempfile
import unittest


class HostsegScanTest(unittest.TestCase):
    def test_marks_only_pointers_into_other_live_segments(self):
        root = pathlib.Path(__file__).resolve().parents[1]
        source = r'''
#include <assert.h>
#include <string.h>
#include "nxnative/dm/gfx/nvn/nvn_hostseg_scan.h"
int main(void) {
    enum { BASE = 0x100000, SPAN = 0x4000 };
    int32_t pages[4] = {1, 2, 0, 3};   // segments 0, 1, nothing, segment 2
    uint8_t mark[3];
    uint64_t w[] = {0, BASE - 8, BASE + SPAN, BASE + 0x2000};   // outside the arena, and a page with no segment
    memset(mark, 0, sizeof mark);
    hostseg_mark_words(w, 4, BASE, SPAN, pages, -1, mark);
    assert(!mark[0] && !mark[1] && !mark[2]);
    uint64_t x[] = {BASE, BASE + 0x1ff8, BASE + 0x3000};
    hostseg_mark_words(x, 3, BASE, SPAN, pages, -1, mark);
    assert(mark[0] && mark[1] && mark[2]);
    memset(mark, 0, sizeof mark);
    hostseg_mark_words(x, 3, BASE, SPAN, pages, 1, mark);   // a segment's pointer to itself keeps nothing alive
    assert(mark[0] && !mark[1] && mark[2]);
    return 0;
}
'''
        compiler = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
        if not compiler:
            self.skipTest('no C compiler')
        with tempfile.TemporaryDirectory() as directory:
            source_path = pathlib.Path(directory) / 'test.c'
            binary = pathlib.Path(directory) / 'test'
            source_path.write_text(source)
            subprocess.run([compiler, '-std=c11', f'-I{root}', str(source_path), '-o', str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
