"""Decode known blocks and compare independent Pillow BC decoders, without game data."""
import ctypes as C
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import unittest

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / 'nxnative/dm/gfx/tex'


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class BCDecode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        lib = Path(cls.tmp.name) / 'bc.so'
        source = (ROOT / 'nxnative/dm/android/gfx_vk_tex.c').read_text()
        adapter = source[source.index('static void bc_decode('):source.index('static pthread_mutex_t g_fmt_mu')]
        wrapper = Path(cls.tmp.name) / 'adapter.c'
        wrapper.write_text('#include <stdint.h>\n#include <stdlib.h>\n#include "nvn_bc.h"\n'
                           'typedef struct {unsigned dec, dbpp;} vkfmt_t;\n'
                           '#define die(...) abort()\n' + adapter +
                           '\nvoid legacy_decode(unsigned dec, unsigned pixel, const uint8_t *src, size_t pitch, '
                           'uint32_t w, uint32_t h, uint32_t z, uint8_t *dst) {'
                           'vkfmt_t f={dec,pixel};vk_bc_decode(&f,src,pitch,w,h,z,dst);}\n')
        subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-fno-strict-aliasing',
                        *getattr(cls, 'compiler_flags', []), '-I', str(TEX), str(TEX / 'nvn_bc.c'), str(TEX / 'nvn_deswizzle.c'),
                        str(TEX / '../nvn/nvn_tex.c'), str(ROOT / 'tests/bc_layout_fixture.c'), str(wrapper),
                        '-o', str(lib)], check=True, capture_output=True)
        cls.lib = C.CDLL(str(lib))
        cls.decode = cls.lib.nvn_bc_decode
        cls.decode.argtypes = [C.c_int, C.c_uint, C.c_uint, C.c_uint, C.c_void_p,
                              C.c_size_t, C.c_size_t, C.c_size_t, C.c_void_p,
                              C.c_size_t, C.c_size_t, C.c_size_t]
        cls.decode.restype = C.c_int
        cls.legacy = cls.lib.legacy_decode
        cls.legacy.argtypes = [C.c_uint, C.c_uint, C.c_void_p, C.c_size_t,
                               C.c_uint, C.c_uint, C.c_uint, C.c_void_p]
        cls.legacy.restype = None

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def block(self, fmt, data):
        pixel = self.lib.nvn_bc_pixel_size(fmt)
        out = C.create_string_buffer(16 * pixel)
        self.assertEqual(self.decode(fmt, 4, 4, 1, data, len(data), len(data), len(data),
                                     out, len(out), 4 * pixel, 16 * pixel), 0)
        return out.raw

    def test_bc1_rgb_and_rgba_transparency(self):
        data = struct.pack('<HHI', 0, 0xffff, 0xffffffff)
        self.assertEqual(self.block(0x42, data), bytes(64))
        self.assertEqual(self.block(0x41, data), bytes([0, 0, 0, 255]) * 16)
        self.assertEqual(self.block(0x46, data), self.block(0x42, data))

    def test_renderer_adapter_preserves_rgb_alpha_and_signed_endpoints(self):
        block = struct.pack('<HHI', 0, 0xffff, 0xffffffff)
        for dec, expected in [(1, bytes([0, 0, 0, 255])*16), (2, bytes(64))]:
            out = C.create_string_buffer(64)
            self.legacy(dec, 4, block, 8, 4, 4, 1, out)
            self.assertEqual(out.raw, expected)
        out = C.create_string_buffer(16)
        self.legacy(6, 1, bytes([129,127])+bytes(6), 8, 4, 4, 1, out)
        self.assertEqual(out.raw, bytes([129])*16)

    def test_signed_and_unsigned_bc4_bc5(self):
        indices = sum((i % 8) << (3 * i) for i in range(16)).to_bytes(6, 'little')
        unsigned = bytes([0, 255]) + indices
        self.assertEqual(self.block(0x49, unsigned), bytes([0, 255, 51, 102, 153, 204, 0, 255]) * 2)
        signed = bytes([129, 127]) + indices
        want = bytes(v & 255 for v in [-127, 127, -76, -25, 25, 76, -127, 127]) * 2
        self.assertEqual(self.block(0x4a, signed), want)
        rg = self.block(0x4b, unsigned + bytes([99, 0]) + bytes(6))
        self.assertEqual(rg[::2], self.block(0x49, unsigned))
        self.assertEqual(rg[1::2], bytes([99]) * 16)
        self.assertEqual(self.block(0x4c, signed + signed)[::2], want)

    def test_bc6h_signed_and_unsigned_black_with_opaque_alpha(self):
        for fmt in (0x4f, 0x50):
            self.assertEqual(self.block(fmt, bytes(16)), struct.pack('<4H', 0, 0, 0, 0x3c00) * 16)

    def test_bc7_known_mode6(self):
        bits, pos = 1 << 6, 7
        for value in [127, 127, 0, 0, 0, 0, 127, 127]:
            bits |= value << pos
            pos += 7
        self.assertEqual(self.block(0x4d, bits.to_bytes(16, 'little')), bytes([254, 0, 0, 254]) * 16)

    def test_independent_pillow_decoders(self):
        rng = random.Random(907)
        for fmt, bc in [(0x42, 1), (0x43, 2), (0x44, 3), (0x4b, 5), (0x4d, 7)]:
            for _ in range(100):
                data = rng.randbytes(self.lib.nvn_bc_block_size(fmt))
                if bc == 7:  # valid mode 6, random endpoints/pbits/indices
                    data = bytes([(data[0] & 128) | 64]) + data[1:]
                mode = 'RGB' if bc == 5 else 'RGBA'
                expected = Image.frombytes(mode, (4, 4), data, 'bcn', (bc, f'BC{bc}')).tobytes()
                actual = self.block(fmt, data)
                if bc == 5:
                    expected = bytes(v for i, v in enumerate(expected) if i % 3 != 2)
                # BC interpolation permits a one-unit difference between decoders.
                self.assertLessEqual(max(abs(a-b) for a, b in zip(actual, expected)), 1, hex(fmt))

    def test_edge_blocks_padded_rows_slices_and_truncation(self):
        # 5x3x2 BC4, non-tight source and destination strides and unaligned input.
        src = C.create_string_buffer(65)
        for z in range(2):
            for x in range(2):
                src[1 + z*32 + x*8] = bytes([20 + z*30 + x])
        out = C.create_string_buffer(bytes([0xab]) * 48)
        args = [0x49, 5, 3, 2, C.byref(src, 1), 64, 24, 32, out, 48, 7, 24]
        self.assertEqual(self.decode(*args), 0)
        for z in range(2):
            for y in range(3):
                self.assertEqual(out.raw[z*24+y*7:z*24+y*7+7], bytes([20+z*30])*4 + bytes([21+z*30, 0xab, 0xab]))
        for index, value in [(5, 47), (6, 8), (7, 23), (9, 40), (10, 4), (11, 20)]:
            bad = list(args); bad[index] = value
            self.assertEqual(self.decode(*bad), -1)

    def test_swizzled_mips_arrays_cubes_and_3d(self):
        self.assertEqual(self.lib.test_bc_layouts(), 115)

    def test_invalid_dimensions_formats_and_overflow_do_not_write(self):
        src = C.create_string_buffer(16)
        dst = C.create_string_buffer(bytes([0xab])*64, 64)
        args = [0x42, 4, 4, 1, src, 16, 8, 8, dst, 64, 16, 64]
        for index, value in [(0, 0), (0, 0x51), (1, 0), (1, 0xffffffff),
                             (2, 0), (2, 0xffffffff), (3, 0),
                             (6, C.c_size_t(-1).value), (10, C.c_size_t(-1).value)]:
            bad = list(args)
            bad[index] = value
            self.assertEqual(self.decode(*bad), -1)
            self.assertEqual(dst.raw, bytes([0xab])*64)


class UnsignedCharBCDecode(BCDecode):
    """Android ARM uses unsigned plain char; signed endpoints must still decode."""
    compiler_flags = ["-funsigned-char"]
