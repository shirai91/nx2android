"""CRI CPK entries and PlatinumGames length-delimited NVN objects."""
import importlib.util, os, struct, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
import cpk


def utf_row(values):
    strings = bytearray(b'table\0'); columns = bytearray(); row = bytearray()
    for name, value in values.items():
        off = len(strings); strings += name.encode() + b'\0'
        columns += struct.pack('>BI', 0x5a if isinstance(value, str) else 0x56, off)
        if isinstance(value, str):
            row += struct.pack('>I', len(strings)); strings += value.encode() + b'\0'
        else: row += struct.pack('>Q', value)
    ro = 0x18 + len(columns); so = ro + len(row); end = so + len(strings)
    return b'@UTF' + struct.pack('>IHHIIIHHI', end, 0, ro, so, end, 0, len(values), len(row), 1) + columns + row + strings


class CpkReader(unittest.TestCase):
    def test_masked_and_plain_toc_stored_entry(self):
        body = b'payload inside the CPK'
        header = utf_row(dict(TocOffset=0x800, ContentOffset=0x1000))
        toc = utf_row(dict(DirName='nested', FileName='file.dat', FileOffset=0x800, FileSize=len(body), ExtractSize=len(body)))
        for masked in (False, True):
            with self.subTest(masked=masked), tempfile.TemporaryDirectory() as tmp:
                data = bytearray(0x1000)
                for offset, magic, table in ((0, b'CPK ', header), (0x800, b'TOC ', toc)):
                    if masked: table = cpk.unmask(table)
                    data[offset:offset + 0x10] = magic + struct.pack('<III', 0, len(table), 0)
                    data[offset + 0x10:offset + 0x10 + len(table)] = table
                path = Path(tmp) / 'data.cpk'; path.write_bytes(data + body)
                entries = cpk.files(path)
                self.assertEqual(entries, [('nested/file.dat', 0x1000, len(body), len(body))])
                self.assertEqual(cpk.read(path, entries[0]), body)

    def test_crilayla_literal_and_backreference(self):
        # Decode ABCABC backwards: three literals, then distance 3 and length 3.
        bits = ''.join('0' + format(x, '08b') for x in b'CBA') + '1' + '0' * 15
        bits += '0' * (-len(bits) % 8)
        packed = bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))[::-1]
        prefix = bytes(range(256))
        self.assertEqual(cpk.crilayla(b'CRILAYLA' + struct.pack('<II', 6, len(packed)) + packed + prefix), prefix + b'ABCABC')


class SizedShader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault('DM_GAME', '0100000000010000')
        spec = importlib.util.spec_from_file_location('split_cpk_under_test', TOOLS / 'shader_split.py')
        cls.s = importlib.util.module_from_spec(spec); spec.loader.exec_module(cls.s)

    def object(self, compute=False):
        code = bytearray(0x100); code[:4] = self.s.CS_MAGIC if compute else self.s.GFX_MAGIC
        struct.pack_into('<I', code, 0x30, 5 << 10)
        ctl = bytearray(0x880); ctl[:4] = self.s.CTL_MAGIC
        struct.pack_into('<I', ctl, 0x20, len(ctl)); struct.pack_into('<I', ctl, 0x704, len(code))
        return struct.pack('<I', 0x88) + bytes(0x84) + struct.pack('<I', len(code)) + code + struct.pack('<I', len(ctl)) + ctl

    def test_graphics_and_compute_scan(self):
        for compute, stage in ((False, 'ps'), (True, 'cs')):
            with self.subTest(stage=stage):
                found = []; self.s.scan_blob(self.object(compute), '/shader', found)
                self.assertEqual([(row[0], row[1]) for row in found], [('/shader#PGSO', stage)])
                self.assertIsNone(self.s.check(*found[0]))

    def test_incomplete_or_unrelated_data_is_not_taken(self):
        d = self.object()
        for other in (b'', bytes(256), d[:-1], d + b'padding', struct.pack('<I', 0xffffffff) + d[4:]):
            self.assertEqual(self.s.pg_sized_shader(other, '/other'), [])


if __name__ == '__main__': unittest.main()
