"""Exercise the actual AVC format/queue/conversion path with labeled YUV frames."""
import ctypes as C
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def sps(*, vui=True, signal=True, colour=True, profile=66, poc=0, chroma=1,
        scaling=False, extended_sar=False, interlaced=False, standard=1, full=False,
        timing=False, hrd=False):
    """Minimal SPS NAL following H.264 7.3.2.1.1 and E.1.1."""
    bits = ''

    def u(value, size=1):
        nonlocal bits
        bits += f'{int(value):0{size}b}'

    def ue(value):
        nonlocal bits
        code = f'{value + 1:b}'
        bits += '0' * (len(code) - 1) + code

    u(profile, 8); u(0, 8); u(30, 8); ue(0)
    if profile not in (66, 77, 88):
        ue(chroma)
        if chroma == 3:
            u(0)
        ue(0); ue(0); u(0); u(scaling)
        if scaling:
            for i in range(12 if chroma == 3 else 8):
                u(i == 0)
                if i == 0:
                    ue(16)  # delta_scale=-8 selects the default scaling list.
    ue(0); ue(poc)
    if poc == 0:
        ue(0)
    elif poc == 1:
        u(0); ue(0); ue(0); ue(2); ue(1); ue(2)
    ue(1); u(0); ue(0); ue(0); u(not interlaced)
    if interlaced:
        u(0)
    u(1); u(interlaced)
    if interlaced:
        for _ in range(4):
            ue(0)
    u(vui)
    if vui:
        u(extended_sar)
        if extended_sar:
            u(255, 8); u(256, 16); u(1, 16)  # forces an emulation-prevention byte
        u(1); u(0)  # overscan present, appropriate=false
        u(signal)
        if signal:
            u(5, 3); u(full); u(colour)
            if colour:
                matrix = {1: 1, 2: 5, 4: 6, 6: 9}.get(standard, 2)
                u(matrix, 8); u(matrix, 8); u(matrix, 8)
        u(hrd)
        if hrd:
            ue(0); ue(0)  # chroma sample locations
        u(timing)
        if timing:
            u(1001, 32); u(60000, 32); u(1)
        for _ in range(2):  # NAL and VCL HRD
            u(hrd)
            if hrd:
                ue(1); u(0, 8)
                for _ in range(2):
                    ue(1); ue(2); u(0)
                for _ in range(4):
                    u(23, 5)
        if hrd:
            u(0)  # low_delay_hrd_flag
        u(0); u(hrd)  # pic_struct absent, optional bitstream restriction
        if hrd:
            u(1)
            for _ in range(6):
                ue(1)
    bits += '1'  # rbsp_stop_one_bit, followed by byte alignment
    bits += '0' * (-len(bits) % 8)
    raw = bytes(int(bits[i:i+8], 2) for i in range(0, len(bits), 8))
    escaped = bytearray([0x67])
    zeros = 0
    for byte in raw:
        if zeros == 2 and byte <= 3:
            escaped.append(3)
            zeros = 0
        escaped.append(byte)
        zeros = zeros + 1 if byte == 0 else 0
    return bytes(escaped)


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
        source = (ROOT / 'nxnative/dm/android/movie_avc.c').read_text(encoding='utf-8')
        state = source[source.index('#define QMAX'):source.index('mavc *mavc_new(')]
        parameters = source[source.index('static void keep_ps('):source.index('static void set_csd(')]
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
''' + state + parameters + queue + output + r'''
// Each frame: standard, range, layout, height, Y, U, V, timestamp.
// Read all format changes before taking any frame: later metadata must not leak.
int exercise(const int *frames, int n, const uint8_t *sps_data, const int *sps_lengths,
             int rgba, int synth, int w, int h, uint8_t *out, int64_t *pts) {
    mavc a = {.rgba=rgba, .synth=synth};
    size_t offset = 0;
    for (int i=0; i<n; i++) {
        const int *p = frames + 8*i;
        mavc_param_sets(&a, sps_data + offset, (size_t)sps_lengths[i]);
        offset += (size_t)sps_lengths[i];
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
        src.write_text(code, encoding='utf-8')
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(src), '-o', str(lib)], check=True, capture_output=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.exercise.argtypes = [C.POINTER(C.c_int), C.c_int, C.POINTER(C.c_uint8), C.POINTER(C.c_int)] + [C.c_int]*4 + [C.POINTER(C.c_uint8), C.POINTER(C.c_int64)]
        cls.lib.exercise.restype = C.c_int

    @classmethod
    def tearDownClass(cls):
        if os.name == 'nt':  # Release the DLL before addClassCleanup removes its folder.
            C.windll.kernel32.FreeLibrary(C.c_void_p(cls.lib._handle))

    def run_frames(self, frames, rgba=True, synth=False, w=2, h=2, parameter_sets=None):
        n = len(frames)
        data = (C.c_int * (8*n))(*(value for f in frames for value in f))
        size = w*h*4 if rgba else w*h*3//2
        out, pts = (C.c_uint8 * (size*n))(), (C.c_int64 * n)()
        if parameter_sets is None:
            parameter_sets = [sps(standard=f[0], full=f[1] == 1) for f in frames]
        nals = [b'\0\0\0\1' + nal if nal else b'' for nal in parameter_sets]
        encoded = b''.join(nals)
        sps_data = (C.c_uint8 * len(encoded)).from_buffer_copy(encoded)
        lengths = (C.c_int * n)(*map(len, nals))
        self.assertEqual(self.lib.exercise(data, n, sps_data, lengths, rgba, synth, w, h, out, pts), n)
        return [bytes(out[i*size:(i+1)*size]) for i in range(n)], list(pts)

    def test_decoder_defaults_do_not_override_untagged_streams(self):
        for height in (2, 720):
            for nal in (b'', sps(vui=False), sps(signal=False)):
                with self.subTest(height=height, nal=nal.hex()):
                    out, _ = self.run_frames([(1, 1, 21, height, 128, 70, 190, 1)], parameter_sets=[nal])
                    self.assert_pixel(out[0][:4], reference(128, 70, 190, 1 if height >= 720 else 4, False))

    def test_range_declaration_is_independent_of_colour_description(self):
        frame = (1, 1, 21, 2, 128, 70, 190, 1)
        out, _ = self.run_frames([frame], parameter_sets=[sps(colour=False, full=True)])
        self.assert_pixel(out[0][:4], reference(128, 70, 190, 4, True))

    def test_truncated_sps_cannot_enable_decoder_defaults(self):
        for timing in (False, True):
            tagged = sps(full=True, timing=timing)
            for end in range(len(tagged)):
                with self.subTest(timing=timing, end=end):
                    out, _ = self.run_frames([(1, 1, 21, 2, 128, 70, 190, 1)], parameter_sets=[tagged[:end]])
                    self.assert_pixel(out[0][:4], reference(128, 70, 190, 4, False))

    def test_malformed_sps_retains_safe_fallback(self):
        invalid = [sps(profile=255), b'\x67\x42\0\x1e' + b'\0'*8 + b'\x80',
                   b'\x67\0\0\x03', b'\x67\0\0\x03\x04', bytes([0xe7]) + sps()[1:]]
        for nal in invalid:
            with self.subTest(nal=nal.hex()):
                out, _ = self.run_frames([(1, 1, 21, 2, 128, 70, 190, 1)], parameter_sets=[nal])
                self.assert_pixel(out[0][:4], reference(128, 70, 190, 4, False))

    def test_parameter_changes_preserve_queued_frame_declarations(self):
        frames = [(1, 1, 21, 2, 128, 70, 190, 30),
                  (1, 1, 21, 2, 128, 70, 190, 10),
                  (6, 2, 21, 2, 128, 70, 190, 20)]
        params = [sps(full=True), sps(signal=False), sps(standard=6)]
        for synth in (False, True):
            out, pts = self.run_frames(frames, synth=synth, parameter_sets=params)
            expected = [(30, 1, True), (10, 4, False), (20, 6, False)]
            if not synth:
                expected.sort()
            self.assertEqual(pts, [row[0] for row in expected])
            for picture, (_, standard, full) in zip(out, expected):
                self.assert_pixel(picture[:4], reference(128, 70, 190, standard, full))

    def test_sps_profiles_and_optional_fields_keep_declared_colour(self):
        for profile in (66, 77, 100, 244):
            for poc in (0, 1, 2):
                with self.subTest(profile=profile, poc=poc):
                    nal = sps(profile=profile, poc=poc, chroma=3, scaling=profile >= 100,
                              extended_sar=True, interlaced=True, full=True, timing=True, hrd=True)
                    out, _ = self.run_frames([(1, 1, 21, 2, 128, 70, 190, 1)], parameter_sets=[nal])
                    self.assert_pixel(out[0][:4], reference(128, 70, 190, 1, True))

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
