"""Exercise the real device-Oodle stage with synthetic archives and decoder output."""
from pathlib import Path
import hashlib
import shutil
import struct
import sys
import tempfile
import tracemalloc
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from porter import pipeline


class OodleResults(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nx2a-oodle-results-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.port = object.__new__(pipeline.Port)
        self.port.work = self.root / 'work'
        self.port.versions = ['v0']
        self.port.dirs = {'v0': self.root / 'v0'}
        self.port.oodle_why = 'synthetic archives'
        self.result = self.root / 'decoder-result'
        self.pack_entries = []
        def decode(job, out, bnsh=False):
            self.assertTrue(bnsh)
            shutil.copyfile(self.result, out)
            job.unlink()
        self.port.device_oodle_job = decode

    def gfpak(self, name, count, version='v0'):
        rom = self.port.dirs[version] / 'RomFS'; rom.mkdir(parents=True, exist_ok=True)
        data = bytearray(0x20 + 24 * count + count)
        data[:8] = b'GFLXPACK'
        struct.pack_into('<IIQ', data, 0x10, count, 0, 0x20)
        for i in range(count):
            struct.pack_into('<2xHII4xQ', data, 0x20 + 24 * i, 3, 1, 1, 0x20 + 24 * count + i)
        (rom / name).write_bytes(data)
        return len(data)

    def trpfs(self, indices):
        rom = self.port.dirs['v0'] / 'RomFS'; rom.mkdir(parents=True, exist_ok=True)
        (rom / 'archive.trpfs').write_bytes(bytes(32))
        self.pack_entries = [(i, None, 3, 1, 0, 1) for i in indices]

    def results(self, entries):
        with self.result.open('wb') as f:
            f.write(b'DMOR' + struct.pack('<I', len(entries)))
            for data in entries:
                if data is None: f.write(struct.pack('<I', 0xffffffff))
                else: f.write(struct.pack('<I', len(data)) + data)

    def run_stage(self):
        with patch.object(pipeline.trinity, 'packs', return_value=[(16, 0, 0)]), \
             patch.object(pipeline.trinity, 'pack_files', return_value=self.pack_entries), \
             patch.object(pipeline, 'PROG'), patch.object(pipeline, 'say'):
            return Path(self.port.device_oodle()['DM_GFPAK_DECODED'])

    def test_formats_sorted_entries_and_filtered_archives(self):
        size = self.gfpak('a.gfpak', 2)
        empty_size = self.gfpak('empty.gfpak', 1)
        self.trpfs([3, 1, 2])
        self.results([b'BNSH-first', b'BNSH-second', None, b'BNSH-three', None, b'BNSH-two'])
        dec = self.run_stage()
        expected = b'DGFD' + struct.pack('<QI', size, 2)
        for i, data in enumerate((b'BNSH-first', b'BNSH-second')):
            expected += struct.pack('<II', i, len(data)) + data
        self.assertEqual((dec / 'a.gfpak.dec').read_bytes(), expected)
        self.assertEqual((dec / 'empty.gfpak.dec').read_bytes(), b'DGFD' + struct.pack('<QI', empty_size, 0))
        expected = b'DTPD' + struct.pack('<QI', 32, 2)
        for i, data in ((2, b'BNSH-two'), (3, b'BNSH-three')):
            expected += struct.pack('<QII', 16, i, len(data)) + data
        self.assertEqual((dec / 'archive.trpfs.dec').read_bytes(), expected)
        self.assertFalse((dec.parent / 'job.out').exists())

    def test_repeated_version_entries_keep_existing_last_value(self):
        self.port.versions.append('v1'); self.port.dirs['v1'] = self.root / 'v1'
        size = self.gfpak('a.gfpak', 1)
        self.gfpak('a.gfpak', 2, 'v1')
        self.results([b'old', b'new', b'extra'])
        dec = self.run_stage()
        self.assertEqual((dec / 'a.gfpak.dec').read_bytes(), b'DGFD' + struct.pack('<QI', size, 2) +
                         struct.pack('<II', 0, 3) + b'new' + struct.pack('<II', 1, 5) + b'extra')

    def test_large_entry_has_bounded_python_memory(self):
        self.trpfs([0])
        size = 8 << 20
        with self.result.open('wb') as f:
            f.write(b'DMOR' + struct.pack('<II', 1, size) + b'BNSH')
            f.seek(size - 5, 1); f.write(b'\0')
        tracing = tracemalloc.is_tracing()
        if not tracing: tracemalloc.start()
        before = tracemalloc.get_traced_memory()[0]; tracemalloc.reset_peak()
        try:
            dec = self.run_stage()
            _, peak = tracemalloc.get_traced_memory()
        finally:
            if not tracing: tracemalloc.stop()
        self.assertLess(peak - before, 4 << 20, f'8 MiB decoder entry added {peak - before:,} bytes of Python allocations')
        out = dec / 'archive.trpfs.dec'
        self.assertEqual(out.stat().st_size, size + 32)
        with out.open('rb') as f:
            self.assertEqual(f.read(32), b'DTPD' + struct.pack('<QIQII', 32, 1, 16, 0, size))
            actual = hashlib.file_digest(f, 'sha256').digest()
        with self.result.open('rb') as f:
            f.seek(12); expected = hashlib.file_digest(f, 'sha256').digest()
        self.assertEqual(actual, expected)

    def test_truncated_result_does_not_publish_decoded_files(self):
        self.gfpak('a.gfpak', 1)
        header = b'DMOR' + struct.pack('<I', 1)
        for data in (b'', b'DMOR', header, header + b'\x04', header + struct.pack('<I', 8) + b'ab'):
            with self.subTest(data=data):
                self.result.write_bytes(data)
                with self.assertRaises(pipeline.BuildError): self.run_stage()
                self.assertFalse((self.port.work / 'tmp/oodle/decoded').exists())

    def test_failed_record_and_wrong_count_are_rejected(self):
        self.gfpak('a.gfpak', 1)
        self.results([b''])
        with self.assertRaisesRegex(pipeline.BuildError, 'failed on /a.gfpak entry 0'): self.run_stage()
        self.results([None, None])
        with self.assertRaisesRegex(pipeline.BuildError, 'other entries'): self.run_stage()


if __name__ == '__main__': unittest.main()
