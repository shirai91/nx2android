"""EA shader archives: nested encrypted/compressed fixtures and rejected corruption."""
import importlib.util
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
import eabig

def big(entries, magic=b'BIG4'):
    size = 16 + sum(9 + len(name.encode()) for name, _ in entries)
    table, body = bytearray(), bytearray()
    for name, data in entries:
        table += struct.pack('>II', size + len(body), len(data)) + name.encode() + b'\0'
        body += data
    return magic + struct.pack('<I' if magic == b'BIG4' else '>I', size + len(body)) + struct.pack('>II', len(entries), size) + table + body

def zipped(data, version, block=80):
    count = (len(data) + block - 1) // block
    out = bytearray(b'chunkzip' + (struct.pack('>8I', 2, len(data), block, count, 16, 0, 0, 0) if version == 2 else struct.pack('>II', len(data), block)))
    for i in range(count):
        if version == 2: out += bytes((((len(out) + 8 + 15) & -16) - 8) - len(out))
        raw = data[i * block:(i + 1) * block]
        z = zlib.compressobj(wbits=-15 if version == 2 else 15); packed = z.compress(raw) + z.flush()
        out += struct.pack('>II', len(packed), 1) if version == 2 else struct.pack('>I', len(packed))
        out += packed
    return bytes(out)

@unittest.skipUnless(importlib.util.find_spec('Crypto'), 'no pycryptodome')
class EaBig(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from Crypto.Cipher import AES
        import hashlib
        cls.key = bytes(range(16)); cls.keyid = b'testkey '
        def envelope(data):
            pad = 16 - len(data) % 16
            return b'EASF' + struct.pack('>I', len(data)) + cls.keyid + hashlib.sha256(data[:32]).digest() + AES.new(cls.key, AES.MODE_CBC, cls.key).encrypt(data + bytes([pad]) * pad)
        cls.envelope = staticmethod(envelope)
        spec = importlib.util.spec_from_file_location('split_eabig_test', TOOLS / 'shader_split.py')
        cls.s = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}): spec.loader.exec_module(cls.s)
        ctl = bytearray(0x880); ctl[:4] = cls.s.CTL_MAGIC; code = cls.s.CS_MAGIC + bytes(0xfc)
        struct.pack_into('<I', ctl, 0x20, len(ctl)); struct.pack_into('<I', ctl, 0x704, len(code))
        h = bytearray(0x50); h[:4] = bytes.fromhex('91688619'); struct.pack_into('<I', h, 0x44, len(h) + len(ctl) + len(code))
        cls.shader = b'NXSHADER' + bytes(h) + bytes(ctl) + code

    def test_directory_members_and_endianness(self):
        for magic in (b'BIG4', b'BIGF'):
            d = big([('empty', b''), ('dir/a', b'test')], magic)
            mm = eabig.members(io.BytesIO(d))
            self.assertEqual([(n, d[o:o + s]) for n, o, s in mm], [('empty', b''), ('dir/a', b'test')])
        self.assertIsNone(eabig.members(io.BytesIO(b'other')))

    def test_bad_directory_bounds_and_names(self):
        d = big([('a', b'data')])
        for raw in (d[:7], d[:-1], d[:25] + b'x' + d[26:], d[:16] + struct.pack('>II', 0, 4) + d[24:]):
            with self.subTest(data=raw), self.assertRaises(ValueError): eabig.members(io.BytesIO(raw))

    def test_chunkzip_versions_and_partial_final_block(self):
        raw = bytes(range(251)) * 3
        for version in (1, 2): self.assertEqual(eabig.chunkzip(zipped(raw, version)), raw)

    def test_chunkzip_corruption_is_rejected(self):
        for version in (1, 2):
            d = zipped(b'repeating input' * 50, version)
            for broken in (d[:12], d[:-1], d + b'junk'):
                with self.subTest(version=version, length=len(broken)), self.assertRaises((ValueError, zlib.error)): eabig.chunkzip(broken)
        d = bytearray(zipped(b'hello' * 30, 2)); struct.pack_into('>I', d, 20, 999)
        with self.assertRaises(ValueError): eabig.chunkzip(d)

    def test_key_comes_from_unaligned_executable_record(self):
        data = self.envelope(zipped(b'hello' * 30, 2))
        img = b'prefix!' + self.keyid + self.key + b'suffix'
        self.assertEqual(eabig.find_key(img, data[:80]), self.key)
        self.assertEqual(eabig.decrypt(data, {self.keyid: self.key}), zipped(b'hello' * 30, 2))
        with self.assertRaises(ValueError): eabig.find_key(img[:-23], data[:80])
        broken = bytearray(data); broken[16] ^= 1
        with self.assertRaises(ValueError): eabig.decrypt(broken, {self.keyid: self.key})
        with self.assertRaises(ValueError): eabig.decrypt(data, {})

    def test_nested_archive_reaches_existing_glslc_reader(self):
        nested = self.envelope(zipped(big([('effect.fxo', self.shader)]), 2))
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'assets.big'; path.write_bytes(big([('texture.bin', b'not a shader'), ('shader.big', nested)]))
            jobs, archives = eabig.jobs([(str(path), 'assets.big')], b'12345' + self.keyid + self.key)
            self.assertEqual(archives, {str(path)}); self.assertEqual(len(jobs), 1)
            rel, found, pairs, extra = self.s.scan_file(jobs[0])
            self.assertEqual(rel, 'assets.big'); self.assertEqual(len(found), 1)
            shader = next(iter(found.values()))
            self.assertEqual(shader[1], 'cs'); self.assertIn('/shader.big/effect.fxo#GLSLC@', shader[0])
            self.assertEqual((pairs, extra), (set(), {}))

    def test_unrelated_members_are_not_selected(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'data.big'; p.write_bytes(big([('image.rx3', b'RX3' + bytes(20))]))
            self.assertEqual(eabig.jobs([(str(p), 'data.big')], b''), ([], {str(p)}))

    def test_only_big_files_are_opened_including_mixed_case_extension(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            other = root / 'not-an-archive.bin'; other.write_bytes(b'BIG4broken')
            archive = root / 'shaders.BiG'; archive.write_bytes(big([('shader.fxo', self.shader)]))
            missing = root / 'not-present.pak'
            jobs, archives = eabig.jobs([(str(p), p.name) for p in (missing, other, archive)], b'')
            self.assertEqual(archives, {str(archive)})
            self.assertEqual([job[:2] for job in jobs], [(str(archive), archive.name)])
            _, found, _, _ = self.s.scan_file(jobs[0])
            self.assertEqual(len(found), 1)

    def test_shared_key_is_discovered_once_per_archive(self):
        payloads = [('first', b'first payload' * 10), ('second', b'second payload' * 10)]
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'shared.big'
            path.write_bytes(big([(name, self.envelope(data)) for name, data in payloads]))
            with patch.object(eabig, 'find_key', wraps=eabig.find_key) as find:
                jobs, _ = eabig.jobs([(str(path), path.name)], self.keyid + self.key)
                self.assertEqual(find.call_count, 1)
            _, selected, keys = jobs[0][2]
            data = path.read_bytes()
            decoded = [list(eabig.leaves(data[off:off + n], name, keys))[0] for name, off, n in selected]
            self.assertEqual(decoded, payloads)

if __name__ == '__main__': unittest.main()
