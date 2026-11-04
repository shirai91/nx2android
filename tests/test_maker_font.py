"""Verify the stand-in font supplies the replacement glyph used during startup."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'nxnative/dm/tools/make_ext_font.py'
LETTER_FONT = Path('/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf')
spec = importlib.util.spec_from_file_location('make_ext_font', TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


@unittest.skipUnless(LETTER_FONT.is_file(), 'requires the macOS stand-in letter font')
class ExtensionFontTest(unittest.TestCase):
    def test_replacement_and_buttons_are_drawable(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / 'extension.ttf'
            module.build(str(output), str(LETTER_FONT))
            with TTFont(output) as font:
                cmap = font.getBestCmap()
                for code in (0xE06B, 0xE0E0, 0xE0E1, 0xE0E6):
                    with self.subTest(code=hex(code)):
                        glyph = cmap[code]
                        self.assertNotEqual(glyph, '.notdef')
                        self.assertGreater(font['glyf'][glyph].numberOfContours, 0)
                        self.assertGreater(font['hmtx'][glyph][0], 0)


if __name__ == '__main__':
    unittest.main()
