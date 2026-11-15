"""A block-compressed image drawn into through an uncompressed view (DM_VK_BC_DRAW): gfx_vk_tex.c bc_standin_ok decides whether
it takes a stand-in in blocks (only 3D images by default when on, every one with 2, never where the image's levels exceed the
stand-in's own chain), and gfx_vk_exec.c copies the level and layers a pass draws from the image into the stand-in before the
pass (bc_fill) and back after it (bc_draw_back), each through a buffer."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def between(text, a, b):
    i = text.index(a); return text[i:text.index(b, i)]


HEAD = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
@FMT@
@IMG@
__attribute__((unused)) static const vkfmt_t F_BC4 = {.vk = VK_FORMAT_BC4_UNORM_BLOCK, .bpb = 8, .bw = 4, .bh = 4, .compressed = 1}, F_BC7 = {.vk = VK_FORMAT_BC7_UNORM_BLOCK, .bpb = 16, .bw = 4, .bh = 4, .compressed = 1},
                     F_U16 = {.vk = VK_FORMAT_R16G16B16A16_UINT, .bpb = 8}, F_U32 = {.vk = VK_FORMAT_R32G32B32A32_UINT, .bpb = 16};
'''

COPIES = HEAD + r'''
@PATT@
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
    vkimg_t p = {.img = (VkImage)(uintptr_t)11, .w = 32, .h = 32, .d = 32, .layers = 1, .levels = 6, .vtype = VK_IMAGE_VIEW_TYPE_3D, .nvn_format = 0x49, .layout = VK_IMAGE_LAYOUT_GENERAL};
    vkimg_t d = {.img = (VkImage)(uintptr_t)12, .w = 8, .h = 8, .d = 32, .layers = 1, .levels = 6, .vtype = VK_IMAGE_VIEW_TYPE_3D, .nvn_format = 0x2c, .bc_of = &p};
    P_att[0].im = &d; P_att[0].bc_lv = 1; P_att[0].bc_ly = 5; P_att[0].bc_nly = 1;
    bc_draw_back();
    assert(order == 2 && prepared == 1 && out_img == d.img && in_img == p.img && p.gpu_w == 1 && scratch_n == 4 * 4 * 8);
    assert(out_r.bufferOffset == 4096 && out_r.imageSubresource.mipLevel == 1 && out_r.imageSubresource.baseArrayLayer == 0 && out_r.imageSubresource.layerCount == 1);
    assert(out_r.imageOffset.z == 5 && out_r.imageExtent.width == 4 && out_r.imageExtent.height == 4 && out_r.imageExtent.depth == 1);
    assert(in_r.bufferOffset == 4096 && in_r.imageSubresource.mipLevel == 1 && in_r.imageOffset.z == 5 && in_r.imageExtent.width == 16 && in_r.imageExtent.height == 16 && in_r.imageExtent.depth == 1);
    // the fill before the pass: the same region the other way, image → buffer → stand-in
    order = 0; barriers = 0; bc_fill(&d, 1, 5, 1);
    assert(order == 2 && out_img == p.img && in_img == d.img && scratch_n == 4 * 4 * 8);
    assert(out_r.imageSubresource.mipLevel == 1 && out_r.imageOffset.z == 5 && out_r.imageExtent.width == 16 && out_r.imageExtent.height == 16 && out_r.imageExtent.depth == 1);
    assert(in_r.imageSubresource.mipLevel == 1 && in_r.imageOffset.z == 5 && in_r.imageExtent.width == 4 && in_r.imageExtent.height == 4 && in_r.bufferOffset == 4096);
    // an image never used (its blocks undefined): no fill, the stand-in stays as it is
    p.layout = VK_IMAGE_LAYOUT_UNDEFINED; order = 0; bc_fill(&d, 1, 5, 1); assert(order == 0); p.layout = VK_IMAGE_LAYOUT_GENERAL;
    // the last level: one block for a 1x1 level
    order = 0; P_att[0].bc_lv = 5; P_att[0].bc_ly = 0; bc_draw_back();
    assert(out_r.imageExtent.width == 1 && out_r.imageExtent.height == 1 && in_r.imageExtent.width == 1 && in_r.imageExtent.height == 1 && scratch_n == 8);
    // an array (a cube's faces): layer 3 of level 0, and an attachment that is no stand-in is left alone
    vkimg_t pa = {.img = (VkImage)(uintptr_t)21, .w = 64, .h = 64, .d = 1, .layers = 6, .levels = 1, .vtype = VK_IMAGE_VIEW_TYPE_CUBE, .nvn_format = 0x49, .layout = VK_IMAGE_LAYOUT_GENERAL};
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

CHOICE = HEAD + r'''
@FN@
static vkimg_t img(uint32_t w, uint32_t h, uint32_t d, uint32_t levels, VkImageViewType t) { vkimg_t i = {.w = w, .h = h, .d = d, .layers = 1, .levels = levels, .samples = 1, .vtype = t}; return i; }
static int ok(vkimg_t i, const vkfmt_t *cf, const vkfmt_t *vf, int mode, uint32_t *bw, uint32_t *bh) { *bw = *bh = 0; return bc_standin_ok(&i, cf, vf, mode, bw, bh); }
int main(void) {
    uint32_t bw, bh; const VkImageViewType D3 = VK_IMAGE_VIEW_TYPE_3D, D2 = VK_IMAGE_VIEW_TYPE_2D;
    // off by default: mode 0 never stands in
    assert(!ok(img(32, 32, 32, 6, D3), &F_BC4, &F_U16, 0, &bw, &bh));
    // mode 1: a 3D image (Breath of the Wild's 32x32x32 BC4 noise, 6 levels: an 8x8x32 stand-in holds 6), not a 2D one
    assert(ok(img(32, 32, 32, 6, D3), &F_BC4, &F_U16, 1, &bw, &bh) && bw == 8 && bh == 8);
    assert(!ok(img(64, 64, 1, 1, D2), &F_BC4, &F_U16, 1, &bw, &bh));
    // mode 2: a 2D one too
    assert(ok(img(64, 64, 1, 1, D2), &F_BC4, &F_U16, 2, &bw, &bh) && bw == 16 && bh == 16);
    // the level guard: a 32x32x16 BC4 image of 6 levels is an 8x8x16 stand-in of 5 at most, so it keeps the direct path; with 5 it stands in
    assert(!ok(img(32, 32, 16, 6, D3), &F_BC4, &F_U16, 1, &bw, &bh));
    assert(ok(img(32, 32, 16, 5, D3), &F_BC4, &F_U16, 1, &bw, &bh));
    // a square 2D BC texture with its full chain (64x64, 7 levels): a 16x16 stand-in holds 5, so not even with mode 2
    assert(!ok(img(64, 64, 1, 7, D2), &F_BC7, &F_U32, 2, &bw, &bh));
    assert(ok(img(64, 64, 1, 5, D2), &F_BC7, &F_U32, 2, &bw, &bh));
    // a view that is not one texel a block, a compressed view, an uncompressed image, a multisampled one
    assert(!ok(img(32, 32, 32, 1, D3), &F_BC4, &F_U32, 1, &bw, &bh));
    assert(!ok(img(32, 32, 32, 1, D3), &F_BC4, &F_BC4, 1, &bw, &bh));
    assert(!ok(img(32, 32, 32, 1, D3), &F_U16, &F_U16, 1, &bw, &bh));
    vkimg_t ms = img(64, 64, 1, 1, D2); ms.samples = 4; assert(!bc_standin_ok(&ms, &F_BC4, &F_U16, 2, &bw, &bh));
    // levels not whole in blocks of the stand-in's chain (a 20x20 image: level 1 is 10x10, 3x3 blocks, where the 5x5 stand-in's level 1 is 2x2)
    assert(!ok(img(20, 20, 1, 2, D2), &F_BC4, &F_U16, 2, &bw, &bh));
    return 0;
}
'''


def build_and_run(code, tag):
    with tempfile.TemporaryDirectory(prefix='nx2a_bc_draw_') as d:
        c = Path(d) / f'{tag}.c'; c.write_text(code, encoding='utf-8')
        exe = Path(d) / (f'{tag}.exe' if shutil.which('cc').lower().endswith('.exe') else tag)
        subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-I', str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'), '-o', str(exe), str(c)], check=True)
        return subprocess.run([str(exe)], capture_output=True, text=True)


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class BcDraw(unittest.TestCase):
    def setUp(self):
        header = (ROOT / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf-8')
        self.head = {'@FMT@': between(header, 'typedef struct { VkFormat vk;', 'const vkfmt_t *vk_fmt'), '@IMG@': between(header, 'typedef struct vkimg vkimg_t;', 'vkimg_t *vk_tex(')}

    def fill(self, code, **parts):
        for k, v in {**self.head, **parts}.items(): code = code.replace(k, v)
        return code

    def test_the_level_and_layers_a_pass_draws_are_filled_and_copied_back_as_blocks(self):
        src = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text(encoding='utf-8')
        a = src.index('static struct { vkimg_t *im; VkImageView v; uint32_t fmt, layers, w, h, bc_lv, bc_ly, bc_nly; } P_att[9];')
        f = src.index('static int bc_region('); g = src.index('\n}\n', src.index('static void bc_draw_back(void) {')) + 3
        r = build_and_run(self.fill(COPIES, **{'@PATT@': src[a:src.index('\n', a)], '@FN@': src[f:g]}), 'copies')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_which_images_take_a_stand_in(self):
        src = (ROOT / 'nxnative/dm/android/gfx_vk_tex.c').read_text(encoding='utf-8')
        f = src.index('static int bc_standin_ok('); g = src.index('\n}\n', f) + 3
        r = build_and_run(self.fill(CHOICE, **{'@FN@': src[f:g]}), 'choice')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
