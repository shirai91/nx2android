"""A LINKDATA archive with an empty DATA1.bin is skipped.

Fire Emblem: Three Houses' DLC patches are LINKDATA archives: DATA0.bin is the index (32-byte
entries {u64 offset, u64 inflated length, u64 stored length, u64 compressed}), DATA1.bin the data
beside it. A patch with no data of its own - every entry empty - has an empty DATA1.bin, and
mapping an empty file fails ("cannot mmap an empty file"): the split has to skip such an archive
instead of reading it, and the file itself holds nothing to find.
"""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'


class Linkdata(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault('DM_GAME', '0100000000010000')
        spec = importlib.util.spec_from_file_location('split_linkdata_under_test', TOOLS / 'shader_split.py')
        cls.split = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.split)

    def archive(self, folder, entries, data=b''):
        """A LINKDATA archive: the index (DATA0.bin) and the data (DATA1.bin)"""
        (folder / 'DATA0.bin').write_bytes(b''.join(struct.pack('<QQQQ', *e) for e in entries))
        (folder / 'DATA1.bin').write_bytes(data)

    def test_an_empty_data_file_has_no_jobs(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            self.archive(folder, [(0, 0, 0, 0)] * 4)   # a patch with no data: its entries are empty
            self.assertIsNone(self.split.linkdata_jobs(str(folder / 'DATA1.bin'), 'aoc/5/DATA1.bin'))

    def test_an_empty_data_file_reads_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            self.archive(folder, [(0, 0, 0, 0)])
            found = []
            self.split.linkdata_shaders(str(folder / 'DATA1.bin'), 'aoc/5/DATA1.bin', (0, 1), found)
            self.assertEqual(found, [])

    def test_an_archive_with_data_is_split_into_jobs(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            self.archive(folder, [(0, 4, 4, 0)], b'\0' * 4)
            jobs = self.split.linkdata_jobs(str(folder / 'DATA1.bin'), 'RomFS/DATA1.bin')
            self.assertIsNotNone(jobs)
            self.assertEqual(jobs[0][2], (0, 1))   # one job for the one entry


if __name__ == '__main__':
    unittest.main()
