"""Descriptor collection rejects a full image array before writing past it."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

import host_cc

ROOT = Path(__file__).resolve().parents[1]


def between(text, start, end):
    at = text.index(start)
    return text[at:text.index(end, at)]


@unittest.skipUnless(host_cc.posix(), 'a POSIX host C compiler is required')
class DescriptorCapacity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text(encoding='utf-8')
        header = (ROOT / 'nxnative/dm/android/gfx_vk.h').read_text(encoding='utf-8')
        stage = source[source.index('static VkDescriptorSet stage_set('):]
        declarations = next(line for line in stage.splitlines()
                            if line.strip().startswith('VkWriteDescriptorSet w[96];'))
        cases = between(stage, '        case RK_TEX: case RK_TEX_UBO:',
                        '        case RK_TBUF: case RK_TBUF_UBO:')
        fallback = between(stage, '        default: {   // unknown:',
                           '        wr->pBufferInfo = &bi[nw];')
        # The fallback slice includes the switch's closing brace.
        code = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nxnative/dm/gfx/nvn/nvn_obj.h"
#define VKLOG(...) ((void)0)
''' + between(header, 'enum { RK_SUPPORT', '_Static_assert(sizeof(dmsv_bind)') + between(source, 'static dmsv_bind sampler_element(', '// dofs (at least') + r'''
static struct { uint64_t tex[6][256], img[6][32], samp[6][32], tex_sseq[6][256]; } S;
static uint64_t g_tex_sseq, g_num_um, g_num_im;
// GCC: the harness keeps the address of stage_set's scratch array to check the writes into it, and reads it only while that call runs
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
static VkDescriptorImageInfo *images;
static uint32_t image_capacity, prefix;
static int expect_reject, rejected;
static jmp_buf rejected_jump;
static void untouched_tail(void) {
    const unsigned char *p = (const unsigned char *)(images + prefix);
    for (size_t i = 0; i < (image_capacity - prefix) * sizeof *images; i++) assert(p[i] == 0xa5);
}
static void die(const char *fmt, ...) {
    assert(expect_reject && strstr(fmt, "capacity"));
    untouched_tail();  // A refused binding must not partially fill the remaining entries.
    rejected = 1; longjmp(rejected_jump, 1);
}
static uint64_t vk_tex_slot_handle(const uint64_t *t, const uint64_t *i, unsigned n) {
    (void)t; (void)i; (void)n; return 11;
}
static uint32_t ubo_word(int st, int r, int o) { (void)st; (void)r; (void)o; return 11; }
static uint64_t mem_handle(uint64_t h) { return h; }
static uint64_t c2_handle(int st, int o) { (void)st; (void)o; return 11; }
static int tex_desc_for(int st, const dmsv_bind *b, uint64_t h, VkDescriptorImageInfo *d, int storage) {
    (void)st; (void)b; (void)h;
    assert(d >= images && d < images + image_capacity);
    *d = (VkDescriptorImageInfo){storage ? VK_NULL_HANDLE : (VkSampler)(uintptr_t)2,
                               (VkImageView)(uintptr_t)9, VK_IMAGE_LAYOUT_GENERAL};
    return 0;
}
static VkSampler vk_dummy_sampler(void) { return (VkSampler)(uintptr_t)2; }
static VkSampler vk_samp_by_id(int id) { (void)id; return VK_NULL_HANDLE; }
static int vk_tex_by_id(int id, const view_t **v, const tex_t **t) {
    (void)id; (void)v; (void)t; return 0;
}
static int view_num(const tex_t *t, const view_t *v) { (void)t; (void)v; return 0; }
static VkImageView vk_dummy_view(int dim, int arr, int ms, int storage, int num) {
    (void)dim; (void)arr; (void)ms; (void)storage; (void)num; return (VkImageView)(uintptr_t)9;
}
static VkBuffer vk_zero_buffer(void) { return VK_NULL_HANDLE; }
static VkBufferView vk_dummy_tbuf(int storage) { (void)storage; return VK_NULL_HANDLE; }
static void collect(dmsv_bind *bindings, int nbind, uint32_t want_count, VkDescriptorType want_type) {
''' + declarations + r'''
    (void)bi; (void)bv;
    memset(ii, 0xa5, sizeof ii); images = ii; image_capacity = sizeof ii / sizeof *ii;
    int st = 0, dynslots = 0;
    for (int i = 0; i < nbind && nw < 96; i++) {
        const dmsv_bind *b = bindings + i;
        VkWriteDescriptorSet *wr = &w[nw]; memset(wr, 0, sizeof *wr); wr->descriptorCount = 1;
        switch (b->kind) {
''' + cases + fallback + r'''
        abort();  // Every image case continues; buffer cases are outside this fixture.
    }
    assert(!expect_reject);
    assert(nw == (uint32_t)nbind && nii == prefix + want_count);
    const VkWriteDescriptorSet *last = &w[nw - 1];
    assert(last->descriptorCount == want_count && last->descriptorType == want_type);
    assert(last->pImageInfo == ii + prefix);
    for (uint32_t i = 0; i < want_count; i++) {
        const VkDescriptorImageInfo *d = &last->pImageInfo[i];
        if (want_type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            assert(d->sampler == (VkSampler)(uintptr_t)2 && d->imageView == VK_NULL_HANDLE);
        } else {
            assert(d->imageView == (VkImageView)(uintptr_t)9 && d->imageLayout == VK_IMAGE_LAYOUT_GENERAL);
            assert(d->sampler == (want_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? (VkSampler)(uintptr_t)2 : VK_NULL_HANDLE));
        }
    }
    prefix = nii; untouched_tail();
}
static void run(int kind, int desc, unsigned count, unsigned before, int refuse,
                unsigned want_count, VkDescriptorType want_type) {
    dmsv_bind b[96]; int n = 0;
    for (unsigned left = before; left;) {
        unsigned take = left > 32 ? 32 : left;
        b[n++] = (dmsv_bind){.kind=RK_TEX_C2A, .desc=RD_SAMPLED, .count=(uint16_t)take, .off=-1};
        left -= take;
    }
    b[n++] = (dmsv_bind){.kind=(uint8_t)kind, .desc=(uint8_t)desc, .count=(uint16_t)count, .off=-1};
    prefix = before; expect_reject = refuse; rejected = 0;
    if (!setjmp(rejected_jump)) collect(b, n, want_count, want_type);
    assert(rejected == refuse);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    int mode = atoi(argv[1]);
    int kinds[] = {RK_TEX, RK_TEX_UBO, RK_TEX_C2A, RK_TEXD, RK_SAMPD, RK_IMG, RK_IMG_UBO, RK_UNKNOWN, RK_UNKNOWN};
    int descs[] = {RD_SAMPLED, RD_SAMPLED, RD_SAMPLED, RD_IMAGE, RD_SAMPLER, RD_STORAGE, RD_STORAGE, RD_SAMPLED, RD_STORAGE};
    VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    for (int i = 0; i < 9; i++) {
        int scalar = i == 5 || i == 6;
        if (mode == 0) { // Exact boundary and non-full arrays preserve their full contents.
            run(kinds[i], descs[i], scalar ? 1 : 32, scalar ? 383 : 352, 0, scalar ? 1 : 32, types[i]);
            run(kinds[i], descs[i], 1, 0, 0, 1, types[i]);
            if (!scalar) run(kinds[i], descs[i], 0, 352, 0, 32, types[i]);
        } else if (mode == 1 && i < 3) { // No count-to-one truncation, including implicit count 32.
            run(kinds[i], descs[i], 2, 383, 1, 0, types[i]);
            run(kinds[i], descs[i], 0, 383, 1, 0, types[i]);
            run(kinds[i], descs[i], 1, 384, 1, 0, types[i]);
            run(kinds[i], descs[i], 65535, 0, 1, 0, types[i]);
        } else if (mode == 2 && scalar) run(kinds[i], descs[i], 1, 384, 1, 0, types[i]);
        else if (mode == 3 && i >= 7) {
            run(kinds[i], descs[i], 2, 383, 1, 0, types[i]);
            run(kinds[i], descs[i], 65535, 0, 1, 0, types[i]);
        } else if (mode == 4 && (i == 3 || i == 4)) {
            run(kinds[i], descs[i], 32, 353, 1, 0, types[i]);
            run(kinds[i], descs[i], 33, 0, 1, 0, types[i]);
        }
    }
    return 0;
}
'''
        cls.tmp = tempfile.TemporaryDirectory(prefix='nx2a_descriptor_capacity_')
        cls.addClassCleanup(cls.tmp.cleanup)
        src = Path(cls.tmp.name) / 'test.c'
        cls.exe = Path(cls.tmp.name) / 'test'
        src.write_text(code, encoding='utf-8')
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('NX2A_TEST_SANITIZE') else []
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', *flags,
                        '-I', str(ROOT), '-I', str(ROOT / 'nxnative/dm/gfx/nvn/include'),
                        '-I', str(ROOT / 'nxnative/dm/third_party/vulkan-headers/include'),
                        str(src), '-o', str(cls.exe)], check=True, capture_output=True, text=True)

    def run_case(self, case):
        result = subprocess.run([str(self.exe), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_valid_counts_and_exact_capacity_preserve_descriptors(self):
        self.run_case(0)

    def test_sampled_arrays_reject_before_writing_or_truncating(self):
        self.run_case(1)

    def test_scalar_storage_images_reject_when_full(self):
        self.run_case(2)

    def test_fallback_arrays_reject_without_partial_writes(self):
        self.run_case(3)

    def test_dynamic_arrays_keep_count_and_capacity_limits(self):
        self.run_case(4)


if __name__ == '__main__':
    unittest.main()
