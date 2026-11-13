"""A block-compressed image drawn into through an uncompressed view (gfx_vk_exec.c bc_draw_back): the pass draws into a stand-in in
blocks, and its level and layers are copied into the image through a buffer. The regions of both copies, for a 3D image's depth
slice and for an array's layer."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def between(text, a, b):
    i = text.index(a); return text[i:text.index(b, i)]


HARNESS = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
@FMT@
@IMG@
@PATT@
static const vkfmt_t F_BC4 = {.vk = VK_FORMAT_BC4_UNORM_BLOCK, .bpb = 8, .bw = 4, .bh = 4, .compressed = 1}, F_U16 = {.vk = VK_FORMAT_R16G16B16A16_UINT, .bpb = 8};
static const vkfmt_t *vk_fmt(int f) { return f == 0x49 ? &F_BC4 : f == 0x2c ? &F_U16 : NULL; }
static VkDeviceSize scratch_n; static int prepared, barriers;
static void *vk_scratch(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) { static char mem[8]; (void)align; scratch_n = n; *buf = (VkBuffer)(uintptr_t)7; *off = 4096; return mem; }
static VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)(uintptr_t)1; }
static void vk_tex_prepare(VkCommandBuffer cb, vkimg_t *im) { (void)cb; (void)im; prepared++; }
static void vk_img_barrier_all(VkCommandBuffer cb) { (void)cb; barriers++; }
static VkBufferImageCopy out_r, in_r; static VkImage out_img, in_img; static int order;
static void vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) { (void)cb; (void)l; assert(n == 1 && b == (VkBuffer)(uintptr_t)7 && order++ == 0); out_r = *r; out_img = i; }
static void vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) { (void)cb; (void)l; assert(n == 1 && b == (VkBuffer)(uintptr_t)7 && order++ == 1 && barriers >= 2); in_r = *r; in_img = i; }
@FN@
int main(void) {
    // Breath of the Wild's noise: 32x32x32 BC4 with 6 levels, level 1's depth slice 5 through an RGBA16UI view
    vkimg_t p = {.img = (VkImage)(uintptr_t)11, .w = 32, .h = 32, .d = 32, .layers = 1, .levels = 6, .vtype = VK_IMAGE_VIEW_TYPE_3D, .nvn_format = 0x49};
    vkimg_t d = {.img = (VkImage)(uintptr_t)12, .w = 8, .h = 8, .d = 32, .layers = 1, .levels = 6, .vtype = VK_IMAGE_VIEW_TYPE_3D, .nvn_format = 0x2c, .bc_of = &p};
    P_att[0].im = &d; P_att[0].bc_lv = 1; P_att[0].bc_ly = 5; P_att[0].bc_nly = 1;
    bc_draw_back();
    assert(order == 2 && prepared == 1 && out_img == d.img && in_img == p.img && p.gpu_w == 1 && scratch_n == 4 * 4 * 8);
    assert(out_r.bufferOffset == 4096 && out_r.imageSubresource.mipLevel == 1 && out_r.imageSubresource.baseArrayLayer == 0 && out_r.imageSubresource.layerCount == 1);
    assert(out_r.imageOffset.z == 5 && out_r.imageExtent.width == 4 && out_r.imageExtent.height == 4 && out_r.imageExtent.depth == 1);
    assert(in_r.bufferOffset == 4096 && in_r.imageSubresource.mipLevel == 1 && in_r.imageOffset.z == 5 && in_r.imageExtent.width == 16 && in_r.imageExtent.height == 16 && in_r.imageExtent.depth == 1);
    // the last level: one block for a 1x1 level
    order = 0; P_att[0].bc_lv = 5; P_att[0].bc_ly = 0; bc_draw_back();
    assert(out_r.imageExtent.width == 1 && out_r.imageExtent.height == 1 && in_r.imageExtent.width == 1 && in_r.imageExtent.height == 1 && scratch_n == 8);
    // an array (a cube's faces): layer 3 of level 0, and an attachment that is no stand-in is left alone
    vkimg_t pa = {.img = (VkImage)(uintptr_t)21, .w = 64, .h = 64, .d = 1, .layers = 6, .levels = 1, .vtype = VK_IMAGE_VIEW_TYPE_CUBE, .nvn_format = 0x49};
    vkimg_t da = {.img = (VkImage)(uintptr_t)22, .w = 16, .h = 16, .d = 1, .layers = 6, .levels = 1, .vtype = VK_IMAGE_VIEW_TYPE_CUBE, .nvn_format = 0x2c, .bc_of = &pa};
    vkimg_t plain = {.img = (VkImage)(uintptr_t)31, .w = 8, .h = 8, .d = 1, .layers = 1, .levels = 1, .nvn_format = 0x2c};
    order = 0; P_att[0].im = &plain; P_att[1].im = &da; P_att[1].bc_lv = 0; P_att[1].bc_ly = 3; P_att[1].bc_nly = 1;
    bc_draw_back();
    assert(order == 2 && out_img == da.img && in_img == pa.img && scratch_n == 16 * 16 * 8);
    assert(out_r.imageSubresource.baseArrayLayer == 3 && out_r.imageSubresource.layerCount == 1 && out_r.imageOffset.z == 0 && out_r.imageExtent.width == 16 && out_r.imageExtent.depth == 1);
    assert(in_r.imageSubresource.baseArrayLayer == 3 && in_r.imageExtent.width == 64 && in_r.imageExtent.height == 64 && in_r.imageExtent.depth == 1);
    return 0;
}
'''


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class BcDraw(unittest.TestCase):
    def test_the_drawn_level_and_layer_are_copied_in_as_blocks(self):
        header = (ROOT / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf-8')
        src = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text(encoding='utf-8')
        a = src.index('static struct { vkimg_t *im; VkImageView v; uint32_t fmt, layers, w, h, bc_lv, bc_ly, bc_nly; } P_att[9];')
        f = src.index('static void bc_draw_back(void) {')
        code = (HARNESS.replace('@FMT@', between(header, 'typedef struct { VkFormat vk;', 'const vkfmt_t *vk_fmt'))
                .replace('@IMG@', between(header, 'typedef struct vkimg vkimg_t;', 'vkimg_t *vk_tex('))
                .replace('@PATT@', src[a:src.index('\n', a)]).replace('@FN@', src[f:src.index('\n}\n', f) + 3]))
        with tempfile.TemporaryDirectory(prefix='nx2a_bc_draw_') as d:
            c = Path(d) / 't.c'; c.write_text(code, encoding='utf-8')
            exe = Path(d) / ('t.exe' if shutil.which('cc').lower().endswith('.exe') else 't')
            subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-I', str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'), '-o', str(exe), str(c)], check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
