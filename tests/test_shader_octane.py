"""Octane zips (Toy Story 3 Complete Edition) through synthetic archive bytes: the leading end-of-central-directory layout, deflate and
stored members, the NVN stages of a .tfx effect (each code area right before its control section) and the whole-file path of scan_file."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'


def octzip(members):
    """An Octane zip: EOCD at +0, then the central directory, then each local header and its raw deflate (8) or stored (0) data; CRCs are 0"""
    cd_size = sum(46 + len(n) for n, _, _ in members)
    eocd_len = 22; cd_off = eocd_len; data_off = cd_off + cd_size
    cd = b''; local = b''
    for name, data, meth in members:
        nm = name.encode(); body = data if meth == 0 else zlib.compress(data, 9)[2:-4]
        lho = data_off + len(local)
        cd += struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 20, 20, 0, meth, 0, 0, 0, len(body), len(data), len(nm), 0, 0, 0, 0, 0, lho) + nm
        local += struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, 0, meth, 0, 0, 0, len(body), len(data), len(nm), 0) + nm + body
    eocd = struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(members), len(members), cd_size, cd_off, 0)
    return eocd + cd + local


@unittest.skipUnless(importlib.util.find_spec('lz4'), 'no lz4')
class OctaneShaders(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(TOOLS))
        spec = importlib.util.spec_from_file_location('split_octane_test', TOOLS / 'shader_split.py')
        cls.s = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}): spec.loader.exec_module(cls.s)

    def stage(self, sph_type, fill):
        """code area (0x12345678 header, SPH word 0 with the stage at bits 10..13) immediately followed by its 0x880-byte control section"""
        code = bytearray(0x100); code[:4] = self.s.GFX_MAGIC; struct.pack_into('<I', code, 0x30, sph_type << 10 | 1); code[0x80:] = bytes([fill]) * 0x80
        ctl = bytearray(0x880); ctl[:4] = self.s.CTL_MAGIC; struct.pack_into('<I', ctl, 0x20, len(ctl)); struct.pack_into('<I', ctl, 0x704, len(code))
        return bytes(code) + bytes(ctl)

    def tfx(self):
        # the effect's own header and the PC build's D3D9 bytecode come first; the NVN stages follow
        return bytes.fromhex('45017629') + b'vs_3_0 Microsoft (R) D3DX9 Shader Compiler' + bytes(37) + self.stage(1, 0xa1) + bytes(64) + self.stage(5, 0xb2)

    def test_members_deflate_and_stored(self):
        d = octzip([('effects/a.tfx', b'x' * 300, 8), ('locale/text.txt', b'hello', 0)])
        self.assertEqual(d[:4], self.s.OCTZIP)
        self.assertEqual(list(self.s.octane_members(d, '/t.zip')), [('effects/a.tfx', b'x' * 300), ('locale/text.txt', b'hello')])

    def test_stages_from_a_tfx(self):
        found = self.s.octane_shaders(self.tfx(), '/fx.zip/a.tfx')
        self.assertEqual([st for _, st, _, _ in found], ['vs', 'ps'])
        self.assertTrue(all('#OCT@' in w for w, _, _, _ in found))
        for _, st, ctl, code in found:
            self.assertIsNone(self.s.check('t', st, ctl, code))
        self.assertTrue(all(any(s in w for s in self.s.SINGLE_STAGE) for w, _, _, _ in found))   # programs of one stage: every vertex shader is padded against the pixel cover

    def test_a_control_section_without_its_code_area_stops(self):
        broken = bytearray(self.tfx()); at = broken.find(self.s.CTL_MAGIC); broken[at - 0x100:at - 0xfc] = bytes(4)   # the first stage's code header gone
        with self.assertRaises(SystemExit) as e: self.s.octane_shaders(bytes(broken), '/fx.zip/a.tfx')
        self.assertIn('no code area right before it', str(e.exception))

    def scan(self, d, octane):
        with tempfile.TemporaryDirectory() as tmp, patch.object(self.s, 'OCTANE', octane):
            p = os.path.join(tmp, 'fx_new.zip'); Path(p).write_bytes(d)
            return self.s.scan_file((p, 'assets/effects/fx_new.zip'))[1]

    def test_scan_file_reads_only_the_tfx_members(self):
        uniq = self.scan(octzip([('a.tfx', self.tfx(), 8), ('b.oct', self.stage(1, 0xc3), 8), ('c.tfx', b'no stages', 0)]), True)
        self.assertEqual(sorted(v[1] for v in uniq.values()), ['ps', 'vs'])
        self.assertTrue(all(v[0].startswith('/assets/effects/fx_new.zip/a.tfx#OCT@') for v in uniq.values()))

    def test_off_by_default_other_profiles_scan_as_before(self):
        self.assertFalse(self.s.OCTANE)   # the test profile (Super Mario Odyssey) has no shader_octane row
        self.assertEqual(self.scan(octzip([('a.tfx', self.tfx(), 0)]), False), {})   # not read as an Octane zip; stored data holds no magic the old scan reads

    def test_an_ordinary_zip_is_not_taken(self):
        self.assertNotEqual(b'PK\x03\x04', self.s.OCTZIP)   # an ordinary zip starts with a local header; only an empty one starts with the EOCD
        self.assertEqual(list(self.s.octane_members(octzip([]), '/empty.zip')), [])


if __name__ == '__main__':
    unittest.main()
