"""shader_split.snowdrop_streams: the older Snowdrop SDF slices (South Park: The Fractured But Whole) keep each asset as bare zlib streams of 64 KB of data each, back to back, with
uncompressed data between them. A shader is a control section followed directly by its code area, and one can straddle two chunks of the same asset"""
import importlib.util, os, struct, sys, unittest, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'nxnative/dm/gfx/tools'


def load_split():
    os.environ.setdefault('DM_GAME', '0100000000010000')   # the module reads a profile at import; any one in dm/games does
    sys.path.insert(0, str(TOOLS))
    spec = importlib.util.spec_from_file_location('shader_split_under_test', TOOLS / 'shader_split.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


def shader(stage_bits, tag):
    """A control section of 0x840 bytes and a graphics code area: the shape the game's slices hold"""
    code = bytearray(0x200); code[:4] = bytes.fromhex('78563412'); struct.pack_into('<I', code, 0x30, 0x0001 | stage_bits << 10); code[-4:] = tag
    ctl = bytearray(0x840); ctl[:4] = bytes.fromhex('34127698'); struct.pack_into('<I', ctl, 0x20, len(ctl)); struct.pack_into('<I', ctl, 0x704, len(code))
    return bytes(ctl + code)


def streams(asset, level=6):
    """An asset as the slices store it: one zlib stream per 64 KB of data, with the game's small window"""
    out = b''
    for i in range(0, len(asset), 0x10000):
        c = zlib.compressobj(level, zlib.DEFLATED, 10); out += c.compress(asset[i:i + 0x10000]) + c.flush()
    return out


class SnowdropStreams(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.s = load_split()

    def found(self, d):
        r = self.s.snowdrop_streams(d, '/t.sdfdata')
        self.assertTrue(all('#SNOWDROP@' in w for w, _, _, _ in r))   # single-stage programs (SINGLE_STAGE)
        return sorted((st, code[-4:]) for _, st, _, code in r)

    def test_shaders_between_raw_data(self):
        a = b'reflection' + shader(1, b'VS01') + shader(5, b'PS01') + b'tail'
        d = b'BERG' + bytes(0x34) + streams(a) + bytes(range(256)) * 20 + streams(b'other asset' * 100) + streams(shader(5, b'PS02'))
        self.assertEqual(self.found(d), [('ps', b'PS01'), ('ps', b'PS02'), ('vs', b'VS01')])

    def test_shader_across_a_chunk_boundary(self):   # a run of full 64 KB chunks is one asset
        a = bytes(range(256)) * 253 + shader(1, b'VS02')
        self.assertEqual(self.found(b'BERG' + bytes(0x34) + streams(a)), [('vs', b'VS02')])

    def test_no_streams(self):
        self.assertEqual(self.found(b'BERG' + bytes(range(256)) * 80), [])


if __name__ == '__main__':
    unittest.main()
