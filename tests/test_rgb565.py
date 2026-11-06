"""Compile the renderer's format table and sample known NVN packed colours."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class Packed565(unittest.TestCase):
    def test_rgb_and_bgr_channel_order(self):
        source = (ROOT / 'nxnative/dm/android/gfx_vk_tex.c').read_text()
        table = source[source.index('enum { K_NONE'):source.index('static vkfmt_t g_fmt')]
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>
''' + table + r'''
// Interpret the selected Vulkan format using its defined packed bit layout.
static unsigned sample(unsigned format, unsigned word) {
    unsigned low=word&31, green=(word>>5)&63, high=(word>>11)&31;
    unsigned r,b;
    if (FT[format].vk==VK_FORMAT_B5G6R5_UNORM_PACK16) {r=low;b=high;}
    else {assert(FT[format].vk==VK_FORMAT_R5G6B5_UNORM_PACK16);r=high;b=low;}
    assert(!FT[format].rb && !FT[format].noalpha);
    return ((r*255/31)<<16)|((green*255/63)<<8)|(b*255/31);
}
int main(void) {
    // NVN RGB565: R occupies bits 0..4; BGR565 has the opposite order.
    const unsigned colours[]={0xff0000,0x00ff00,0x0000ff,0xffff00,0x00ffff,0,0xffffff};
    const unsigned rgb[]={0x001f,0x07e0,0xf800,0x07ff,0xffe0,0,0xffff};
    const unsigned bgr[]={0xf800,0x07e0,0x001f,0xffe0,0x07ff,0,0xffff};
    for (unsigned i=0;i<sizeof colours/sizeof *colours;i++) {
        assert(sample(0x3c,rgb[i])==colours[i]);
        assert(sample(0x95,bgr[i])==colours[i]);
    }
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / 'test.c'
            exe = Path(folder) / 'test'
            src.write_text(code)
            # The production table deliberately truncates extension enums to
            # uint16_t; vk_fmt reconstructs those unrelated packed formats.
            # GCC warns about that with -Woverflow, clang with -Wconstant-conversion;
            # GCC ignores the clang option when it reports nothing else.
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-overflow',
                            '-Wno-constant-conversion',
                            '-I', str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'),
                            str(src), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
