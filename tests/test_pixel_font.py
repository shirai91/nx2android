"""The overlay's font pack and text helpers, on the committed font, without a device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'
PACK = DM / 'third_party/fusion-pixel-font/fusion-pixel-12px-proportional-zh_tw.nxpf'

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pixel_font.h"
static int count, minx = 9999, maxx = -9999, miny = 9999, maxy = -9999;
static void plot(void *u, int x, int y) { (void)u; count++; if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; }
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f); uint8_t *p = malloc((size_t)n); assert(fread(p, 1, (size_t)n, f) == (size_t)n); (void)argc;
    assert(!memcmp(p, "NXPF", 4) && sizeof(pf_index) == 12);
    const char *s = "A\xe5\x8b\x9d\xf0\x9f\x98\x80\xff"; assert(pf_next(&s) == 'A'); assert(pf_next(&s) == 0x52DD); assert(pf_next(&s) == 0x1F600); assert(pf_next(&s) == 0xFFFD); assert(!*s);
    const char *cut = "\xe5\x8b"; assert(pf_next(&cut) == 0xFFFD);   // a cut sequence stops at its terminator
    assert((pf_find(p, 'A')->cp_adv & 255) == 8 && pf_find(p, 0x52DD) && pf_find(p, 0xD55C));   // A, the Traditional Chinese and the Hangul Ark lacks
    assert(pf_find(p, 0x10FFFF)->cp_adv >> 8 == 0xFFFD); assert(!pf_find(NULL, 'A')); assert(pf_width(NULL, "A") == 0);
    assert(pf_width(p, "A\xe5\x8b\x9d") == 8 + 12);
    pf_draw(p, "A", 10, 100, 2, plot, NULL);   // 7 x 9, sitting on the baseline, at twice the size
    assert(minx == 10 && maxx == 23 && miny == 82 && maxy == 99 && count % 4 == 0);
    char t[16]; strcpy(t, "a\xe5\x8b\x9d"); assert(pf_chop(t, strlen(t)) == 1 && !strcmp(t, "a")); assert(pf_chop(t, 1) == 0 && pf_chop(t, 0) == 0);
    strcpy(t, "a\xe5\x8b"); pf_trim(t); assert(!strcmp(t, "a")); strcpy(t, "a\xe5\x8b\x9d"); pf_trim(t); assert(strlen(t) == 4); strcpy(t, "ab"); pf_trim(t); assert(!strcmp(t, "ab"));
    assert(pf_units("a\xe5\x8b\x9d\xf0\x9f\x98\x80") == 4);
    return 0;
}
'''


class PixelFont(unittest.TestCase):
    def test_reader_on_the_committed_pack(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: self.skipTest('a C compiler is needed')
        with tempfile.TemporaryDirectory() as t:
            c, exe = Path(t) / 'h.c', Path(t) / 'h'
            c.write_text(HARNESS)
            subprocess.run([cc, '-std=c11', '-Wall', '-Werror', f'-I{DM / "android"}', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe), str(PACK)], check=True)

    def test_converter(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('make_pixel_font', DM / 'tools/make_pixel_font.py'); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        with tempfile.TemporaryDirectory() as t:
            b = Path(t) / 'f.bdf'
            b.write_text('STARTFONT 2.1\nCHARS 2\nSTARTCHAR b\nENCODING 66\nDWIDTH 3 0\nBBX 2 1 0 -1\nBITMAP\nC0\nENDCHAR\nSTARTCHAR a\nENCODING 65\nDWIDTH 2 0\nBBX 1 2 1 0\nBITMAP\n80\n00\nENDCHAR\nENDFONT\n')
            out = m.pack(m.glyphs(b))
        self.assertEqual(out[:8], b'NXPF\x02\0\0\0')
        self.assertEqual(out[8:20], bytes([2, 65, 0, 0, 1, 0, 0, 0, 2, 1, 0, 0]))   # sorted: A first, bitmap offset 0
        self.assertEqual(out[32:], b'\x80\x00\xC0')

    def test_port_packs_the_font_and_its_licenses(self):
        from porter import pipeline
        args = pipeline.Port.ui_font(None)
        names = [a.partition('=')[0] for a in args[1::2]]
        self.assertIn('font/ui.bin', names); self.assertIn('font/OFL.txt', names)
        for lic in ('ark-pixel/OFL.txt', 'cubic-11/OFL.txt', 'galmuri/LICENSE.txt'): self.assertIn(f'font/LICENSES/{lic}', names)
        for a in args[1::2]: self.assertTrue(Path(a.partition('=')[2]).is_file(), a)


if __name__ == '__main__':
    unittest.main()
