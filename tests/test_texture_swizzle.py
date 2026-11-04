"""NVN view selectors override texture selectors, retaining format channel mapping."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class TextureSwizzle(unittest.TestCase):
    def test_view_override_format_mapping_and_attachment_identity(self):
        source=(ROOT/'nxnative/dm/android/gfx_vk_tex.c').read_text()
        helper=source[source.index('static VkComponentSwizzle swz_comp'):source.index('__thread VkFormat vk_view_fmt')]
        code=r'''
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <assert.h>
#include <stddef.h>
typedef struct {int set_swz;uint16_t swz;} tex_t;
typedef struct {int set_swizzle;uint8_t swz[4];} view_t;
''' + helper + r'''
static unsigned channel(VkComponentSwizzle sw,unsigned self) {
    const unsigned pixel[]={0,0,255,128,7,255,64};
    return sw==VK_COMPONENT_SWIZZLE_IDENTITY?pixel[self]:pixel[sw];
}
int main(void) {
    VkComponentMapping format={0},out;
    // The parent replicates red. A view selecting green must still read the
    // original green channel, not the parent's replicated red (128 vs 7).
    tex_t t={.set_swz=1,.swz=2|(2<<3)|(2<<6)|(2<<9)};
    view_t v={.set_swizzle=1,.swz={3,3,3,5}};
    out=sample_swizzle(&format,&t,&v,0);
    assert(channel(out.r,VK_COMPONENT_SWIZZLE_R)==7);
    assert(channel(out.g,VK_COMPONENT_SWIZZLE_G)==7);
    assert(channel(out.a,VK_COMPONENT_SWIZZLE_A)==64);
    // An explicit identity view overrides a nonidentity parent too.
    v=(view_t){.set_swizzle=1,.swz={2,3,4,5}};
    out=sample_swizzle(&format,&t,&v,0);assert(out.g==VK_COMPONENT_SWIZZLE_G);
    // With no view selection, the parent's selection is retained.
    v.set_swizzle=0;out=sample_swizzle(&format,&t,&v,0);assert(out.g==VK_COMPONENT_SWIZZLE_R);
    // Format-level remaps/constants are applied once after choosing selectors.
    format=(VkComponentMapping){VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};
    v=(view_t){.set_swizzle=1,.swz={4,0,1,5}};
    out=sample_swizzle(&format,&t,&v,0);
    assert(out.r==VK_COMPONENT_SWIZZLE_R && out.g==VK_COMPONENT_SWIZZLE_ZERO && out.b==VK_COMPONENT_SWIZZLE_ONE && out.a==VK_COMPONENT_SWIZZLE_ONE);
    out=sample_swizzle(&format,&t,&v,1);assert(!out.r&&!out.g&&!out.b&&!out.a);
    t.set_swz=0;out=sample_swizzle(&format,&t,NULL,0);assert(out.r==format.r && out.a==format.a);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src=Path(folder)/'test.c';src.write_text(code);exe=Path(folder)/'test'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'nxnative/dm/third_party/vulkan-headers/include'),str(src),'-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
