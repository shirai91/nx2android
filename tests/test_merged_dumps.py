"""Synthetic dump directories: embedded titles must not disappear without a warning."""
import contextlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from porter import dumps
from porter.upstream import nsp


class MergedDumps(unittest.TestCase):
    BASE = 0x0100000000AAA000

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='nx2a_merged_dump_')
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def dump(self, name, tids):
        # Tickets with different rights-ID suffixes can still name the same title.
        files = [(f'{tid:016x}{i:016x}.tik', bytes(16)) for i, tid in enumerate(tids)]
        def fs(entries):
            names = b''.join(n.encode() + b'\0' for n, _ in entries)
            table, offset, string = b'', 0, 0
            for n, data in entries:
                table += struct.pack('<QQII32sQ', offset, len(data), string, 0, bytes(32), 0)
                offset += len(data)
                string += len(n) + 1
            return (struct.pack('<4sIII', b'HFS0', len(entries), len(names), 0)
                    + table + names + b''.join(d for _, d in entries))
        if Path(name).suffix in ('.xci', '.xcz'):
            header = bytearray(0x200)
            header[0x100:0x104] = b'HEAD'
            struct.pack_into('<Q', header, 0x130, 0x200)
            data = header + fs([('secure', fs(files))])
        else:
            data = (dumps._pfs0_header([n for n, _ in files], [len(d) for _, d in files])
                    + b''.join(d for _, d in files))
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_base_update_and_dlc_warn_in_any_dump_kind_or_order(self):
        tids = [self.BASE, self.BASE + 0x800, self.BASE + 0x1001]
        for ext in ('nsp', 'nsz', 'xci', 'xcz'):
            for order in (tids, tids[::-1]):
                with self.subTest(ext=ext, order=order):
                    path = self.dump('Merged.' + ext, order)
                    with patch.object(nsp, 'say') as say:
                        self.assertEqual(nsp.title_of(path), self.BASE)
                    say.assert_called_once()
                    msg = say.call_args.args[0]
                    self.assertTrue(msg.startswith('[RED] '))
                    self.assertIn(path.name, msg)
                    for tid in tids:
                        self.assertIn(f'{tid:016X}', msg)
                    self.assertIn('not selected as updates or DLC', msg)
                    self.assertIn('separate files', msg)

    def test_duplicate_tickets_are_one_title(self):
        path = self.dump('Duplicate.nsp', [self.BASE, self.BASE])
        with patch.object(nsp, 'say') as say:
            self.assertEqual(nsp.title_of(path), self.BASE)
        say.assert_not_called()

    def test_single_titles_and_filename_fallback_stay_quiet(self):
        for tid in (self.BASE, self.BASE + 0x800, self.BASE + 0x1001):
            with self.subTest(tid=tid), patch.object(nsp, 'say') as say:
                self.assertEqual(nsp.title_of(self.dump('Single.nsp', [tid])), tid)
                self.assertEqual(nsp.title_of(self.dump(f'Tag [{tid:016X}].nsp', [])), tid)
                say.assert_not_called()

    def test_scan_still_selects_a_separate_update(self):
        base = self.dump('Merged.nsp', [self.BASE + 0x800, self.BASE])
        update = self.dump('Update [v262144].nsp', [self.BASE + 0x800])
        with patch.object(nsp, 'say') as say:
            app, fam = nsp.scan_nsps(self.root)
        self.assertEqual(app, self.BASE)
        self.assertEqual(fam['base'], base)
        self.assertEqual(fam['updates'], [(262144, update)])
        self.assertEqual(nsp.nsp_versions(fam, []), {'v0': None, 'v262144': update})
        say.assert_called_once()

    def test_meta_titles_on_cartridge_join_ticket_titles(self):
        # The existing encrypted-header tests cover meta_title; here the base is
        # ticketless while the cartridge carries an update ticket (#156).
        path = self.dump('Card.xci', [self.BASE + 0x800])
        entries = [(f'{self.BASE + 0x800:016x}{0:016x}.tik', 0), ('base.cnmt.nca', 0)]
        with patch.object(nsp, 'dump_files', return_value=entries), \
                patch.object(nsp, 'aes_ecb', return_value=True), \
                patch.object(nsp, 'meta_title', return_value=self.BASE), \
                patch.object(nsp, 'say') as say:
            self.assertEqual(nsp.title_of(path, bytes(32)), self.BASE)
        say.assert_called_once()
        self.assertIn(f'{self.BASE:016X}', say.call_args.args[0])
        self.assertIn(f'{self.BASE + 0x800:016X}', say.call_args.args[0])

    def test_inspect_json_remains_one_document_with_warning(self):
        from porter import cli, config
        base = 0x010043B013C5C000   # a supported profile, with no game data
        for tids, expected_status in (([base, base + 0x800], 0),
                                      ([base + 0x800, base + 0x1001], 2)):
            with self.subTest(tids=tids):
                self.dump('Merged.nsp', tids)
                out, err = io.StringIO(), io.StringIO()
                with patch.object(config, 'load', return_value={}), \
                        contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    status = cli.main(['inspect', str(self.root), '--json'])
                self.assertEqual(status, expected_status)
                info = json.loads(out.getvalue())
                if status == 0:
                    self.assertTrue(info['supported'])
                    self.assertEqual(info['title_id'], f'{base:016X}')
                else:
                    self.assertIn('no base game', info['error'])
                self.assertIn('[RED] Merged.nsp:', err.getvalue())


if __name__ == '__main__':
    unittest.main()
