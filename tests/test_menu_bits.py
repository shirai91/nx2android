# dm_menu_state's mode bits (overlay_layout.h OVT_ST, OVE_ST, OVS_ST): one bit each in 16..19, no two the same, and the Java copies (ExternalActivity) the same
# values. Two pull requests that took the same bit for a new menu mode met only on the device before.
import re, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ANDROID = REPO / 'nxnative/dm/android'


class MenuBits(unittest.TestCase):
    def test_mode_bits_are_distinct_single_bits(self):
        h = (ANDROID / 'overlay_layout.h').read_text(encoding='utf-8')
        bits = {n: int(v, 0) for n, v in re.findall(r'^#define (OV[A-Z]_ST) (0x[0-9a-fA-F]+)', h, re.M)}
        self.assertEqual(set(bits), {'OVT_ST', 'OVE_ST', 'OVS_ST'}, 'a new menu mode gets its own OV?_ST define here')
        for n, v in bits.items():
            self.assertEqual(v & (v - 1), 0, f'{n} is one bit'); self.assertTrue(1 << 16 <= v < 1 << 20, f'{n} is in bits 16..19 (the others carry fields)')
        self.assertEqual(len(set(bits.values())), len(bits), f'two modes share a bit: {bits}')
        java = (ANDROID / 'java/org/nx2a/ExternalActivity.java').read_text(encoding='utf-8')
        self.assertIn(f'(menu & {bits["OVE_ST"]:#x}) != 0 && (menu & {bits["OVT_ST"]:#x}) == 0', java, 'ExternalActivity reads the editor and text bits as overlay_layout.h defines them')

    def test_no_bare_mode_bit_in_the_sources(self):
        for f in ('front_android.c', 'gfx_vk_present.c'):
            s = (ANDROID / f).read_text(encoding='utf-8')
            self.assertNotRegex(s, r'(st|menu) >> 16 & 1|return 1 << 16 \|', f'{f}: use OVT_ST')


if __name__ == '__main__':
    unittest.main()
