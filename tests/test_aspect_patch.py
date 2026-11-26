"""User-provided ultrawide patches: strict matching, syntax and collision checks."""
import ast
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from porter import aspect_patch
from porter.upstream import BuildError


class AspectPatch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nx2a_aspect_')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.dm = self.root / 'dm'
        (self.dm / 'tools').mkdir(parents=True)
        (self.dm / 'games').mkdir()
        (self.dm / 'tools/sdk_patches.py').write_text('CUTS = {}\n')
        self.build = bytes(range(20)).hex()
        # Synthetic NSO, not game content: three uncompressed initialized segments.
        header = bytearray(0x100)
        header[:4] = b'NSO0'
        header[0x40:0x54] = bytes.fromhex(self.build)
        for i in range(3):
            struct.pack_into('<III', header, 0x10 + i * 0x10, 0x100 + i * 16, i * 16, 16)
        struct.pack_into('<III', header, 0x60, 16, 16, 16)
        struct.pack_into('<I', header, 0x3c, 16)
        self.main = self.root / 'main'
        self.main.write_bytes(header + bytes(range(48)))
        self.source = self.root / 'wide.pchtxt'
        self.source.write_text(self.text())
        self.dm_patch = patch.object(aspect_patch, 'DM', self.dm)
        self.dm_patch.start()
        self.addCleanup(self.dm_patch.stop)

    def text(self, writes='00000004 AABBCCDD', build=None):
        return f'@nsobid-{build or self.build}\n@flag print_values\n@flag offset_shift 0x100\n@enabled\n{writes}\n@stop\n'

    def prepare(self):
        dest = aspect_patch.prepare(self.source, self.main, '0123456789abcdef', self.root, '')
        return ast.literal_eval(dest.read_text().split('CUTS = ', 1)[1])[self.build[:32]]

    def test_original_bytes_and_full_build_key(self):
        cuts = self.prepare()
        self.assertEqual(cuts[0][:3], (4, '04050607', 'aabbccdd'))
        self.assertEqual(len(cuts), 1)

    def test_full_build_id_not_just_rewriter_prefix(self):
        self.source.write_text(self.text(build=self.build[:32] + 'ffffffff'))
        with self.assertRaisesRegex(BuildError, 'found 0'):
            self.prepare()

    def test_unsupported_or_incomplete_instructions(self):
        for text in (self.text('00000004 ABC'), self.text('00000004 AABB\n@disabled'), self.text().replace('@stop', ''), self.text().replace('@enabled', '')):
            with self.subTest(text=text):
                self.source.write_text(text)
                with self.assertRaises(BuildError):
                    self.prepare()

    def test_overlapping_user_writes_rejected_even_if_identical(self):
        for writes in ('00000004 AABBCCDD\n00000006 CCDD', '00000004 AABBCCDD\n00000004 AABBCCDD'):
            self.source.write_text(self.text(writes))
            with self.assertRaisesRegex(BuildError, 'overlapping'):
                self.prepare()

    def test_existing_cuts_and_observation_gates_rejected(self):
        for table in (f"CUTS = {{{self.build[:32]!r}: [(4, '04050607', 'aabbccdd', 'existing')]}}", f"OBS = {{{self.build[:32]!r}: [(4, 0, 0, 'gate')]}}"):
            with self.subTest(table=table):
                (self.dm / 'tools/sdk_patches.py').write_text(table)
                with self.assertRaisesRegex(BuildError, 'overlapping'):
                    self.prepare()

    def test_initialized_boundary_and_bss(self):
        self.source.write_text(self.text('0000002F AABB'))
        with self.assertRaisesRegex(BuildError, 'outside initialized'):
            self.prepare()
        self.source.write_text(self.text('00000030 AABB'))
        self.assertEqual(self.prepare()[0][:3], (48, '0000', 'aabb'))

    def test_zip_only_ultrawide_directory_and_matching_build(self):
        self.source = self.root / 'patch [0123456789ABCDEF].zip'
        with zipfile.ZipFile(self.source, 'w') as z:
            z.writestr('60 FPS/wrong.pchtxt', self.text())
            z.writestr('21.9 Ultrawide/old.pchtxt', self.text(build='ff' * 20))
            z.writestr('[21:9 Ultrawide]/wide.pchtxt', self.text())
            z.writestr('21.9 Ultrawide/readme.txt', self.text())
        self.assertEqual(len(self.prepare()), 1)

    def test_zip_ambiguous_or_wrong_title_rejected(self):
        self.source = self.root / 'patch.zip'
        with zipfile.ZipFile(self.source, 'w') as z:
            z.writestr('21.9 Ultrawide/a.pchtxt', self.text())
            z.writestr('21.9 Ultrawide/b.pchtxt', self.text())
        with self.assertRaisesRegex(BuildError, 'found 2'):
            self.prepare()
        self.source = self.root / 'patch [FFFFFFFFFFFFFFFF].zip'
        self.source.touch()
        with self.assertRaisesRegex(BuildError, 'title ID'):
            self.prepare()
