# nxnative/dm/gfx/tools/uepak.py find_key: an AES key that main's code stores from two 16-byte literals (ldr q / ldr q / stp q, q), so no window of main holds it
# (Tetris Effect: Connected). Synthetic main and pak, no game files. python -m unittest discover tests
import struct, sys, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from porter.upstream import uepak   # noqa: E402

try: from Crypto.Cipher import AES
except ImportError: AES = None

KEY = bytes(range(0x30, 0x50))   # 32 distinct bytes
MOUNT = struct.pack('<i', 10) + b'../../../\0' + b'\0\0'   # an index block: the mount point FString


def adrp(rd, at, target): imm = (target >> 12) - (at >> 12); return 0x90000000 | (imm & 3) << 29 | (imm >> 2 & 0x7FFFF) << 5 | rd
def ldr_q(rt, rn, off): return 0x3DC00000 | (off // 16) << 10 | rn << 5 | rt
def stp_q(a, b, rn): return 0xAD000000 | b << 10 | rn << 5 | a


def make_pak(path, key):
    cipher = AES.new(key, AES.MODE_ECB).encrypt(MOUNT)
    ft = bytearray(0xDD); ft[20] = 1
    struct.pack_into('<II', ft, 21, uepak.PAK_MAGIC, 11); struct.pack_into('<QQ', ft, 29, 0, 16)
    Path(path).write_bytes(cipher + bytes(ft))


@unittest.skipIf(AES is None, 'pycryptodome is not installed')
class CodeKeys(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory(); self.pak = str(Path(self.d.name) / 'x.pak'); make_pak(self.pak, KEY)
    def tearDown(self): self.d.cleanup()

    def image(self, split):
        lo = 0x3000   # text; the literals live in the page after it, the first half and the second half 0x1000 apart
        words = [0] * (lo // 4)
        code = [adrp(8, 0x100, 0x3000), ldr_q(0, 8, 0x10), adrp(9, 0x108, 0x4000), ldr_q(1, 9, 0x20), stp_q(0, 1, 0)] if split else []
        words[64:64 + len(code)] = code
        img = bytearray(struct.pack(f'<{lo // 4}I', *words)) + bytearray(0x2000)
        if split: img[0x3010:0x3020] = KEY[:16]; img[0x4020:0x4030] = KEY[16:]
        else: img[0x3040:0x3060] = KEY   # a plain contiguous key, as in Octopath Traveler 0
        return bytes(img), lo

    def test_key_from_two_literals(self):
        img, lo = self.image(True)
        self.assertEqual(uepak.code_keys(img, lo), [KEY])
        self.assertEqual(uepak.find_key(img, lo, len(img), self.pak), KEY)

    def test_plain_window_still_found(self):
        img, lo = self.image(False)
        self.assertEqual(uepak.code_keys(img, lo), [])
        self.assertEqual(uepak.find_key(img, lo, len(img), self.pak), KEY)

    def test_no_key(self):
        self.assertIsNone(uepak.find_key(bytes(0x5000), 0x3000, 0x5000, self.pak))


if __name__ == '__main__': unittest.main()
