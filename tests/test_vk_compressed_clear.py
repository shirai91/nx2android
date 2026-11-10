"""Decode constant clear blocks independently, without a Vulkan device.

BC uses the existing third-party bcdec implementation. ASTC validates the
Khronos void-extent layout; texture2ddecoder adds an optional full decoder check.
"""
import ctypes
import importlib.util
import math
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BC = [
    ('BC1_RGB_UNORM', 1, False, False, 8, 4),
    ('BC1_RGB_SRGB', 1, False, True, 8, 4),
    ('BC1_RGBA_UNORM', 1, False, False, 8, 4),
    ('BC1_RGBA_SRGB', 1, False, True, 8, 4),
    ('BC2_UNORM', 2, False, False, 16, 4),
    ('BC2_SRGB', 2, False, True, 16, 4),
    ('BC3_UNORM', 3, False, False, 16, 4),
    ('BC3_SRGB', 3, False, True, 16, 4),
    ('BC4_UNORM', 4, False, False, 8, 1),
    ('BC4_SNORM', 4, True, False, 8, 1),
    ('BC5_UNORM', 5, False, False, 16, 2),
    ('BC5_SNORM', 5, True, False, 16, 2),
    ('BC6H_UFLOAT', 6, False, False, 16, 3),
    ('BC6H_SFLOAT', 6, True, False, 16, 3),
    ('BC7_UNORM', 7, False, False, 16, 4),
    ('BC7_SRGB', 7, False, True, 16, 4),
]
FOOTPRINTS = [(4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6),
              (8, 8), (10, 5), (10, 6), (10, 8), (10, 10), (12, 10), (12, 12)]
ASTC = [(f'ASTC_{w}x{h}_{transfer}', w, h, transfer == 'SRGB')
        for w, h in FOOTPRINTS for transfer in ('UNORM', 'SRGB')]


def bounded(value, signed=False, hdr=False):
    if math.isnan(value):
        return 0.0
    limit = 65504.0 if hdr else 1.0
    return min(limit, max(-limit if signed else 0.0, value))


def srgb(value):
    return 12.92 * value if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class CompressedClear(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        names = [entry[0] for entry in BC + ASTC]
        constants = ','.join(f'VK_FORMAT_{name}_BLOCK' for name in names)
        source = r'''
#include "vk_compressed_clear.h"
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
int encode(int format_index, const float color[4], unsigned char *block) {
    const VkFormat formats[] = { FORMATS };
    VkFormat format = format_index < 0 ? VK_FORMAT_R8G8B8A8_UNORM : formats[format_index];
    return vk_compressed_clear_block(format, color, block);
}
void decode(int family, int is_signed, const unsigned char *block, float *out) {
    unsigned char rgba[64];
    switch (family) {
    case 1: bcdec_bc1(block, rgba, 16); break;
    case 2: bcdec_bc2(block, rgba, 16); break;
    case 3: bcdec_bc3(block, rgba, 16); break;
    case 4: bcdec_bc4_float(block, out, 4, is_signed); return;
    case 5: bcdec_bc5_float(block, out, 8, is_signed); return;
    case 6: bcdec_bc6h_float(block, out, 12, is_signed); return;
    case 7: bcdec_bc7(block, rgba, 16); break;
    default: return;
    }
    for (int i = 0; i < 64; ++i) out[i] = rgba[i] / 255.0f;
}
'''.replace('FORMATS', constants)
        src = folder / 'compressed_clear.c'
        src.write_text(source)
        library = folder / 'compressed_clear.so'
        command = ['cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                   '-I', str(ROOT / 'nxnative/dm/android'),
                   '-I', str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'),
                   '-I', str(ROOT / 'nxnative/dm/third_party/bcdec'),
                   str(src), '-lm', '-o', str(library)]
        build = subprocess.run(command, capture_output=True, text=True)
        if build.returncode:
            raise AssertionError(build.stderr)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.encode.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_float),
                                   ctypes.POINTER(ctypes.c_ubyte)]
        cls.lib.encode.restype = ctypes.c_int
        cls.lib.decode.argtypes = [ctypes.c_int, ctypes.c_int,
                                   ctypes.POINTER(ctypes.c_ubyte), ctypes.POINTER(ctypes.c_float)]

    @classmethod
    def tearDownClass(cls):
        if os.name == 'nt':  # Release the DLL before addClassCleanup removes its folder.
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(cls.lib._handle))

    def encode(self, index, color, size):
        # Unaligned destination and guards catch whole-word stores and overruns.
        data = (ctypes.c_ubyte * 34)(*([0xA5] * 34))
        ptr = ctypes.cast(ctypes.byref(data, 1), ctypes.POINTER(ctypes.c_ubyte))
        got = self.lib.encode(index, (ctypes.c_float * 4)(*color), ptr)
        self.assertEqual(got, size)
        self.assertEqual(bytes(data[:1]), b'\xa5')
        self.assertEqual(bytes(data[size + 1:]), b'\xa5' * (33 - size))
        return bytes(data[1:size + 1])

    def check_bc(self, index, color):
        name, family, signed, nonlinear, size, channels = BC[index]
        block = self.encode(index, color, size)
        decoded = (ctypes.c_float * 64)()
        self.lib.decode(family, signed, (ctypes.c_ubyte * size).from_buffer_copy(block), decoded)
        expected = [bounded(c, signed, family == 6) for c in color]
        if nonlinear:
            expected[:3] = [srgb(c) for c in expected[:3]]
        if family == 1:
            expected[3] = 0.0 if 'RGBA' in name and expected[3] < 0.5 else 1.0
            if not expected[3]:
                expected[:3] = [0.0] * 3
        for pixel in range(16):
            for channel in range(channels):
                want = expected[channel]
                tolerance = 0.0021
                if family <= 3 and channel < 3:
                    tolerance = 0.019 if channel != 1 else 0.010
                if family == 2 and channel == 3:
                    tolerance = 0.034
                if signed and family in (4, 5):
                    tolerance = 0.004
                if family == 6:
                    tolerance = max(0.000004, abs(want) * (0.032 if signed else 0.016))
                if family == 7 and channel < 3:
                    tolerance = 0.006
                self.assertAlmostEqual(decoded[pixel * channels + channel], want,
                                       delta=tolerance,
                                       msg=f'{name} color={color} pixel={pixel} channel={channel}')
        return block

    def test_bc_all_formats_and_every_texel(self):
        rng = random.Random(170)
        colors = [(0, 0, 0, 0), (1, 1, 1, 1), (1, 0, 0.25, 0.5),
                  (0.18, 0.4, 0.73, 0.31), (0.003, 0.0032, 0.04, 0.7)]
        colors += [tuple(rng.random() for _ in range(4)) for _ in range(24)]
        for index, entry in enumerate(BC):
            for color in colors:
                with self.subTest(format=entry[0], color=color):
                    self.check_bc(index, color)

    def test_signed_and_hdr_colors(self):
        for index in (9, 11, 12, 13):
            for color in [(-1, -0.5, 0.25, 1), (-0.03, 0.125, 1.5, 1),
                          (2.0, 16.0, 400.0, 0), (65504, -65504, 8192, 0),
                          (0.00001, -0.00002, 0.0001, 1)]:
                with self.subTest(format=BC[index][0], color=color):
                    self.check_bc(index, color)

    def test_nonfinite_and_clamping(self):
        colors = [(math.nan, math.inf, -math.inf, math.nan),
                  (-2, 2, 1e30, math.inf), (-1e30, -0.3, -math.inf, -math.inf)]
        for index in range(len(BC)):
            for color in colors:
                with self.subTest(format=BC[index][0], color=color):
                    block = self.check_bc(index, color)
                    self.assertEqual(block, self.encode(index, color, len(block)))

    def test_bc1_punch_through_alpha_and_rgb_opaque(self):
        for index in range(4):
            for alpha in (0, 0.49, 0.5, 1):
                self.check_bc(index, (0.3, 0.6, 0.9, alpha))

    def check_astc(self, index, color):
        name, width, height, nonlinear = ASTC[index]
        block = self.encode(len(BC) + index, color, 16)
        bits = int.from_bytes(block[:8], 'little')
        # Khronos DataFormat, ASTC "Void-Extent Blocks": LDR, both reserved
        # bits one, all four 13-bit coordinates all-one (extent disabled).
        self.assertEqual(bits & 0x1FF, 0x1FC, name)
        self.assertEqual((bits >> 9) & 1, 0, name)
        self.assertEqual((bits >> 10) & 3, 3, name)
        self.assertEqual(bits >> 12, (1 << 52) - 1, name)
        components = struct.unpack('<4H', block[8:])
        expected = [bounded(c) for c in color]
        if nonlinear:
            expected[:3] = [srgb(c) for c in expected[:3]]
        for got, want in zip(components, expected):
            self.assertAlmostEqual(got / 65535.0, want, delta=0.51 / 65535.0, msg=name)
        return block, expected, width, height

    def test_astc_every_footprint_and_transfer(self):
        for index in range(len(ASTC)):
            for color in [(0, 0, 0, 0), (1, 1, 1, 1), (0.18, 0.4, 0.73, 0.31),
                          (0.003, 0.0032, 0.04, 0.7),
                          (math.nan, math.inf, -math.inf, 0.45)]:
                with self.subTest(format=ASTC[index][0], color=color):
                    self.check_astc(index, color)

    @unittest.skipUnless(importlib.util.find_spec('texture2ddecoder'),
                         'optional independent ASTC decoder is not installed')
    def test_astc_independent_decoder_and_edge_blocks(self):
        import texture2ddecoder
        for index in range(len(ASTC)):
            block, expected, width, height = self.check_astc(index, (0.18, 0.4, 0.73, 0.31))
            # Two blocks in each direction, with a clipped final row/column.
            w, h = width + 1, height + 1
            pixels = texture2ddecoder.decode_astc(block * 4, w, h, width, height)
            self.assertEqual(len(pixels), w * h * 4)
            for pixel in range(w * h):
                for byte, channel in enumerate((2, 1, 0, 3)):  # decoder returns BGRA
                    self.assertAlmostEqual(pixels[pixel * 4 + byte] / 255.0,
                                           expected[channel], delta=1 / 255.0)

    def test_unsupported_leaves_output_untouched(self):
        self.encode(-1, (0.2, 0.3, 0.4, 0.5), 0)


if __name__ == '__main__':
    unittest.main()
