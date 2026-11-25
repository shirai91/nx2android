"""A copy that runs over a chunk's end is done in pieces.

A memory pool over ZC_CHUNK is made of chunks, each with a buffer of its own, so a range that runs
over a chunk's end has no single buffer for it: the copy paths asked for the whole range at once,
found no buffer and dropped the copy whole, with a red line that says "outside the memory pools".
The copies, the fills and the uniform updates now do such a range in pieces, one per chunk, and
the three paths that need one buffer for the whole range (a linear texture's storage, a linear
render target's copy back, the indirect draw arguments) report it instead of dropping it quietly.
"""
from pathlib import Path
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm/android'


class ChunkedCopies(unittest.TestCase):
    def test_the_chunk_of_an_address_is_asked_for(self):
        core = (DM / 'gfx_vk_core.c').read_text(encoding='utf-8')
        self.assertIn('vk_chunk_span', core)                 # the chunk holding an address, and how far it reaches
        header = (DM / 'gfx_vk.h').read_text(encoding='utf-8')
        self.assertIn('uint64_t vk_chunk_span(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);', header)

    def test_the_copies_and_the_fills_go_in_pieces(self):
        core = (DM / 'gfx_vk_core.c').read_text(encoding='utf-8')
        self.assertIn('chunk_span(src + done, n - done, &cs, &co)', core)   # the copy: one piece per chunk, as far as both sides cover
        self.assertIn('chunk_span(dst + done, n - done, &cd, &cod)', core)  # and the other side of it
        self.assertIn('chunk_span(dst + done, n - done, &fb, &fo)', core)   # the fill

    def test_the_paths_that_need_one_buffer_report_it(self):
        tex = (DM / 'gfx_vk_tex.c').read_text(encoding='utf-8')
        self.assertIn('VKRED_ONCE(180', tex)   # a linear texture's storage crosses a chunk boundary
        exec_src = csrc.unit(DM / 'gfx_vk_exec.c')
        self.assertIn('VKRED_ONCE(181', exec_src)   # a linear render target crosses a chunk boundary
        self.assertIn('VKRED_ONCE(182', exec_src)   # the indirect draw arguments cross a chunk boundary

    def test_those_paths_look_the_whole_range_up_first(self):
        # The chunk lookup asks for the chunk holding an address with a one-byte lookup, so with a pool inside a pool (has_inner) it finds the inner pool and comes
        # out short: a range that vk_buf_at found before was refused. The whole range is looked up first, and the chunk span only tells a crossing from a range that
        # is in no pool at all.
        tex = (DM / 'gfx_vk_tex.c').read_text(encoding='utf-8')
        self.assertIn('if (!vk_buf_at((uint64_t)(uintptr_t)t->mem, (uint64_t)len, &b, &o)) {', tex)
        exec_src = csrc.unit(DM / 'gfx_vk_exec.c')
        self.assertIn('if (!vk_buf_at((uint64_t)(uintptr_t)t->mem, len, &b, &o)) {', exec_src)
        self.assertIn('if (!vk_buf_at(ind, len, &sb, &so)) {', exec_src)   # the sparse address is translated again too


if __name__ == '__main__':
    unittest.main()
