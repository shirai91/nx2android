"""Image descriptor reuse depends on Vulkan fields, never C structure padding."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class DescriptorHash(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text()
        fnv = source[source.index('static inline uint64_t fnv('):
                     source.index('static void drv_shared(')]
        cache = source[source.index('enum { SETC ='):source.index('static struct { uint64_t h, epoch; VkBuffer b;')]
        start = source.index('    uint64_t h = fnv(0xcbf29ce484222325ull, &s->dsl')
        block = source[start:source.index('    // verify: the same content', start)]
        code = r'''
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static uint64_t g_set_epoch = 1, g_set_hits, g_set_new, g_ns_set_hash, g_ns_set_new;
static int g_tracing, g_x_nosetc, updates;
static struct { VkDevice dev; } V;
static uint64_t tick(void) { return 0; }
static VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    (void)layout; return (VkDescriptorSet)(uintptr_t)(g_set_new + 1);
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t nw,
    const VkWriteDescriptorSet *writes, uint32_t nc, const VkCopyDescriptorSet *copies) {
    (void)device; (void)nw; (void)writes; (void)nc; (void)copies; updates++;
}
''' + cache + fnv + r'''
static VkDescriptorSet cached(VkDescriptorSetLayout layout, VkWriteDescriptorSet *w, uint32_t nw) {
    struct { VkDescriptorSetLayout dsl; } shader = {layout}, *s = &shader;
    VkDescriptorSet set;
    uint64_t tb1 = 0;
''' + block + r'''
    return set;
}
static void reset(void) {
    memset(g_setc, 0, sizeof g_setc);
    g_set_epoch = 1; g_set_hits = g_set_new = updates = 0;
}
static void image_padding(VkDescriptorImageInfo *info, unsigned char byte) {
    unsigned char used[sizeof *info] = {0};
#define FIELD(f) memset(used + offsetof(VkDescriptorImageInfo, f), 1, sizeof info->f)
    FIELD(sampler); FIELD(imageView); FIELD(imageLayout);
#undef FIELD
    for (size_t i = 0; i < sizeof *info; i++) if (!used[i]) ((unsigned char *)info)[i] = byte;
}
static VkWriteDescriptorSet image_write(VkDescriptorImageInfo *images, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        images[i].sampler = (VkSampler)(uintptr_t)(10 + i);
        images[i].imageView = (VkImageView)(uintptr_t)(100 + i);
        images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        image_padding(&images[i], 0x11);
    }
    return (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstBinding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = n, .pImageInfo = images};
}
static void padding_reuse(void) {
    for (unsigned n = 1; n <= 32; n *= 2) {
        reset(); VkDescriptorImageInfo images[32]; VkWriteDescriptorSet w = image_write(images, n);
        VkDescriptorSet first = cached(VK_NULL_HANDLE, &w, 1);
        for (unsigned i = 0; i < n; i++) image_padding(&images[i], 0xa5);
        assert(cached(VK_NULL_HANDLE, &w, 1) == first);
        assert(g_set_new == 1 && g_set_hits == 1 && updates == 1);
        // Reuse is still limited to the submission epoch.
        g_set_epoch++;
        assert(cached(VK_NULL_HANDLE, &w, 1) != first);
        assert(g_set_new == 2 && updates == 2);
    }
}
static void field_changes(void) {
    for (unsigned pos = 0; pos < 32; pos++) for (int field = 0; field < 3; field++) {
        reset(); VkDescriptorImageInfo images[32]; VkWriteDescriptorSet w = image_write(images, 32);
        VkDescriptorSet first = cached(VK_NULL_HANDLE, &w, 1);
        if (field == 0) images[pos].sampler = (VkSampler)(uintptr_t)999;
        if (field == 1) images[pos].imageView = (VkImageView)(uintptr_t)999;
        if (field == 2) images[pos].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorSet second = cached(VK_NULL_HANDLE, &w, 1);
        assert(second != first && updates == 2);
        assert(cached(VK_NULL_HANDLE, &w, 1) == second && g_set_hits == 1);
    }
}
static void other_key_fields(void) {
    for (int field = 0; field < 4; field++) {
        reset(); VkDescriptorImageInfo images[2]; VkWriteDescriptorSet w = image_write(images, 2);
        VkDescriptorSet first = cached(VK_NULL_HANDLE, &w, 1);
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (field == 0) w.dstBinding++;
        if (field == 1) w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if (field == 2) w.descriptorCount = 1;
        if (field == 3) layout = (VkDescriptorSetLayout)(uintptr_t)1;
        assert(cached(layout, &w, 1) != first && updates == 2);
    }
    for (int field = 0; field < 3; field++) {
        reset(); VkDescriptorBufferInfo b = {(VkBuffer)(uintptr_t)1, 64, 128};
        VkWriteDescriptorSet w = {.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1, .pBufferInfo = &b};
        VkDescriptorSet first = cached(VK_NULL_HANDLE, &w, 1);
        if (field == 0) b.buffer = (VkBuffer)(uintptr_t)2;
        if (field == 1) b.offset++;
        if (field == 2) b.range++;
        assert(cached(VK_NULL_HANDLE, &w, 1) != first && updates == 2);
    }
    reset(); VkBufferView view = (VkBufferView)(uintptr_t)1;
    VkWriteDescriptorSet w = {.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        .descriptorCount = 1, .pTexelBufferView = &view};
    VkDescriptorSet first = cached(VK_NULL_HANDLE, &w, 1);
    view = (VkBufferView)(uintptr_t)2;
    assert(cached(VK_NULL_HANDLE, &w, 1) != first && updates == 2);
}
static void texel_array_views(void) {
    // Every view of a texel buffer array is part of the key: two arrays differing only past slot 0 must not share a set.
    for (unsigned pos = 0; pos < 32; pos++) {
        reset(); VkBufferView views[32];
        for (unsigned i = 0; i < 32; i++) views[i] = (VkBufferView)(uintptr_t)(100 + i);
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = 5, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            .descriptorCount = 32, .pTexelBufferView = views};
        VkDescriptorSet first = cached(VK_NULL_HANDLE, &w, 1);
        views[pos] = (VkBufferView)(uintptr_t)999;
        VkDescriptorSet second = cached(VK_NULL_HANDLE, &w, 1);
        assert(second != first && updates == 2);
        assert(cached(VK_NULL_HANDLE, &w, 1) == second && g_set_hits == 1);
    }
    reset(); VkBufferView views[32];
    for (unsigned i = 0; i < 32; i++) views[i] = (VkBufferView)(uintptr_t)(100 + i);
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstBinding = 5, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        .descriptorCount = 32, .pTexelBufferView = views};
    VkDescriptorSet first = cached(VK_NULL_HANDLE, &w, 1);
    assert(cached(VK_NULL_HANDLE, &w, 1) == first && g_set_hits == 1 && updates == 1);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: padding_reuse(); break;
    case 1: field_changes(); break;
    case 2: other_key_fields(); break;
    case 3: texel_array_views(); break;
    default: abort();
    }
}
'''
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_descriptor_hash_')
        cls.addClassCleanup(cls.tmp.cleanup)
        src = Path(cls.tmp.name) / 'test.c'
        cls.exe = Path(cls.tmp.name) / 'test'
        src.write_text(code)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I',
                        str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'),
                        str(src), '-o', str(cls.exe)], check=True, capture_output=True, text=True)

    def run_case(self, case):
        result = subprocess.run([str(self.exe), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_equal_descriptors_reuse_despite_padding(self):
        self.run_case(0)

    def test_each_image_field_and_array_element_changes_the_key(self):
        self.run_case(1)

    def test_other_descriptor_keys_remain_distinct(self):
        self.run_case(2)

    def test_texel_buffer_array_hashes_every_view(self):
        self.run_case(3)


if __name__ == '__main__':
    unittest.main()
