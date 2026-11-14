"""porter/romfs_pack.py: the compressed RomFS (android.tsv romfs zstd) that dm/host/hle_fs.c reads back (zf_*)"""
import io, os, random, struct, tempfile, unittest
from pathlib import Path

from porter import romfs_pack as R

Z = R.zstd_mod()


@unittest.skipUnless(Z, 'no compression.zstd (Python 3.14+) or backports.zstd')
class RomfsPack(unittest.TestCase):
    def test_round_trip_and_layout(self):
        rnd = random.Random(1)
        text = b''.join(b'card %d power %d\n' % (i, rnd.randrange(1 << 16)) for i in range(60000))   # compresses well, a partial last block
        packed = R.pack_bytes(text, Z)
        self.assertIsNotNone(packed)
        self.assertEqual(R.unpack_bytes(packed, Z), text)
        magic, ver, raw, block, n, zero = R.HDR.unpack_from(packed)   # the header the host checks (hle_fs.c zhdr)
        self.assertEqual((magic, ver, raw, block, zero), (b'NXZR', 1, len(text), R.BLOCK, 0))
        self.assertEqual(n, (len(text) + R.BLOCK - 1) // R.BLOCK)
        offs = struct.unpack_from(f'<{n + 1}Q', packed, R.HDR.size)
        self.assertEqual(offs[0], R.HDR.size + 8 * (n + 1)); self.assertEqual(offs[-1], len(packed))

    def test_incompressible_is_left_alone(self):
        self.assertIsNone(R.pack_bytes(os.urandom(3 * R.BLOCK), Z))

    def test_stored_block_beside_compressed_ones(self):
        data = bytes(R.BLOCK) * 4 + os.urandom(R.BLOCK)   # four blocks of zeros, one that does not compress: stored as it is
        packed = R.pack_bytes(data, Z)
        offs = struct.unpack_from('<6Q', packed, R.HDR.size)
        self.assertEqual(offs[5] - offs[4], R.BLOCK)
        self.assertEqual(R.unpack_bytes(packed, Z), data)

    def test_streams_one_block_at_a_time(self):
        data = b'abc' * (5 * R.BLOCK)
        class Src(io.BytesIO):
            most = 0
            def read(self, n=-1):
                Src.most = max(Src.most, n if n >= 0 else len(self.getvalue())); return super().read(n)
        out = io.BytesIO()
        self.assertTrue(R.pack_stream(Src(data), len(data), out, Z))
        self.assertLessEqual(Src.most, R.BLOCK)   # a multi-GB archive never sits in memory whole
        self.assertEqual(R.unpack_bytes(out.getvalue(), Z), data)

    def test_tree_reuse_and_links(self):
        with tempfile.TemporaryDirectory() as t:
            src, dst = Path(t, 'RomFS'), Path(t, 'packed', 'v1')
            (src / 'a').mkdir(parents=True)
            (src / 'a/big.bin').write_bytes(b'x' * (2 * R.BLOCK + 5)); (src / 'small.txt').write_bytes(b'hi')
            n, npk, raw, out = R.pack_tree(src, dst, jobs=2)
            self.assertEqual((n, npk), (2, 1)); self.assertTrue(R.is_packed(dst / 'a/big.bin'))
            self.assertEqual((dst / 'small.txt').read_bytes(), b'hi'); self.assertFalse(Path(t, 'packed', 'v1', 'v1.json').exists())   # the state is beside the tree, not in it
            mt = (dst / 'a/big.bin').stat().st_mtime_ns
            R.pack_tree(src, dst, jobs=2)   # unchanged: reused
            self.assertEqual((dst / 'a/big.bin').stat().st_mtime_ns, mt)
            (src / 'small.txt').unlink(); R.pack_tree(src, dst, jobs=2)   # a file the RomFS no longer has goes
            self.assertFalse((dst / 'small.txt').exists())


class RomfsKeyAndInstall(unittest.TestCase):
    def test_profile_key(self):
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'nxnative/dm/android/tools'))
        import android_profile as A
        with tempfile.TemporaryDirectory() as t:
            def prof(row):
                f = Path(t, 'android.tsv'); f.write_text('status\tplayable\npackage\torg.nx2a.x\n' + row, encoding='utf-8')
                import types
                return A.AndroidProfile(types.SimpleNamespace(id='0100000000000000', dir=Path(t)))
            self.assertTrue(prof('romfs\tzstd\n').romfs_zstd)
            self.assertFalse(prof('').romfs_zstd)   # raw, the default
            with self.assertRaises(A.ProfileError): prof('romfs\tlz4\n').check()

    def test_install_knows_a_packed_pack(self):
        from porter import install
        with tempfile.TemporaryDirectory() as t:
            Path(t, 'env.txt').write_text('DM_LANG=en-US\nDM_ROMFS_ZSTD=1\n', encoding='utf-8')
            self.assertTrue(install.packed_romfs(t))
            Path(t, 'env.txt').write_text('DM_LANG=en-US\n', encoding='utf-8')
            self.assertFalse(install.packed_romfs(t))


if __name__ == '__main__':
    unittest.main()
