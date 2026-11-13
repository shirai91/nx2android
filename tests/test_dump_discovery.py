"""Dump discovery accepts the supported extensions in any case, only on files."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from porter import dumps
from porter.upstream import nsp


class DumpDiscovery(unittest.TestCase):
    BASE = 0x0100000000AAA000

    def setUp(self):
        folder = tempfile.TemporaryDirectory(prefix='nx2a_dump_discovery_')
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)

    def dump(self, folder, name, tid):
        def fs(magic, entry, data):
            names = entry.encode() + b'\0'
            record = struct.pack('<QQII', 0, len(data), 0, 0) + (bytes(0x28) if magic == b'HFS0' else b'')
            return struct.pack('<4sII4x', magic, 1, len(names)) + record + names + data
        ticket = f'{tid:016x}{0:016x}.tik'
        if Path(name).suffix.lower() in ('.xci', '.xcz'):
            header = bytearray(0x200); header[0x100:0x104] = b'HEAD'; struct.pack_into('<Q', header, 0x130, 0x200)
            data = header + fs(b'HFS0', 'secure', fs(b'HFS0', ticket, bytes(16)))
        else: data = fs(b'PFS0', ticket, bytes(16))
        path = folder / name; path.write_bytes(data)
        return path

    def test_all_extensions_preserve_family_and_order_in_any_case(self):
        for ext in dumps.EXTS:
            for suffix in (ext, ext.upper(), ext[0].upper() + ext[1:]):
                with self.subTest(suffix=suffix), tempfile.TemporaryDirectory(dir=self.root) as folder:
                    folder = Path(folder)
                    # Create in reverse filename order: discovery sorts names, while the version remains the filename's value.
                    old = self.dump(folder, 'z-update [v65536].' + suffix, self.BASE + 0x800)
                    dlc = self.dump(folder, 'dlc [v0].' + suffix, self.BASE + 0x1001)
                    base = self.dump(folder, 'base.' + suffix, self.BASE)
                    new = self.dump(folder, 'a-update [v131072].' + suffix, self.BASE + 0x800)
                    app, fam = nsp.scan_nsps(folder, dumps.EXTS)
                    self.assertEqual(app, self.BASE)
                    self.assertEqual(fam, {'base': base, 'updates': [(131072, new), (65536, old)], 'dlc': [(self.BASE + 0x1001, 0, dlc)]})

    def test_directories_and_unrequested_extensions_are_ignored(self):
        base = self.dump(self.root, 'base.nsp', self.BASE)
        for name in ('folder.nsp', 'folder.XCI', 'folder.NsZ', 'folder.xcz'):
            (self.root / name).mkdir()
            self.dump(self.root / name, 'nested.nsp', self.BASE + 0x2000)
        self.dump(self.root, 'other.xci', self.BASE + 0x2000)
        self.dump(self.root, 'notes.txt', self.BASE + 0x2000)
        app, fam = nsp.scan_nsps(self.root, ('nsp',))
        self.assertEqual(app, self.BASE)
        self.assertEqual(fam, {'base': base, 'updates': [], 'dlc': []})


if __name__ == '__main__': unittest.main()
