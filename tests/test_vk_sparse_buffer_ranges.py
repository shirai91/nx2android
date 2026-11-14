"""Bounds checks for sparse Vulkan buffer bindings and image transfers."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def function_source(source, name):
    start = source.index(f"static int {name}(")
    brace = source.index('{', start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
            if not depth:
                return source[start:end + 1]
    raise AssertionError(f"unterminated function {name}")


class SparseBufferRanges(unittest.TestCase):
    def test_callers_validate_the_extent_the_gpu_can_access(self):
        dm = Path(__file__).resolve().parents[1] / 'nxnative/dm/android'
        core = (dm / 'gfx_vk_core.c').read_text(encoding='utf-8')
        exec_src = (dm / 'gfx_vk_exec.c').read_text(encoding='utf-8')

        self.assertIn('S.vbuf[b].size && nvn_sparse_address_pool(S.vbuf[b].address, 1) ? S.vbuf[b].size : 1', exec_src)
        self.assertIn('nvn_sparse_address_pool(d->ibuf, 1) ? index_bytes : 1', exec_src)
        self.assertIn('vk_buf_at(d->ibuf, index_lookup, &ib, &io)', exec_src)
        self.assertIn('copy_buffer_span(&r, f, depth, &span)', exec_src)
        self.assertIn('nvn_sparse_address_pool(c->src, 1) ? span : 1', exec_src)
        self.assertIn('vk_buf_at(c->src, lookup, &b, &o)', exec_src)
        self.assertIn('else if (!vk_buf_at(c->src, span, &b, &o))', exec_src)
        self.assertIn('vk_ubd_read(d->ibuf, raw_n, raw)', exec_src)
        self.assertIn('uint8_t *raw = malloc((size_t)raw_n)', exec_src)
        self.assertIn('free(raw);', exec_src)
        self.assertIn('if (raw_n > SIZE_MAX)', exec_src)
        self.assertNotIn('if (nq > QUAD_MAX) nq = QUAD_MAX', exec_src)
        self.assertIn('quad_indices(nq * 4, raw,', exec_src)
        self.assertIn('vk_ubd_read(c->src, span, raw)', exec_src)
        self.assertNotIn('(const uint8_t *)(uintptr_t)d->ibuf', exec_src)
        self.assertNotIn('(const uint8_t *)(uintptr_t)c->src', exec_src)
        self.assertIn('sub_gpu_wrote(dst, n)', core)

    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_transfer_span_includes_row_and_image_strides(self):
        source = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_exec.c').read_text(encoding='utf-8')
        helpers = '\n'.join(function_source(source, name) for name in ('copy_span_bytes', 'copy_buffer_span'))
        harness = r'''
#include <assert.h>
#include <stdint.h>
typedef struct { uint32_t bw, bh, bpb; } vkfmt_t;
typedef struct {
    uint32_t bufferRowLength, bufferImageHeight;
    struct { uint32_t width, height, depth; } imageExtent;
} VkBufferImageCopy;
HELPERS
int main(void) {
    uint64_t span = 0;
    vkfmt_t rgba = {1, 1, 4};
    VkBufferImageCopy padded = {16, 4, {10, 3, 1}};
    assert(copy_buffer_span(&padded, &rgba, 2, &span) && span == 424);

    VkBufferImageCopy tight = {0, 0, {10, 3, 1}};
    assert(copy_buffer_span(&tight, &rgba, 1, &span) && span == 120);

    vkfmt_t bc = {4, 4, 16};
    VkBufferImageCopy blocks = {12, 8, {7, 5, 1}};
    assert(copy_buffer_span(&blocks, &bc, 2, &span) && span == 176);

    VkBufferImageCopy short_row = {4, 4, {10, 3, 1}};
    assert(!copy_buffer_span(&short_row, &rgba, 1, &span));
    VkBufferImageCopy short_image = {16, 2, {10, 3, 1}};
    assert(!copy_buffer_span(&short_image, &rgba, 2, &span));

    vkfmt_t huge = {1, 1, UINT32_MAX};
    VkBufferImageCopy overflow = {UINT32_MAX, UINT32_MAX, {UINT32_MAX, UINT32_MAX, 1}};
    assert(!copy_buffer_span(&overflow, &huge, UINT32_MAX, &span));
}
'''.replace('HELPERS', helpers)
        with tempfile.TemporaryDirectory(prefix='nx2a-sparse-buffer-ranges-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness, encoding='utf-8')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
