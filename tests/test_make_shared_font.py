"""devtools/make_shared_font.py: one static TrueType font from several, the first font's glyphs first."""
import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
try:
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.ttLib import TTFont
except ImportError:   # (fontTools is in requirements.txt)
    FontBuilder = None


def tool():
    spec = importlib.util.spec_from_file_location('make_shared_font', ROOT / 'devtools/make_shared_font.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


def square(size):
    pen = TTGlyphPen(None)
    pen.moveTo((0, 0)); pen.lineTo((0, size)); pen.lineTo((size, size)); pen.lineTo((size, 0)); pen.closePath()
    return pen.glyph()


def ttf(path, upem, chars, width, variable=False):
    names = ['.notdef'] + [f'u{ord(c):04X}' for c in chars]
    fb = FontBuilder(upem, isTTF=True); fb.setupGlyphOrder(names)
    fb.setupCharacterMap({ord(c): f'u{ord(c):04X}' for c in chars})
    fb.setupGlyf({n: square(width) for n in names}); fb.setupHorizontalMetrics({n: (width, 0) for n in names})
    fb.setupHorizontalHeader(ascent=upem * 8 // 10, descent=-upem // 5)
    fb.setupNameTable({'familyName': 'Test', 'styleName': 'Regular'}); fb.setupOS2(); fb.setupPost()
    if variable:
        fb.setupFvar(axes=[('wght', 100, 400, 900, 'Weight')], instances=[]); fb.setupGvar({})
    fb.save(str(path))


def otf(path):
    pen = T2CharStringPen(500, None); pen.moveTo((0, 0)); pen.lineTo((0, 500)); pen.lineTo((500, 0)); pen.closePath()
    cs = pen.getCharString()
    fb = FontBuilder(1000, isTTF=False); fb.setupGlyphOrder(['.notdef', 'A']); fb.setupCharacterMap({0x41: 'A'})
    fb.setupCFF('Test', {'FullName': 'Test'}, {'.notdef': cs, 'A': cs}, {})
    fb.setupHorizontalMetrics({'.notdef': (500, 0), 'A': (500, 0)}); fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': 'Test', 'styleName': 'Regular'}); fb.setupOS2(); fb.setupPost()
    fb.save(str(path))


@unittest.skipUnless(FontBuilder, 'fontTools required')
class MakeSharedFont(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='shared-font-')); self.addCleanup(lambda: __import__('shutil').rmtree(self.tmp))
        self.m = tool()

    def test_latin_first_then_hangul_static(self):
        latin, korean, out = self.tmp / 'latin.ttf', self.tmp / 'kr.ttf', self.tmp / 'out.ttf'
        ttf(latin, 2048, 'Aa', 1100)
        ttf(korean, 1000, 'A한국어', 900, variable=True)   # a Hangul font that also maps 'A', variable like a phone's
        cover, n = self.m.merge([str(latin), str(korean)], str(out))
        f = TTFont(str(out)); cmap = f.getBestCmap()
        self.assertEqual(f['head'].unitsPerEm, 2048)
        self.assertNotIn('fvar', f); self.assertNotIn('gvar', f)
        self.assertTrue(cover['Latin']); self.assertTrue(cover['Hangul']); self.assertFalse(cover['Kanji'])
        self.assertEqual(n, 5)
        self.assertEqual(f['hmtx'][cmap[0x41]][0], 1100)   # 'A' from the first font
        self.assertEqual(f['hmtx'][cmap[ord('한')]][0], 900 * 2048 // 1000)   # Hangul scaled to the first font's em

    def test_refuses_cff_and_collections(self):
        latin, cff = self.tmp / 'latin.ttf', self.tmp / 'cjk.otf'
        ttf(latin, 2048, 'A', 1100); otf(cff)
        with self.assertRaises(self.m.FontError): self.m.merge([str(latin), str(cff)], str(self.tmp / 'out.ttf'))
        with self.assertRaises(self.m.FontError): self.m.load(str(self.tmp / 'fonts.ttc'))

    def test_adb_missing_or_unrunnable_is_a_message(self):
        # --adb uses porter's adb (the Setup tab's download need not be on PATH); none, or one that cannot run, ends with a message, not a traceback
        for found in (None, self.tmp / 'no-such-adb'):
            self.m.find_adb = lambda found=found: found
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(self.m.main(['--adb', '-o', str(self.tmp / 'out.ttf')]), 1)
            self.assertIn('make_shared_font: ', err.getvalue())
            self.assertNotIn('Traceback', err.getvalue())


if __name__ == '__main__':
    unittest.main()
