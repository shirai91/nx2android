"""Exercise the actual AVC format/queue/conversion path with labeled YUV frames."""
import ctypes as C
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def reference(y, u, v, standard, full):
    # Independent normalized Y'CbCr matrix (not the implementation's integer table).
    kr, kb = {1: (.2126, .0722), 6: (.2627, .0593)}.get(standard, (.299, .114))
    yy = y if full else (y - 16) * 255 / 219
    u, v = ((c - 128) * (1 if full else 255 / 224) for c in (u, v))
    r, b = yy + 2 * (1 - kr) * v, yy + 2 * (1 - kb) * u
    g = yy - 2 * kb * (1 - kb) / (1 - kr - kb) * u - 2 * kr * (1 - kr) / (1 - kr - kb) * v
    return tuple(max(0, min(255, round(c))) for c in (r, g, b)) + (255,)


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class AvcColor(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'nxnative/dm/android/movie_avc.c').read_text()
        state = source[source.index('#define QMAX'):source.index('mavc *mavc_new(')]
        queue = source[source.index('static void read_format('):source.index('// collect the pictures')]
        output = source[source.index('static void nv12_rgba('):]
        code = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct { int h, standard, range, layout; } AMediaFormat;
typedef void AMediaCodec;
typedef struct mavc mavc;
#define LOG(...) ((void)0)
#define die(...) abort()
#define AMEDIAFORMAT_KEY_WIDTH "width"
#define AMEDIAFORMAT_KEY_HEIGHT "height"
#define AMEDIAFORMAT_KEY_STRIDE "stride"
#define AMEDIAFORMAT_KEY_SLICE_HEIGHT "slice-height"
#define AMEDIAFORMAT_KEY_COLOR_FORMAT "color-format"
#define AMEDIAFORMAT_KEY_DISPLAY_CROP "crop"
static AMediaFormat format;
static AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *mc) { (void)mc; return &format; }
static void AMediaFormat_delete(AMediaFormat *f) { (void)f; }
static int AMediaFormat_getInt32(AMediaFormat *f, const char *key, int32_t *v) {
    if (!strcmp(key, "width") || !strcmp(key, "stride")) *v = 2;
    else if (!strcmp(key, "height") || !strcmp(key, "slice-height")) *v = f->h;
    else if (!strcmp(key, "color-format")) *v = f->layout;
    else if (!strcmp(key, "color-standard") && f->standard) *v = f->standard;
    else if (!strcmp(key, "color-range") && f->range) *v = f->range;
    else return 0;
    return 1;
}
static int AMediaFormat_getRect(AMediaFormat *f, const char *k, int32_t *l, int32_t *t, int32_t *r, int32_t *b) {
    (void)f; (void)k; (void)l; (void)t; (void)r; (void)b; return 0;
}
static void finish(mavc *a) { (void)a; }
static int drain(mavc *a, int64_t us) { (void)a; (void)us; return 0; }
''' + state + queue + output + r'''
// Each frame: standard, range, layout, height, Y, U, V, timestamp.
// Read all format changes before taking any frame: later metadata must not leak.
int exercise(const int *frames, int n, int rgba, int synth, int w, int h, uint8_t *out, int64_t *pts) {
    mavc a = {.rgba=rgba, .synth=synth};
    for (int i=0; i<n; i++) {
        const int *p = frames + 8*i;
        format = (AMediaFormat){.standard=p[0], .range=p[1], .layout=p[2], .h=p[3]};
        read_format(&a);
        size_t ysize = (size_t)2*p[3], size = ysize*3/2;
        uint8_t *src = malloc(size);
        memset(src,p[4],ysize);
        if (p[2] == 19) { memset(src+ysize,p[5],ysize/4); memset(src+ysize*5/4,p[6],ysize/4); }
        else for (size_t j=ysize;j<size;j+=2) { src[j]=p[5]; src[j+1]=p[6]; }
        keep_picture(&a,src,size,p[7]); free(src);
    }
    // A future format with missing metadata must reset its defaults.
    format = (AMediaFormat){.h=2,.layout=21}; read_format(&a);
    int count=0; size_t size=(size_t)w*h*(rgba ? 4 : 3)/ (rgba ? 1 : 2);
    while (count<n && mavc_take(&a,0,out+count*size,w,h,pts+count)) count++;
    return count;
}
'''
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        src = Path(cls.folder.name) / 'avc.c'
        lib = Path(cls.folder.name) / 'avc.so'
        src.write_text(code)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(src), '-o', str(lib)], check=True, capture_output=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.exercise.argtypes = [C.POINTER(C.c_int)] + [C.c_int]*5 + [C.POINTER(C.c_uint8), C.POINTER(C.c_int64)]
        cls.lib.exercise.restype = C.c_int

    def run_frames(self, frames, rgba=True, synth=False, w=2, h=2):
        n = len(frames)
        data = (C.c_int * (8*n))(*(value for f in frames for value in f))
        size = w*h*4 if rgba else w*h*3//2
        out, pts = (C.c_uint8 * (size*n))(), (C.c_int64 * n)()
        self.assertEqual(self.lib.exercise(data, n, rgba, synth, w, h, out, pts), n)
        return [bytes(out[i*size:(i+1)*size]) for i in range(n)], list(pts)

    def assert_pixel(self, actual, expected):
        self.assertTrue(all(abs(a-b) <= 2 for a, b in zip(actual[:3], expected[:3])), (tuple(actual), expected))
        self.assertEqual(actual[3], 255)

    def test_matrices_ranges_and_layouts(self):
        for std in (1, 2, 4, 6):
            for full in (False, True):
                for layout in (19, 21):
                    for yuv in ((128, 70, 190), (16, 128, 128), (235, 128, 128),
                                (0, 128, 128), (255, 128, 128), (128, 255, 0)):
                        with self.subTest(standard=std, full=full, layout=layout, yuv=yuv):
                            out, pts = self.run_frames([(std, 1 if full else 2, layout, 2, *yuv, 37)])
                            expected = reference(*yuv, std, full)
                            for pixel in range(4):
                                self.assert_pixel(out[0][pixel*4:pixel*4+4], expected)
                            self.assertEqual(pts, [37])

    def test_format_changes_keep_each_frames_metadata_and_order(self):
        frames = [(1, 1, 19, 2, 128, 70, 190, 30),
                  (6, 2, 21, 2, 128, 70, 190, 10),
                  (4, 2, 19, 2, 128, 70, 190, 20)]
        for synth in (False, True):
            out, pts = self.run_frames(frames, synth=synth)
            expected = frames if synth else sorted(frames, key=lambda f: f[-1])
            self.assertEqual(pts, [f[-1] for f in expected])
            for picture, f in zip(out, expected):
                self.assert_pixel(picture[:4], reference(*f[4:7], f[0], f[1] == 1))

    def test_missing_or_unknown_metadata_retains_height_fallback(self):
        for height in (2, 720):
            for std, color_range in ((0, 0), (99, 99), (0, 2)):
                frames = [(1, 1, 21, 2, 128, 70, 190, 1),
                          (std, color_range, 21, height, 128, 70, 190, 2)]
                out, _ = self.run_frames(frames)
                self.assert_pixel(out[1][:4], reference(128, 70, 190, 1 if height >= 720 else 4, False))

    def test_padding_is_black_and_nv12_samples_are_preserved(self):
        for full in (False, True):
            frame = (1, 1 if full else 2, 21, 2, 128, 70, 190, 1)
            raw, _ = self.run_frames([frame], rgba=False, w=4, h=4)
            black = 0 if full else 16
            self.assertEqual(raw[0], bytes([128, 128, black, black]*2 + [black]*8 + [70, 190] + [128]*6))
            rgba, _ = self.run_frames([frame], w=4, h=4)
            for y in range(4):
                for x in range(4):
                    pixel = rgba[0][(y*4+x)*4:(y*4+x+1)*4]
                    if x < 2 and y < 2:
                        self.assert_pixel(pixel, reference(128, 70, 190, 1, full))
                    else:
                        self.assertEqual(pixel, bytes((0, 0, 0, 255)))
