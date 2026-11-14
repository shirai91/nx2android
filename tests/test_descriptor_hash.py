"""Image descriptor reuse depends on Vulkan fields, never C structure padding."""
from pathlib import Path
import os
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
        start = source.index('    uint64_t tb1 = tick(); g_ns_set_build += tb1 - tb0;')
        start = source.index('\n', start) + 1
        block = source[start:source.index('    // verify: the same content', start)]
        dynamic_start = source.index('        if (s->dyn[i]) {')
        dynamic = source[dynamic_start:source.index('        nw++;', dynamic_start)]
        code = r'''
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static uint64_t g_set_epoch = 1, g_set_hits, g_set_new, g_set_key_bypass, g_ns_set_hash, g_ns_set_new;
static int g_tracing, g_x_nosetc, updates, force_collision;
static struct { VkDevice dev; } V;
static uint64_t tick(void) { return 0; }
static VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    (void)layout; return (VkDescriptorSet)(uintptr_t)(g_set_new + 1);
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t nw,
    const VkWriteDescriptorSet *writes, uint32_t nc, const VkCopyDescriptorSet *copies) {
    (void)device; (void)nw; (void)writes; (void)nc; (void)copies; updates++;
}
''' + cache + fnv.replace('static inline uint64_t fnv(', 'static inline uint64_t fnv_real(', 1) + r'''
// Inject collisions deterministically; never search for a naturally colliding key.
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    return force_collision ? 0x1234 : fnv_real(h, p, n);
}

static VkDescriptorSet cached(VkDescriptorSetLayout layout, VkWriteDescriptorSet *w, uint32_t nw) {
    struct { VkDescriptorSetLayout dsl; } shader = {layout}, *s = &shader;
    VkDescriptorSet set;
    uint64_t tb1 = 0;
''' + block + r'''
    return set;
}
static void reset(void) {
    memset(g_setc, 0, sizeof g_setc);
    g_set_epoch++; g_set_hits = g_set_new = g_set_key_bypass = updates = 0;
    g_tracing = g_x_nosetc = 0;
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
    for (int field = 0; field < 5; field++) {
        reset(); VkDescriptorImageInfo images[2]; VkWriteDescriptorSet w = image_write(images, 2);
        VkDescriptorSet first = cached(VK_NULL_HANDLE, &w, 1);
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (field == 0) w.dstBinding++;
        if (field == 1) w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if (field == 2) w.descriptorCount = 1;
        if (field == 3) layout = (VkDescriptorSetLayout)(uintptr_t)1;
        if (field == 4) w.dstArrayElement++;
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
static void ownership_and_modes(void) {
    reset(); VkDescriptorImageInfo images[2], original[2];
    VkWriteDescriptorSet w = image_write(images, 2); memcpy(original, images, sizeof images);
    VkDescriptorSet first = cached(VK_NULL_HANDLE, &w, 1);
    // Retaining the caller's pointers would mutate the old key in place.
    images[1].imageView = (VkImageView)(uintptr_t)999;
    VkDescriptorSet changed = cached(VK_NULL_HANDLE, &w, 1); assert(changed != first);
    w.pImageInfo = original;
    VkDescriptorSet restored = cached(VK_NULL_HANDLE, &w, 1); assert(restored != changed);
    assert(cached(VK_NULL_HANDLE, &w, 1) == restored);
    for (int mode = 0; mode < 2; mode++) {
        if (mode) g_tracing = 1; else g_x_nosetc = 1;
        VkDescriptorSet a = cached(VK_NULL_HANDLE, &w, 1), b = cached(VK_NULL_HANDLE, &w, 1);
        assert(a != b); g_tracing = g_x_nosetc = 0;
    }
    g_set_epoch++; assert(cached(VK_NULL_HANDLE, &w, 1) != restored);
    assert(g_set_key_bypass == 0);
}
static VkDescriptorSet normalized_set(VkDescriptorBufferInfo value, uint32_t *offset) {
    struct { int dyn[1]; } shader = {{1}}, *s = &shader;
    struct { uint32_t binding; } binding = {3}, *b = &binding;
    struct { uint32_t b, o; int i; } dp[1]; int nd = 0, i = 0; unsigned nw = 0;
    VkDescriptorBufferInfo bi[1] = {value};
    VkWriteDescriptorSet w[1] = {{.dstBinding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1, .pBufferInfo = bi}}, *wr = w;
#define VKRED_ONCE(...) ((void)0)
''' + dynamic + r'''
#undef VKRED_ONCE
    assert(nd == 1); *offset = dp[0].o; return cached(VK_NULL_HANDLE, w, 1);
}
static void dynamic_offsets(void) {
    reset(); force_collision = 1; uint32_t offset;
    VkDescriptorBufferInfo b = {(VkBuffer)(uintptr_t)1, 64, 128};
    VkDescriptorSet a = normalized_set(b, &offset); assert(offset == 64);
    b.offset = 256; assert(normalized_set(b, &offset) == a && offset == 256);
    b.range = 512; assert(normalized_set(b, &offset) != a && offset == 256);
}
static void large_mixed_key(void) {
    reset(); force_collision = 1;
    VkDescriptorImageInfo images[384]; VkDescriptorBufferInfo buffers[84]; VkWriteDescriptorSet writes[96];
    for (unsigned i = 0; i < 12; i++) { writes[i] = image_write(images + i * 32, 32); writes[i].dstBinding = i; }
    for (unsigned i = 0; i < 84; i++) {
        buffers[i] = (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)(i + 1), 256 * i, 128};
        writes[12 + i] = (VkWriteDescriptorSet){.dstBinding = 12 + i, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1, .pBufferInfo = &buffers[i]};
    }
    VkDescriptorSet a = cached(VK_NULL_HANDLE, writes, 96); assert(cached(VK_NULL_HANDLE, writes, 96) == a);
    images[383].imageView = (VkImageView)(uintptr_t)999;
    VkDescriptorSet b = cached(VK_NULL_HANDLE, writes, 96); assert(b != a && cached(VK_NULL_HANDLE, writes, 96) == b);
    buffers[83].range++; assert(cached(VK_NULL_HANDLE, writes, 96) != b);
}
static void exhausted_storage(void) {
    reset(); force_collision = 1;
    VkDescriptorBufferInfo b = {(VkBuffer)(uintptr_t)1, 0, 64};
    VkWriteDescriptorSet w = {.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1, .pBufferInfo = &b};
    VkDescriptorSet previous = VK_NULL_HANDLE; int bypassed = 0;
    // Replacements must eventually bypass a bounded key store, never alias old content.
    for (unsigned i = 1; i <= 1000000; i++) {
        b.offset = i * 256ull;
        VkDescriptorSet a = cached(VK_NULL_HANDLE, &w, 1), same = cached(VK_NULL_HANDLE, &w, 1);
        assert(a != previous);
        if (same != a) { bypassed = 1; break; }
        previous = a;
    }
    assert(bypassed && g_set_key_bypass == 2);
    // A retained entry still hits even after later misses exhaust the arena.
    b.offset -= 256;
    assert(cached(VK_NULL_HANDLE, &w, 1) == previous && g_set_key_bypass == 2);
    b.offset += 256;
    VkDescriptorSet a = cached(VK_NULL_HANDLE, &w, 1); assert(cached(VK_NULL_HANDLE, &w, 1) != a);
    assert(g_set_key_bypass == 4);
    // Explicitly disabled caching is not an arena-capacity bypass.
    g_x_nosetc = 1; cached(VK_NULL_HANDLE, &w, 1); g_x_nosetc = 0;
    g_tracing = 1; cached(VK_NULL_HANDLE, &w, 1); g_tracing = 0;
    assert(g_set_key_bypass == 4);
    g_set_epoch++; a = cached(VK_NULL_HANDLE, &w, 1);
    assert(cached(VK_NULL_HANDLE, &w, 1) == a && g_set_key_bypass == 4);
}
static void oversized_key(void) {
    reset(); VkDescriptorImageInfo images[4096];
    VkWriteDescriptorSet w = image_write(images, 4096);
    VkDescriptorSet a = cached(VK_NULL_HANDLE, &w, 1);
    assert(cached(VK_NULL_HANDLE, &w, 1) != a); // unrepresentable key: allocate, never trust hash alone
    assert(g_set_key_bypass == 0);
    w.descriptorCount = 32; a = cached(VK_NULL_HANDLE, &w, 1);
    assert(cached(VK_NULL_HANDLE, &w, 1) == a);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: padding_reuse(); break;
    case 1: field_changes(); break;
    case 2: other_key_fields(); break;
    case 3: force_collision = 1; field_changes(); break;
    case 4: force_collision = 1; other_key_fields(); break;
    case 5: force_collision = 1; padding_reuse(); ownership_and_modes(); break;
    case 6: exhausted_storage(); break;
    case 7: oversized_key(); break;
    case 8: dynamic_offsets(); break;
    case 9: large_mixed_key(); break;
    default: abort();
    }
}
'''
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_descriptor_hash_')
        cls.addClassCleanup(cls.tmp.cleanup)
        src = Path(cls.tmp.name) / 'test.c'
        cls.exe = Path(cls.tmp.name) / 'test'
        src.write_text(code)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        *(['-fsanitize=address,undefined', '-g'] if os.environ.get('NX2A_TEST_SANITIZERS') else []), '-I',
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

    def test_colliding_image_fields_and_array_entries_are_distinct(self):
        self.run_case(3)

    def test_colliding_metadata_buffers_and_texel_views_are_distinct(self):
        self.run_case(4)

    def test_owned_content_padding_epoch_and_disabled_modes(self):
        self.run_case(5)

    def test_arena_exhaustion_bypasses_cache_and_epoch_recovers(self):
        self.run_case(6)

    def test_oversized_key_bypasses_cache(self):
        self.run_case(7)

    def test_dynamic_offsets_stay_in_bind_not_static_key(self):
        self.run_case(8)

    def test_maximum_mixed_descriptor_content_is_cacheable_and_exact(self):
        self.run_case(9)


if __name__ == '__main__':
    unittest.main()
