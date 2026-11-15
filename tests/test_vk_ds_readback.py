"""gfx_vk_tex.c ds_texels: a depth / stencil ReadTexels rebuilt in the guest's layout from the aspects' buffer copies, cut from the source."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from porter.toolchain import find_ndk
sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_cc   # noqa: E402

TEX = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_tex.c'
HARNESS = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
CUT
int main(void) {
    // the depth aspect's copy: 4 bytes a texel (floats, or D24 in the low 24 bits); the stencil aspect's: 1 byte a texel
    float df[3] = {0.25f, 1.0f, 0.0f}; uint8_t st[3] = {7, 0xff, 0x80}, out[24];
    memset(out, 0xee, sizeof out); ds_texels(0x36, VK_FORMAT_D32_SFLOAT_S8_UINT, (const uint8_t *)df, st, 3, out);   // DEPTH32F_STENCIL8: 8 bytes a texel
    for (int i = 0; i < 3; i++) { float x; memcpy(&x, out + 8 * i, 4); assert(x == df[i] && out[8 * i + 4] == st[i] && !out[8 * i + 5] && !out[8 * i + 6] && !out[8 * i + 7]); }
    uint32_t d24[3] = {0x00123456, 0xff000001, 0x00ffffff}, w[3];   // (the copy's top byte undefined: 0xff in the second)
    ds_texels(0x35, VK_FORMAT_D24_UNORM_S8_UINT, (const uint8_t *)d24, st, 3, (uint8_t *)w);   // DEPTH24_STENCIL8: stencil in the top byte
    assert(w[0] == 0x07123456 && w[1] == 0xff000001 && w[2] == 0x80ffffff);
    memset(out, 0, sizeof out); ds_texels(0x31, VK_FORMAT_D24_UNORM_S8_UINT, NULL, st, 3, out); assert(!memcmp(out, st, 3) && !out[3]);   // STENCIL8 in a combined image
    ds_texels(0x33, VK_FORMAT_X8_D24_UNORM_PACK32, (const uint8_t *)d24, NULL, 3, (uint8_t *)w); assert(w[0] == 0x123456 && w[1] == 1 && w[2] == 0xffffff);   // DEPTH24
    float dx[4] = {0.5f, 1.5f, -1.0f, 0.0f / 0.0f}; uint32_t w4[4];
    ds_texels(0x33, VK_FORMAT_D32_SFLOAT, (const uint8_t *)dx, NULL, 4, (uint8_t *)w4); assert(w4[0] == 0x800000 && w4[1] == 0xffffff && !w4[2] && !w4[3]);   // DEPTH24 kept as D32F
    uint16_t d16[2] = {0x1234, 0xfedc}, o16[2]; ds_texels(0x32, VK_FORMAT_D16_UNORM, (const uint8_t *)d16, NULL, 2, (uint8_t *)o16); assert(o16[0] == 0x1234 && o16[1] == 0xfedc);
    ds_texels(0x34, VK_FORMAT_D32_SFLOAT, (const uint8_t *)df, NULL, 3, (uint8_t *)w); assert(!memcmp(w, df, sizeof df));   // DEPTH32F
    ds_texels(0x36, VK_FORMAT_D32_SFLOAT_S8_UINT, NULL, NULL, 3, out); assert(!out[0] && !out[4]);   // an aspect the image lacks reads 0
    puts("ds ok"); return 0;
}
'''


@unittest.skipUnless(shutil.which('cc') and host_cc.posix(), 'a POSIX host C compiler is required')
class DepthStencilReadback(unittest.TestCase):
    def test_guest_layouts(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = TEX.read_text()
        start = src.index('static void ds_texels(')
        cut = src[start:src.index('\n}\n', start) + 3]
        with tempfile.TemporaryDirectory(prefix='nx2a-ds-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(HARNESS.replace('CUT', cut))
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn('ds ok', out.stdout)


if __name__ == '__main__':
    unittest.main()
