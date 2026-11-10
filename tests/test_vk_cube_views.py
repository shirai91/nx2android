"""Cube requests reject incompatible backing images before the descriptor fallback."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def between(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class CubeViews(unittest.TestCase):
    def test_production_views_and_descriptor_fallback(self):
        tex = (ROOT/'nxnative/dm/android/gfx_vk_tex.c').read_text()
        exe = (ROOT/'nxnative/dm/android/gfx_vk_exec.c').read_text()
        header = (ROOT/'nxnative/dm/android/gfx_vk.h').read_text()
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
#define VKLOG(...) ((void)0)
#define VKRED_ONCE(...) ((void)0)
'''
        code += between(header, 'typedef struct { VkFormat vk;', 'const vkfmt_t *vk_fmt')
        code += between(header, 'typedef struct vkimg vkimg_t;', 'vkimg_t *vk_tex(')
        code += between(header, 'typedef struct __attribute__((packed)) { uint8_t kind, desc, dim, flags;', '_Static_assert(sizeof(dmsv_bind)')
        code += r'''
static struct { VkDevice dev; VkPhysicalDevice pd; } V;
static uint64_t g_wseq, g_tex_gen=1, g_set_epoch=1, g_x_logfs;
static int g_var_now, g_tracing;
static struct { struct { uint64_t key; } *sh[2]; } S;
static vkimg_t image;
static tex_t texture;
static view_t subview;
static int has_subview, view_calls, dummy_calls, lookup_calls;
static VkImageViewCreateInfo last_view;
static VkImageView dummy;
static int dummy_dim, dummy_arr, dummy_ms, dummy_storage, dummy_int;
static const vkfmt_t format = {.vk=VK_FORMAT_R8G8B8A8_UNORM, .kind=1, .bpb=4};
static const vkfmt_t *vk_fmt(int f) { (void)f; return &format; }
static vkimg_t *vk_tex(const tex_t *t) { (void)t; return &image; }
static void alias_check(vkimg_t *im) { (void)im; }
static void vk_watch(const vkimg_t *im, const char *s) { (void)im; (void)s; }
void vk_texdump_note(vkimg_t *im) { (void)im; }
static void vk_defer_free_view(VkImageView v) { (void)v; }
static void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p, VkFormat f, VkFormatProperties *out) {
    (void)p; (void)f; *out=(VkFormatProperties){.optimalTilingFeatures=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT};
}
static VkResult vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out) {
    (void)d; (void)a; last_view=*ci; *out=(VkImageView)(uintptr_t)(++view_calls); return VK_SUCCESS;
}
static vkimg_t *vk_tex_by_id(int tid, const view_t **v, const tex_t **t) {
    (void)tid; ++lookup_calls; *t=&texture; *v=has_subview?&subview:NULL; return &image;
}
static uint64_t mtl_last_fence(void) { return 4; }
static VkSampler samp_for(int st, const dmsv_bind *b, uint64_t h, int n) {
    (void)st; (void)b; (void)h; (void)n; return (VkSampler)(uintptr_t)99;
}
static VkImageView vk_dummy_view(int dim, int arr, int ms, int storage, int is_int) {
    ++dummy_calls; dummy_dim=dim; dummy_arr=arr; dummy_ms=ms; dummy_storage=storage; dummy_int=is_int;
    return dummy;
}
'''
        code += between(tex, 'static VkImageAspectFlags aspect_of(', '// ---------------- images')
        code += between(tex, 'static unsigned layers_of(', '// storage key:')
        code += between(tex, 'static VkFormat srgb_twin(', 'static vkimg_t *img_new(')
        # Execute the actual image setup through its saved creation flags; GPU
        # allocation/upload is outside this host-only compatibility regression.
        setup = between(tex, 'static vkimg_t *img_new(', '    VkResult r = vkCreateImage(')
        code += setup + '    return im;\n}\n'
        code += between(tex, 'typedef struct vview', '// ---------------- texture / sampler ID tables')
        code += between(exe, 'enum { TVC =', 'static uint64_t g_num_um')
        code += between(exe, 'static int tex_desc_for(', '// Reuse within a submission')
        code += r'''
static void reset(int target, unsigned w, unsigned h, unsigned layers, unsigned samples) {
    vk_tex_view_drop_all(&image); memset(g_tvc,0,sizeof g_tvc); ++g_set_epoch;
    texture=(tex_t){.d={.target=target,.width=w,.height=h,.depth=layers,.levels=3,.samples=samples,.format=0x25}};
    vkimg_t *created=img_new(&texture,&format); image=*created; free(created);
    image.img=(VkImage)(uintptr_t)1; has_subview=0; view_calls=dummy_calls=lookup_calls=0;
    dummy=(VkImageView)(uintptr_t)10000;
}
static void rejected(int arr) {
    int calls=view_calls; VkImageView v=vk_tex_view(&texture,has_subview?&subview:NULL,3,arr,0,NULL);
    if(v) fprintf(stderr,"cube rejection returned non-null type=%d layers=%u flags=%u\n",last_view.viewType,last_view.subresourceRange.layerCount,image.cflags);
    assert(!v); assert(view_calls==calls);
    dmsv_bind b={.dim=3,.flags=arr}; VkDescriptorImageInfo ii;
    assert(tex_desc_for(1,&b,1,&ii,0)==0);
    assert(ii.imageView==dummy && dummy_dim==3 && dummy_arr==arr && !dummy_ms && !dummy_storage && !dummy_int);
    assert(ii.sampler==(VkSampler)(uintptr_t)99 && ii.imageLayout==VK_IMAGE_LAYOUT_GENERAL);
    assert(dummy_calls==1 && view_calls==calls);
    // Rejection never caches the incompatible real view (or a dummy as real).
    tex_desc_for(1,&b,1,&ii,0); assert(dummy_calls==2 && lookup_calls==2 && view_calls==calls);
    tex_desc_for(1,&b,1,&ii,1); assert(ii.imageView==dummy && dummy_storage==1 && dummy_dim==3 && dummy_arr==arr);
}
static void accepted(int arr, unsigned base, unsigned count) {
    dmsv_bind b={.dim=3,.flags=arr}; VkDescriptorImageInfo ii;
    tex_desc_for(1,&b,1,&ii,0);
    assert(ii.imageView && ii.imageView!=dummy && !dummy_calls);
    assert(last_view.viewType==(arr?VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:VK_IMAGE_VIEW_TYPE_CUBE));
    assert(last_view.subresourceRange.baseArrayLayer==base && last_view.subresourceRange.layerCount==count);
    assert(last_view.image==image.img && (image.cflags&VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT));
    int calls=view_calls,lookups=lookup_calls; VkImageView saved=ii.imageView;
    tex_desc_for(1,&b,1,&ii,0); assert(ii.imageView==saved && view_calls==calls && lookup_calls==lookups);
    assert(vk_tex_view(&texture,has_subview?&subview:NULL,3,arr,0,NULL)==saved && view_calls==calls);
}
int main(void) {
    // The reported failure: an ordinary 2D view already exists in the cache.
    reset(1,16,16,1,1); assert(vk_tex_view(&texture,NULL,1,0,0,NULL)); rejected(0);
    reset(4,16,16,5,1); rejected(1);
    // Valid 2D-array storage with six layers must also have the creation flag.
    reset(4,16,8,6,1); rejected(0);
    reset(4,16,16,6,4); rejected(1);
    reset(4,16,16,7,1); rejected(0); // this image was not created cube compatible
    reset(3,16,6,1,1); rejected(0); // 1D-array, six layers
    reset(2,16,16,6,1); rejected(0); // 3D depth does not supply cube faces
    reset(4,16,16,12,1); image.cflags&=~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; rejected(1);
    reset(4,16,16,12,1); has_subview=1; subview=(view_t){.set_layers=1,.min_layer=6,.nlayers=5}; rejected(0);
    reset(4,16,16,12,1); has_subview=1; subview=(view_t){.set_layers=1,.min_layer=7,.nlayers=6}; rejected(1);
    reset(4,16,16,12,1); has_subview=1; subview=(view_t){.set_layers=1,.min_layer=0,.nlayers=7}; rejected(1);
    reset(4,16,16,12,1); has_subview=1; subview=(view_t){.set_layers=1,.min_layer=12,.nlayers=6}; rejected(0);
    reset(4,16,16,12,1); has_subview=1; subview=(view_t){.set_layers=1,.min_layer=UINT16_MAX,.nlayers=UINT16_MAX}; rejected(1);
    reset(8,16,16,1,1); accepted(0,0,6);
    reset(9,16,16,12,1); accepted(1,0,12);
    reset(4,16,16,12,1); accepted(0,0,6);
    reset(4,16,16,12,1); accepted(1,0,12);
    reset(4,16,16,12,1); has_subview=1; subview=(view_t){.set_layers=1,.min_layer=6,.nlayers=6,.set_levels=1,.base_level=1,.nlevels=1}; accepted(0,6,6);
    assert(last_view.subresourceRange.baseMipLevel==1 && last_view.subresourceRange.levelCount==1);
    reset(4,16,16,12,1); has_subview=1; subview=(view_t){.set_layers=1,.min_layer=6,.nlayers=0}; accepted(1,6,6);
    // Cube faces are six consecutive layers; base layer need not be a multiple of six.
    reset(4,16,16,12,1); has_subview=1; subview=(view_t){.set_layers=1,.min_layer=1,.nlayers=6}; accepted(1,1,6);
    // Single-cube requests take six faces from a larger selected range.
    reset(4,16,16,12,1); has_subview=1; subview=(view_t){.set_layers=1,.min_layer=1,.nlayers=7}; accepted(0,1,6);
    // A conflicting NVN view target cannot undo the shader's cube type once
    // the backing image and selected range are known to support it.
    reset(4,16,16,12,1); has_subview=1; subview=(view_t){.set_target=1,.target=3}; accepted(0,0,6);
    // Reading a cube face as 2D and rendering into one remain valid.
    reset(8,16,16,1,1); subview=(view_t){.set_layers=1,.min_layer=2,.nlayers=1};
    assert(vk_tex_view(&texture,&subview,1,0,0,NULL)); assert(last_view.viewType==VK_IMAGE_VIEW_TYPE_2D && last_view.subresourceRange.baseArrayLayer==2);
    assert(vk_tex_view(&texture,&subview,3,0,1,NULL)); assert(last_view.viewType==VK_IMAGE_VIEW_TYPE_2D && last_view.subresourceRange.layerCount==1);
    vk_tex_view_drop_all(&image); free(g_views.keys); free(g_views.vals);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src=Path(folder)/'cube.c';src.write_text(code);binary=Path(folder)/'cube'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT),'-I',str(ROOT/'nxnative/dm/gfx/nvn/include'),'-I',str(ROOT/'nxnative/dm/third_party/vulkan-headers/include'),str(src),'-o',str(binary),'-pthread'],check=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)


if __name__ == '__main__':
    unittest.main()
