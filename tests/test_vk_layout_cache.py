"""Exercise the runtime layout cache with Vulkan creation/registration stubbed."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402


class LayoutCache(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_layout_identity(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (DM / 'android/gfx_vk_shader.c').read_text()
        helper = csrc.function(src, 'static VkDescriptorSetLayout shader_layout(')
        code = r'''
#include <assert.h>
#include <vulkan/vulkan.h>
#include "nvn_hmap.h"
static struct { VkDevice dev; } V;
static unsigned created, registered;
static VkResult make_layout(VkDevice dev, const VkDescriptorSetLayoutCreateInfo *ci,
                            const VkAllocationCallbacks *alloc, VkDescriptorSetLayout *out) {
    (void)dev; (void)alloc;
    assert(ci->flags == 0 && ci->pNext == NULL && ci->bindingCount <= 96);
    for (uint32_t i = 0; i < ci->bindingCount; i++) assert(ci->pBindings[i].pImmutableSamplers == NULL);
    *out = (VkDescriptorSetLayout)(uintptr_t)++created;
    return VK_SUCCESS;
}
void vk_dsl_register(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutBinding *b, uint32_t n) {
    (void)b; (void)n; assert(layout != VK_NULL_HANDLE); registered++;
}
#define vkCreateDescriptorSetLayout make_layout
#define VKCHK(call) assert((call) == VK_SUCCESS)
''' + helper + r'''
int main(void) {
    VkDescriptorSetLayoutBinding a[2] = {
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_VERTEX_BIT, NULL}
    }, b[2];
    VkDescriptorSetLayout original = shader_layout(a, 2);
    assert(shader_layout(a, 2) == original && created == 1 && registered == 1);
    memset(b, 0xa5, sizeof b);
    for (unsigned i = 0; i < 2; i++) {
        b[i].binding = a[1-i].binding; b[i].descriptorType = a[1-i].descriptorType;
        b[i].descriptorCount = a[1-i].descriptorCount; b[i].stageFlags = a[1-i].stageFlags;
        b[i].pImmutableSamplers = NULL;
    }
    assert(shader_layout(b, 2) == original && created == 1 && registered == 1);
    for (unsigned field = 0; field < 4; field++) {
        memcpy(b, a, sizeof a);
        if (field == 0) b[0].binding++;
        if (field == 1) b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (field == 2) b[0].descriptorCount++;
        if (field == 3) b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        assert(shader_layout(b, 2) != original && created == field + 2 && registered == created);
        assert(shader_layout(b, 2) != original && created == field + 2 && registered == created);
    }
    assert(shader_layout(a, 1) != original);
    VkDescriptorSetLayout empty = shader_layout(NULL, 0);
    assert(empty != original && shader_layout(a, 0) == empty);
    VkDescriptorSetLayoutBinding full[96], reversed[96];
    for (unsigned i = 0; i < 96; i++) { full[i] = a[0]; full[i].binding = i; }
    for (unsigned i = 0; i < 96; i++) reversed[i] = full[95-i];
    assert(shader_layout(full, 96) == shader_layout(reversed, 96));
    assert(created == 8 && registered == created);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='vk-layout-test-') as temp:
            cfile = Path(temp) / 'layout.c'
            exe = Path(temp) / 'layout-test'
            cfile.write_text(code)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-idirafter', str(headers),
                            '-I' + str(DM / 'gfx/nvn'), str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
