"""Synthetic dump directories: a merged dump's titles are all used (the update and DLC taken from the base game's file), not dropped."""
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

    def test_base_update_and_dlc_are_all_used_in_any_dump_kind_or_order(self):
        tids = [self.BASE, self.BASE + 0x800, self.BASE + 0x1001]
        for ext in ('nsp', 'nsz', 'xci', 'xcz'):
            for order in (tids, tids[::-1]):
                with self.subTest(ext=ext, order=order):
                    path = self.dump(f'Merged [v262144].{ext}', order)
                    with patch.object(nsp, 'say') as say:
                        app, fam = nsp.scan_nsps(self.root, dumps.EXTS)
                        self.assertEqual(app, self.BASE)
                        self.assertEqual(fam['base'], path)
                        self.assertEqual(fam['updates'], [(262144, path)])
                        self.assertEqual(fam['dlc'], [(self.BASE + 0x1001, None, path)])   # the [v] tag is the update's: the merged dump's DLC gets no version
                        have = nsp.nsp_versions(fam, [])
                        self.assertEqual(have, {'v0': path, 'v262144': path})   # v0 extracts from the merged dump too: nx_extract --merged
                        self.assertEqual(nsp.pick_nsps(fam, have, ['v0', 'v262144']),
                                         {'v0': (path, path), 'v262144': (path, path)})
                    self.assertFalse([c for c in say.call_args_list if c.args[0].startswith('[RED]')], say.call_args_list)
                    path.unlink()

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

    def test_a_merged_dump_without_a_version_tag_is_a_loose_update(self):
        path = self.dump('Merged.nsp', [self.BASE, self.BASE + 0x800])
        with patch.object(nsp, 'say'):
            app, fam = nsp.scan_nsps(self.root)
            self.assertEqual(fam['updates'], [(None, path)])
            self.assertEqual(nsp.nsp_versions(fam, {'v0': {}, 'v196608': {}}), {'v0': path, 'v196608': path})   # taken for the newest registered update, as an untagged update NSP is
            self.assertEqual(nsp.nsp_versions(fam, {'v0': {}}), {'v0': path})   # the profile registers no update: the loose one is left out

    def test_an_untagged_merged_dump_extracts_the_newest_patch_it_holds(self):
        # A retail card dump with an embedded patch carries no [v] tag: nsp_versions takes it for the newest update the profile registers on the name alone, and --merged --version N
        # would stop the port when the card holds another patch. Instead the untagged one is extracted with no --version (nx_extract picks the newest patch in the dump) and the
        # build-ID check decides, as with a separate loose update (the review of #452).
        tagged = self.dump('Merged [v262144].nsp', [self.BASE, self.BASE + 0x800])
        untagged = self.dump('Merged.nsp', [self.BASE, self.BASE + 0x800])
        self.assertEqual(nsp.merged_version(tagged, 'v262144'), 262144)   # a [v] tag: --version selects the patch the tag names
        self.assertEqual(nsp.merged_version(untagged, 'v0'), 0)   # the base game alone, tagged or not
        self.assertIsNone(nsp.merged_version(untagged, 'v196608'))   # taken for the newest registered update on its name alone: no --version
        self.assertEqual(nsp.merged_args('nx', 'keys', untagged, 'out', nsp.merged_version(untagged, 'v196608')),
                         ['nx', 'keys', '--merged', untagged, 'out'])
        self.assertEqual(nsp.merged_args('nx', 'keys', tagged, 'out', nsp.merged_version(tagged, 'v262144')),
                         ['nx', 'keys', '--merged', tagged, 'out', '--version', '262144'])

    def test_an_untagged_merged_update_is_left_out_when_a_tagged_one_is_there(self):
        self.dump('Merged.nsp', [self.BASE, self.BASE + 0x800])
        self.dump('Update [v262144].nsp', [self.BASE + 0x800])
        with patch.object(nsp, 'say') as say:
            app, fam = nsp.scan_nsps(self.root)
            self.assertEqual(nsp.nsp_versions(fam, {'v0': {}, 'v262144': {}}), {'v0': fam['base'], 'v262144': self.root / 'Update [v262144].nsp'})
        self.assertEqual(say.call_count, 1)
        self.assertIn('Merged.nsp', say.call_args.args[0])

    def test_scan_still_selects_a_separate_update(self):
        base = self.dump('Merged [v262144].nsp', [self.BASE, self.BASE + 0x800])
        update = self.dump('Update [v196608].nsp', [self.BASE + 0x800])
        with patch.object(nsp, 'say') as say:
            app, fam = nsp.scan_nsps(self.root)
        self.assertEqual(app, self.BASE)
        self.assertEqual(fam['base'], base)
        self.assertEqual(fam['updates'], [(262144, base), (196608, update)])
        self.assertEqual(nsp.nsp_versions(fam, {'v0': {}, 'v196608': {}}), {'v0': base, 'v262144': base, 'v196608': update})
        say.assert_not_called()

    def test_meta_titles_on_cartridge_beat_its_update_ticket(self):
        # The existing encrypted-header tests cover meta_title; here the base is
        # ticketless while the cartridge carries an update ticket (#156): what its
        # meta NCAs name is what it holds, so it reads as the base game alone.
        path = self.dump('Card.xci', [self.BASE + 0x800])
        entries = [(f'{self.BASE + 0x800:016x}{0:016x}.tik', 0), ('base.cnmt.nca', 0)]
        with patch.object(nsp, 'dump_files', return_value=entries), \
                patch.object(nsp, 'aes_ecb', return_value=True), \
                patch.object(nsp, 'meta_title', return_value=self.BASE), \
                patch.object(nsp, 'say') as say:
            self.assertEqual(nsp.titles_of(path, bytes(32)), [self.BASE])
        say.assert_not_called()

    def test_a_foreign_title_in_a_file_is_skipped_with_a_note(self):
        # A stray ticket of another game (or a compilation card) beside a game's titles: main read only the file's first title and went on, so the scan must not stop with
        # "more than one game" now that every title ID in a file is read (the review of #452).
        other = 0x0100000000BBB000
        path = self.dump('Card.nsp', [self.BASE, self.BASE + 0x800, other])
        with patch.object(nsp, 'say') as say:
            app, fam = nsp.scan_nsps(self.root)
        self.assertEqual(app, self.BASE)
        self.assertEqual(fam, {'base': path, 'updates': [(None, path)], 'dlc': []})
        self.assertEqual(say.call_count, 1)
        self.assertIn(f'{other:016X}', say.call_args.args[0])
        self.assertIn(path.name, say.call_args.args[0])

    def test_extract_aoc_of_a_decompressed_merged_dump_still_tells_the_dumps_apart(self):
        # With NSZ / XCZ DLC, porter hands extract_aoc the decompressed DLC paths — but fam['base'] as scanned is still the compressed file: extract_aoc tells a merged dump from a
        # separate DLC NSP by comparing its paths against the base's, so the base must go through the decompression mapping too. Without it --aoc runs without the DLC's title ID
        # and every DLC index gets the first DLC's RomFS (the review of #452).
        dlc = self.BASE + 0x1001
        compressed = self.dump('Merged [v262144].nsz', [self.BASE, self.BASE + 0x800, dlc])
        unpacked = self.root / 'Merged [v262144].nsp'; unpacked.write_bytes(b'PFS0')   # what dumps.decompress leaves
        from porter.paths import NXNATIVE
        m = {compressed: unpacked}
        fam = {'base': compressed, 'updates': [(262144, compressed)], 'dlc': [(dlc, None, compressed)]}
        def run_stub(cmd, log, **_):
            Path(cmd[4]).parent.mkdir(parents=True, exist_ok=True)   # what nx_extract would create, so swap_in can rename it
        with patch.object(nsp, 'run', side_effect=run_stub) as run, patch.object(nsp, 'PROG'), patch.object(nsp, 'say') as say:
            nsp.extract_aoc('nx', 'keys', NXNATIVE, self.root, self.BASE, dict(fam, base=m.get(fam['base'], fam['base']), dlc=[(t, v, m.get(p, p)) for t, v, p in fam['dlc']]), self.root / 'logs')
            args = run.call_args.args[0]
            self.assertEqual([str(a) for a in args[:4]], ['nx', 'keys', '--aoc', str(unpacked)])
            self.assertEqual(args[4], self.root / 'aoc.partial' / '1')   # index = the DLC's title ID − the base's − 0x1000
            self.assertEqual(args[5], f'{dlc:x}')   # the mapped base: the DLC's title ID, hex — Aoc parses it with fromBase 16
            self.assertIn('the merged dump holds it', say.call_args.args[0])
            nsp.rmtree(self.root / 'aoc'); nsp.rmtree(self.root / 'aoc.key')   # not the reuse cache: the same DLC fingerprints map through to the same decompressed file
            nsp.extract_aoc('nx', 'keys', NXNATIVE, self.root, self.BASE, dict(fam, dlc=[(t, v, m.get(p, p)) for t, v, p in fam['dlc']]), self.root / 'logs')
            self.assertEqual(run.call_args.args[0][5], f'{dlc:x}')   # the title ID is passed whatever the base's path maps to
            self.assertNotIn('the merged dump holds it', say.call_args.args[0])   # the mapped base is what the note still names

    def test_a_dlc_bundles_indices_get_their_own_romfs(self):
        # A DLC-bundle NSP (several DLC titles in one file, no base game): the file is not the base's, so --aoc used to run without the title ID and Aoc fell back to Pick —
        # every index got the same (lowest title ID's) RomFS. The title ID is passed always now; Aoc's pick works for a single-DLC NSP too (its data NCA carries its own ID).
        d1, d2 = self.BASE + 0x1001, self.BASE + 0x1002
        bundle = self.dump('DLC bundle.nsp', [d1, d2])
        base = self.dump('Game.nsp', [self.BASE])
        from porter.paths import NXNATIVE
        def run_stub(cmd, log, **_):
            Path(cmd[4]).parent.mkdir(parents=True, exist_ok=True)   # what nx_extract would create, so swap_in can rename it
        with patch.object(nsp, 'say'), patch.object(nsp, 'PROG'), patch.object(nsp, 'run', side_effect=run_stub) as run:
            app, fam = nsp.scan_nsps(self.root)
            self.assertEqual(app, self.BASE)
            self.assertEqual(fam, {'base': base, 'updates': [], 'dlc': [(d1, None, bundle), (d2, None, bundle)]})
            nsp.extract_aoc('nx', 'keys', NXNATIVE, self.root, app, fam, self.root / 'logs')
            self.assertEqual([c.args[0][3:] for c in run.call_args_list],
                             [[bundle, self.root / 'aoc.partial' / '1', f'{d1:x}'],   # index = the DLC's title ID − the base's − 0x1000, its own title ID, hex
                              [bundle, self.root / 'aoc.partial' / '2', f'{d2:x}']])

    def test_inspect_json_reports_the_merged_dump(self):
        from porter import cli, config
        base = 0x010043B013C5C000   # a supported profile, with no game data
        self.dump(f'Merged [v196608].nsp', [base, base + 0x800, base + 0x1001])
        out, err = io.StringIO(), io.StringIO()
        with patch.object(config, 'load', return_value={}), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = cli.main(['inspect', str(self.root), '--json'])
        self.assertEqual(status, 0)
        info = json.loads(out.getvalue())
        self.assertTrue(info['supported'])
        self.assertEqual(info['title_id'], f'{base:016X}')
        self.assertEqual([u['file'] for u in info['updates']], ['Merged [v196608].nsp'])
        self.assertEqual(info['versions'], ['v0', 'v196608'])
        self.assertNotIn('[RED]', err.getvalue())

    def test_inspect_json_accepts_one_dump_file(self):
        from porter import cli, config
        base = 0x010043B013C5C000   # a supported profile, with no game data
        path = self.dump('Merged [v196608].nsp', [base, base + 0x800, base + 0x1001])
        out, err = io.StringIO(), io.StringIO()
        with patch.object(config, 'load', return_value={}), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = cli.main(['inspect', str(path), '--json'])
        self.assertEqual(status, 0)
        info = json.loads(out.getvalue())
        self.assertTrue(info['supported'])
        self.assertEqual(info['title_id'], f'{base:016X}')
        self.assertEqual([u['file'] for u in info['updates']], [path.name])
        self.assertEqual(info['versions'], ['v0', 'v196608'])
        self.assertNotIn('[RED]', err.getvalue())

    def test_an_update_and_dlc_without_a_base_still_report_no_base_game(self):
        from porter import cli, config
        base = 0x010043B013C5C000
        self.dump('Merged.nsp', [base + 0x800, base + 0x1001])
        out, err = io.StringIO(), io.StringIO()
        with patch.object(config, 'load', return_value={}), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = cli.main(['inspect', str(self.root), '--json'])
        self.assertEqual(status, 2)
        info = json.loads(out.getvalue())
        self.assertIn('no base game', info['error'])


if __name__ == '__main__':
    unittest.main()
