# nxnative/dm/gfx/tools/uepak.py: shaders kept inline in cooked Unreal Engine 4.26 / 4.27 data (Tetris Effect: Connected: export bundles of an IoStore container and the pak's global
# shader cache), read as LZ4 blocks, and written as a version 2 shader code archive. Synthetic data, no game files. python -m unittest discover tests
import struct, sys, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from porter.upstream import uepak   # noqa: E402

try: import lz4.block
except ImportError: lz4 = None


def entry(rec, fr, literal=None):
    """One FShaderMapResourceCode entry: i32 length, the LZ4 block, i32 raw size, u8 frequency"""
    blk = lz4.block.compress(rec, store_size=False)
    return struct.pack('<i', len(blk)) + blk + struct.pack('<iB', len(rec), fr)


@unittest.skipIf(lz4 is None, 'lz4 is not installed')
class Inline(unittest.TestCase):
    A = b'LSLG' + bytes(range(1, 200)) * 3          # a record that LZ4 can shrink
    B = b'LSLG' + bytes((i * 37) % 251 for i in range(2000))

    def test_blocks_are_found_between_other_bytes(self):
        d = b'\x07\x00' * 5 + entry(self.A, 0) + b'\xff' * 9 + entry(self.B, 3) + b'LSLGnot a block' + b'\0' * 7
        self.assertEqual(uepak.lz4_inline(d), [(0, self.A), (3, self.B)])

    def test_long_literal_run(self):
        rec = b'LSLG' + bytes((i * 91 + 13) % 256 for i in range(600))   # no repeats: a literal run of more than 15 bytes, so extra length bytes sit before the record
        self.assertEqual(uepak.lz4_inline(entry(rec, 5)), [(5, rec)])

    def test_garbage_is_not_a_shader(self):
        self.assertEqual(uepak.lz4_inline(b'\0' * 50 + b'LSLG' + b'\1' * 40), [])

    def test_archive_roundtrip(self):
        recs = [(0, self.A), (3, self.B), (0, self.A)]   # a duplicate drops out
        b = uepak.inline_archive(recs); a = uepak.archive(b)
        self.assertEqual([(s[2], s[3]) for s in a['shaders']], [(len(self.A), 0), (len(self.B), 3)])
        got = [b[a['code'] + off:a['code'] + off + size] for off, size, _, _ in a['shaders']]
        self.assertEqual(got, [self.A, self.B])


if __name__ == '__main__': unittest.main()
