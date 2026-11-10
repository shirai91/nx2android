# The Xbox layout's icon swap (porter/xbox_icons.py): an Xbox PNG re-encoded into a Switch icon's place byte for byte, the rest of the file untouched, and a file
# that is not the one the table was made from refused. Needs Pillow; no game files.
import hashlib, io, sys, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from porter import xbox_icons   # noqa: E402

try: from PIL import Image
except ImportError: Image = None


def png(color, size=64, noise=False):
    im = Image.new('RGBA', (size, size), color)
    if noise:   # a picture that compresses badly
        im.putdata([((i * 37) % 256, (i * 91) % 256, (i * 13) % 256, 255) for i in range(size * size)])
    b = io.BytesIO(); im.save(b, 'PNG', compress_level=0); return b.getvalue()


@unittest.skipIf(Image is None, 'needs Pillow')
class IconSwap(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix='nx2a_test_icons_')); self.rom = self.dir / 'rom'
        self.switch, self.xbox = png((255, 255, 255, 255)), png((0, 200, 0, 255))
        self.head, self.mid, self.tail = b'H' * 100, b'M' * 50, b'T' * 30
        self.data = self.head + self.switch + self.mid + self.xbox + self.tail
        (self.rom / 'a').mkdir(parents=True); (self.rom / 'a/slice.bin').write_bytes(self.data)

    def table(self, data=None):
        d = data or self.data; o, so = len(self.head), len(self.head) + len(self.switch) + len(self.mid)
        md = lambda a, n: hashlib.md5(d[a:a + n]).hexdigest()
        row = ['a/slice.bin', o, len(self.switch), md(o, len(self.switch)), so, len(self.xbox), md(so, len(self.xbox))]
        t = self.dir / 'xbox_icons.tsv'; t.write_text('# comment\n' + '\t'.join(map(str, row)) + '\t# nx <- durango\n', encoding='utf-8'); return t

    def test_swap_keeps_length_and_the_rest(self):
        out = self.dir / 'out'
        self.assertEqual(xbox_icons.patch(self.rom, self.table(), out), 1)
        d = (out / 'a/slice.bin').read_bytes(); o, n = len(self.head), len(self.switch)
        self.assertEqual(len(d), len(self.data))
        self.assertEqual(d[:o], self.head); self.assertEqual(d[o + n:], self.data[o + n:])
        im = Image.open(io.BytesIO(d[o:o + n])); im.load()
        self.assertEqual(im.convert('RGBA').getpixel((5, 5)), (0, 200, 0, 255))
        self.assertEqual(d[o + n - 8:o + n - 4], b'IEND')

    def test_other_file_is_refused(self):
        t = self.table(); o = len(self.head) + 40   # one byte of the Switch icon differs: another version of the game
        (self.rom / 'a/slice.bin').write_bytes(self.data[:o] + bytes([self.data[o] ^ 1]) + self.data[o + 1:])
        with self.assertRaises(xbox_icons.IconError): xbox_icons.patch(self.rom, t, self.dir / 'out')

    def test_too_big_is_refused(self):
        with self.assertRaises(xbox_icons.IconError): xbox_icons.fit_png(png((0, 0, 0, 255), noise=True), 200)


if __name__ == '__main__': unittest.main()
