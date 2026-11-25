"""Blending and clear values of a colour target follow the format its attachment really has. begin_pass takes S.cfmt[i] from the view vk_tex_view
made (vk_view_fmt), the format the render pass, the framebuffer and the pipelines take. vk_tex_view gives an RGBA16UI view of a BC4 image its own
format, but keeps the image's format for a view across a compressed-class change outside a block view (a BC image decoded on the CPU among them) and
for a view a format-listed image (DM_VK_UBWC) does not list: there the view's NVN format names the other numeric class than the attachment has.
The harness runs the real vk_tex_view_as and img_new (gfx_vk_tex.c), then prepare_draw's blend choice and clear_color's clear value (gfx_vk_exec.c)."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

# name, DM_VK_UBWC, what the case is
CASES = [
    ('block_view', '3', 'BC4 image drawn into through an RGBA16UI view of its block size: the view\'s integer format'),
    ('bc_layers', '3', 'BC4 array, RGBA16UI view over 2 layers (no block view): the image\'s BC4, a float class'),
    ('bc_decoded', '3', 'BC3 decoded on the CPU (no BC on the device), RGBA32UI view: the image\'s RGBA8 UNORM'),
    ('listed_uint_view', '3', 'format-listed RGBA8 UNORM image, RGBA8UI view: the image\'s own UNORM'),
    ('listed_float_view', '3', 'format-listed R32UI image, R32F view: the image\'s own R32UI'),
    ('listed_sint', '3', 'format-listed RGBA8I image, RGBA8 UNORM view: the image\'s own RGBA8I'),
    ('mutable', '0', 'MUTABLE_FORMAT RGBA8 UNORM image, RGBA8UI view: the view\'s integer format'),
    ('listed_class', '7', 'format-listed RGBA8 image flagged as an image (DM_VK_UBWC bit 4), R32UI view: in its list, R32UI'),
]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class TargetFormatClass(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tex = csrc.read('nxnative/dm/android/gfx_vk_tex.c')
        exe = csrc.unit('nxnative/dm/android/gfx_vk_exec.c')
        header = csrc.read('nxnative/dm/android/gfx_vk.h')
        cls.exe_src = exe
        code = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include "nxnative/dm/gfx/nvn/nvn_obj.h"
#include "nxnative/dm/gfx/nvn/nvn_hmap.h"
static int red_view_fmt;
#define VKLOG(...) do { char line_[256]; snprintf(line_, sizeof line_, __VA_ARGS__); if (strstr(line_, "[RED] view format")) red_view_fmt++; } while (0)
#define LOG(...) ((void)0)
'''
        code += next(line for line in header.splitlines() if line.startswith('#define VKRED_ONCE(')) + '\n'
        code += csrc.between(header, 'typedef struct { VkFormat vk;', 'const vkfmt_t *vk_fmt', 'gfx_vk.h')
        code += csrc.between(header, 'typedef struct vkimg vkimg_t;', 'vkimg_t *vk_tex(', 'gfx_vk.h')
        code += r'''
static struct { VkDevice dev; VkPhysicalDevice pd; } V;
static uint64_t g_wseq; uint64_t g_tex_gen = 1;
static vkimg_t image;
static VkImageViewCreateInfo last_view;
static int view_calls, has_bc = 1;
static vkfmt_t fmts[0x9D];
static const vkfmt_t *vk_fmt(int f) {   // the formats of the cases, as gfx_vk_tex.c vk_fmt makes them (a BC format decoded on the CPU keeps the guest's blocks)
    if (f <= 0 || f >= 0x9D) return NULL;
    vkfmt_t *o = &fmts[f]; memset(o, 0, sizeof *o); o->kind = 1; o->bw = o->bh = 1;
    switch (f) {
    case 0x0A: o->vk = VK_FORMAT_R32_SFLOAT; o->bpb = 4; break;
    case 0x0B: o->vk = VK_FORMAT_R32_UINT; o->bpb = 4; o->is_int = 1; break;
    case 0x25: o->vk = VK_FORMAT_R8G8B8A8_UNORM; o->bpb = 4; break;
    case 0x27: o->vk = VK_FORMAT_R8G8B8A8_UINT; o->bpb = 4; o->is_int = 1; break;
    case 0x28: o->vk = VK_FORMAT_R8G8B8A8_SINT; o->bpb = 4; o->is_int = o->is_sint = 1; break;
    case 0x2C: o->vk = VK_FORMAT_R16G16B16A16_UINT; o->bpb = 8; o->is_int = 1; break;
    case 0x2F: o->vk = VK_FORMAT_R32G32B32A32_UINT; o->bpb = 16; o->is_int = 1; break;
    case 0x44: o->bpb = 16; o->bw = o->bh = 4;
        if (has_bc) { o->vk = VK_FORMAT_BC3_UNORM_BLOCK; o->compressed = 1; } else { o->vk = VK_FORMAT_R8G8B8A8_UNORM; o->dec = 4; o->dbpp = 4; } break;
    case 0x49: o->vk = VK_FORMAT_BC4_UNORM_BLOCK; o->bpb = 8; o->bw = o->bh = 4; o->compressed = 1; break;
    default: return NULL;
    }
    return o;
}
static vkimg_t *vk_tex(const tex_t *t) { (void)t; return &image; }
static void alias_check(vkimg_t *im) { (void)im; }
VkImageView vk_tex_view_as(const tex_t *t, const view_t *vw, int dim, int arrayed, int as_attachment, vkimg_t **img_out, VkFormat vfmt);
static void vk_watch(const vkimg_t *im, const char *s) { (void)im; (void)s; }
void vk_texdump_note(vkimg_t *im) { (void)im; }
static void vk_defer_free_view(VkImageView v) { (void)v; }
static void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p, VkFormat f, VkFormatProperties *out) {
    (void)p; (void)f; *out = (VkFormatProperties){.optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT};
}
static VkResult vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out) {
    (void)d; (void)a; last_view = *ci; *out = (VkImageView)(uintptr_t)(++view_calls); return VK_SUCCESS;
}
'''
        code += csrc.between(tex, 'static VkImageAspectFlags aspect_of(', '// ---------------- images', 'gfx_vk_tex.c')
        code += csrc.between(tex, 'static unsigned layers_of(', '// storage key:', 'gfx_vk_tex.c')
        code += csrc.between(tex, 'static VkFormat srgb_twin(', 'static vkimg_t *img_new(', 'gfx_vk_tex.c')
        # the image's creation flags and format list as img_new sets them; its GPU allocation is outside this host test
        code += csrc.between(tex, 'static vkimg_t *img_new(', '    VkResult r = vkCreateImage(', 'gfx_vk_tex.c') + '    return im;\n}\n'
        code += csrc.between(tex, 'typedef struct vview', '// ---------------- texture / sampler ID tables', 'gfx_vk_tex.c')
        # the render state prepare_draw and clear_color read
        code += r'''
typedef struct { uint32_t out_locs; } shader;
static vkimg_t g_s0_image;
static struct { vkimg_t *im; } P_att[9];
static struct { int ncol; VkFormat cfmt[8], dfmt; uint32_t samples; struct { int a2c; } ms;
    struct { unsigned logic_op, blend_en; } color; struct { unsigned mask; } chmask;
    shader *sh[3]; int rt_has_col[8];
    struct { int src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a; } blend[8]; } S;
static struct { unsigned debug_mode, ncol, dfmt, samples, a2c, logic_en, logic_op, cfmt[8], wmask[8], blend_en;
    struct { int src, dst, srca, dsta, eq, eqa; } bl[8]; } k;
'''
        at = exe.index('static int att_int_kind(')
        code += exe[at:exe.index('\n}\n', at) + 3]
        code += 'static void prepare(void) {\n' + csrc.between(exe, '    // attachments and blending\n', '    // Dual-source blending:', 'gfx_vk_exec.c') + '}\n'
        fn = exe.index('static void clear_color(const p_clear_color *c)')
        code += ('static VkClearColorValue clear_value(float v) {\n    p_clear_color cc = {0, 0xf, {v, v, v, v}}, *c = &cc;\n'
                 + csrc.between(exe, '    VkClearAttachment a =', '    // every layer of the pass', 'gfx_vk_exec.c clear_color', fn)
                 + '    return a.clearValue.color;\n}\n')
        code += r'''
// one colour target drawn into through a view: begin_pass's S.cfmt[0] = vk_view_fmt after vk_tex_view
static void target(int tfmt, int vfmt, int nvn_target, int layers, int flags) {
    tex_t t = {.d = {.target = nvn_target, .width = 16, .height = 16, .depth = layers, .levels = 1, .samples = 1, .format = tfmt, .flags = flags}};
    vkimg_t *made = img_new(&t, vk_fmt(tfmt)); assert(made); image = *made; free(made); image.img = (VkImage)(uintptr_t)1;
    view_t vw = {.set_format = 1, .format = (uint16_t)vfmt};
    if (layers > 1) { vw.set_layers = 1; vw.min_layer = 0; vw.nlayers = (uint16_t)layers; }
    vkimg_t *im = NULL; VkImageView v = vk_tex_view(&t, &vw, 0xff, 0, 1, &im);
    assert(v && im == &image && last_view.format == vk_view_fmt);   // the attachment view is made in the format begin_pass records
    S.cfmt[0] = vk_view_fmt;
    S.ncol = 1; S.rt_has_col[0] = 1; S.color.blend_en = 1; S.chmask.mask = 0xf; S.blend[0].src_rgb = 5;
    static shader fs = {.out_locs = 1}; S.sh[1] = &fs;
    memset(&k, 0, sizeof k); prepare();
}
int main(int argc, char **argv) {
    assert(argc == 2); const char *c = argv[1];
    int kind_view = -1;   // what the view's NVN format would say (0 float, 1 uint, 2 sint), for the cases where it differs from the attachment
    VkFormat want; int kind;
    if (!strcmp(c, "block_view")) { target(0x49, 0x2C, 1, 1, 0); want = VK_FORMAT_R16G16B16A16_UINT; kind = 1; assert(image.cflags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT); }
    else if (!strcmp(c, "bc_layers")) { target(0x49, 0x2C, 4, 2, 0); want = VK_FORMAT_BC4_UNORM_BLOCK; kind = 0; kind_view = 1; }
    else if (!strcmp(c, "bc_decoded")) { has_bc = 0; target(0x44, 0x2F, 1, 1, 0); want = VK_FORMAT_R8G8B8A8_UNORM; kind = 0; kind_view = 1; }
    else if (!strcmp(c, "listed_uint_view")) { target(0x25, 0x27, 1, 1, 0); want = VK_FORMAT_R8G8B8A8_UNORM; kind = 0; kind_view = 1; assert(image.fixed_fmt && red_view_fmt == 1); }
    else if (!strcmp(c, "listed_float_view")) { target(0x0B, 0x0A, 1, 1, 0); want = VK_FORMAT_R32_UINT; kind = 1; kind_view = 0; assert(image.fixed_fmt && red_view_fmt == 1); }
    else if (!strcmp(c, "listed_sint")) { target(0x28, 0x25, 1, 1, 0); want = VK_FORMAT_R8G8B8A8_SINT; kind = 2; kind_view = 0; assert(image.fixed_fmt && red_view_fmt == 1); }
    else if (!strcmp(c, "mutable")) { target(0x25, 0x27, 1, 1, 0); want = VK_FORMAT_R8G8B8A8_UINT; kind = 1; assert(!image.fixed_fmt && (image.cflags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)); }
    else if (!strcmp(c, "listed_class")) { target(0x25, 0x0B, 1, 1, NVN_TEXTURE_FLAGS_IMAGE); want = VK_FORMAT_R32_UINT; kind = 1; assert(image.fixed_fmt && image.fmt_cls[0] == VK_FORMAT_R32_UINT && !red_view_fmt); }
    else { fprintf(stderr, "unknown case %s\n", c); return 2; }
    if (vk_view_fmt != want) { fprintf(stderr, "attachment format %d, expected %d\n", vk_view_fmt, want); return 1; }
    assert(att_int_kind(vk_view_fmt) == kind);
    if (kind_view >= 0) assert(kind_view != kind);   // the view's NVN format names the other class: only the attachment's format gets these right
    // blending: only into a float or normalized attachment
    if ((k.blend_en & 1) != (kind == 0)) { fprintf(stderr, "blend %u for attachment kind %d\n", k.blend_en, kind); return 1; }
    // the clear value: 1.0 into an unsigned integer attachment is integer 1 (not the bits of 1.0), -1.0 into a signed one is -1, a float attachment keeps the floats
    VkClearColorValue one = clear_value(1.0f), neg = clear_value(-1.0f);
    if (kind == 1) assert(one.uint32[0] == 1 && one.uint32[3] == 1 && neg.uint32[0] == 0);
    else if (kind == 2) assert(one.int32[0] == 1 && neg.int32[0] == -1 && neg.int32[3] == -1);
    else assert(one.float32[0] == 1.0f && neg.float32[0] == -1.0f);
    return 0;
}
'''
        cls.temp = tempfile.TemporaryDirectory(prefix='nx2a-target-fmt-'); cls.addClassCleanup(cls.temp.cleanup)
        src = Path(cls.temp.name) / 'target.c'; src.write_text(code, encoding='utf-8'); cls.binary = Path(cls.temp.name) / 'target'
        made = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-Wno-unused-function', '-I', str(ROOT),
                               '-I', str(ROOT / 'nxnative/dm/gfx/nvn/include'), '-I', str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'),
                               str(src), '-o', str(cls.binary), '-pthread'], capture_output=True, text=True)
        if made.returncode: raise AssertionError(made.stderr)

    def test_blend_and_clear_follow_the_attachment_format(self):
        for name, ubwc, what in CASES:
            with self.subTest(case=name, what=what):
                env = {k: v for k, v in os.environ.items() if not k.startswith('DM_VK_')}
                env['DM_VK_UBWC'] = ubwc
                result = subprocess.run([str(self.binary), name], capture_output=True, text=True, env=env)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_begin_pass_records_the_view_format(self):
        # the harness above stands in for begin_pass's own two lines: the colour target's view, then S.cfmt[i] from the format vk_tex_view gave it
        body = csrc.function(self.exe_src, 'static int begin_pass(void) {', 'gfx_vk_exec.c begin_pass')
        view = body.index('VkImageView v = vk_tex_view(&S.rt_col[i]')
        self.assertIn('S.cfmt[i] = vk_view_fmt;', body[view:body.index('S.dfmt = VK_FORMAT_UNDEFINED', view)])


if __name__ == '__main__':
    unittest.main()
