"""shader_split.unsized / frame_size: HAL's size-prefixed .cmp containers (Kirby and the Forgotten Land) are opened, and files that only look like one are left as they are"""
import contextlib, importlib.util, io, os, struct, sys, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'nxnative/dm/gfx/tools'


def load_split():
    os.environ.setdefault('DM_GAME', '0100000000010000')   # the module reads a profile at import; any one in dm/games does
    sys.path.insert(0, str(TOOLS))
    spec = importlib.util.spec_from_file_location('shader_split_under_test', TOOLS / 'shader_split.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


@unittest.skipUnless(importlib.util.find_spec('lz4'), 'no lz4')
class CmpContainer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.s = load_split()
        cls.zstd = cls.s.zstd_mod()
        cls.body = b'BNSH' + bytes(range(256)) * 40   # 10,244 bytes: a 2-byte size field (counts from 256) in a single-segment zstd frame

    def unsized(self, d):
        out = io.StringIO()
        with contextlib.redirect_stdout(out): r = self.s.unsized(d, '/t.cmp')
        return r, out.getvalue()

    def test_zstd_frame(self):
        f = self.zstd.compress(self.body)
        self.assertEqual(self.s.frame_size(f), len(self.body))
        self.assertEqual(self.unsized(struct.pack('<I', len(self.body)) + f), (self.body, ''))

    def test_zstd_frame_without_a_size(self):   # a streamed frame records no size: the prefix decides
        c = self.zstd.ZstdCompressor(); f = c.compress(self.body) + c.flush()
        if self.s.frame_size(f) is not None: self.skipTest('this zstd build records the size in streamed frames')
        self.assertEqual(self.unsized(struct.pack('<I', len(self.body)) + f), (self.body, ''))

    def test_lz4_frame(self):   # the ubin/ map data: LZ4 frames, with and without a content size
        import lz4.frame
        for flag in (True, False):
            f = lz4.frame.compress(self.body, store_size=flag)
            self.assertEqual(self.s.frame_size(f), len(self.body) if flag else None)
            self.assertEqual(self.unsized(struct.pack('<I', len(self.body)) + f), (self.body, ''))

    def test_other_size_is_not_a_container(self):   # Tears of the Kingdom's *.quad: a hash word, then a zstd frame of another size: left alone, no red line
        d = struct.pack('<I', 0x9e3779b9) + self.zstd.compress(self.body)
        self.assertEqual(self.unsized(d), (d, ''))

    def test_damaged_frame_is_red_and_kept(self):
        f = bytearray(self.zstd.compress(self.body)); f[-6] ^= 0xff
        d = struct.pack('<I', len(self.body)) + bytes(f)
        r, said = self.unsized(d)
        self.assertEqual(r, d); self.assertIn('[RED] /t.cmp', said)

    def test_wrong_prefix_without_a_frame_size_is_red(self):
        c = self.zstd.ZstdCompressor(); f = c.compress(self.body) + c.flush()
        if self.s.frame_size(f) is not None: self.skipTest('this zstd build records the size in streamed frames')
        d = struct.pack('<I', len(self.body) + 1) + f
        r, said = self.unsized(d)
        self.assertEqual(r, d); self.assertIn('bytes where the prefix gives', said)


if __name__ == '__main__':
    unittest.main()
