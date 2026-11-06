import pathlib
import subprocess
import tempfile
import unittest


class VkSlotHandleTest(unittest.TestCase):
    def test_image_alias_and_texture_fallback(self):
        root = pathlib.Path(__file__).resolve().parents[1]
        source = r'''
#include <assert.h>
#include "nxnative/dm/android/gfx_vk_slot.h"
int main(void) {
    uint64_t tex[256] = {0}, img[32] = {0};
    tex[31] = 11; tex[32] = 12; tex[39] = 13; tex[40] = 14; tex[255] = 15;
    img[0] = 21; img[7] = 27; img[8] = 28;
    assert(vk_tex_slot_handle(tex, img, 31) == 11);
    assert(vk_tex_slot_handle(tex, img, 32) == 21);
    assert(vk_tex_slot_handle(tex, img, 39) == 27);
    assert(vk_tex_slot_handle(tex, img, 40) == 14);
    assert(vk_tex_slot_handle(tex, img, 255) == 15);
    assert(vk_tex_slot_handle(tex, img, 256) == 0);
    assert(vk_tex_slot_handle(tex, img, (unsigned)-1) == 0);
    img[0] = 0;
    assert(vk_tex_slot_handle(tex, img, 32) == 12);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            c = pathlib.Path(temp) / 'slot.c'
            out = pathlib.Path(temp) / 'slot'
            c.write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(c), '-o', str(out)], check=True)
            subprocess.run([str(out)], check=True)


if __name__ == '__main__':
    unittest.main()
