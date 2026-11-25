"""shader_split: one file no reader can read is left out with a [RED] line and the split goes on; archives in archives are bounded (NEST); cut or out-of-range data red()s
with where it is. A reader's red() is a SystemExit and an exception ends the process pool, so one such file (one of Mario Strikers: Battle League's 879 Next Level Games
archives) used to end the whole split with nothing written; a SARC member spanning its own archive was scanned about 1,000 levels deep. No game files needed.
Adapted from seedds/nxnative's test_split_unreadable.py and test_split_nesting.py (MIT)."""
import contextlib
import importlib.util
import io
import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'nxnative/dm/gfx/tools'
GAME = '0100000000010000'   # shader_split reads a profile at import; any one in dm/games does


def load_split():
    sys.path.insert(0, str(TOOLS))
    with mock.patch.dict(os.environ, {'DM_GAME': GAME}):
        spec = importlib.util.spec_from_file_location('shader_split_unreadable_under_test', TOOLS / 'shader_split.py')
        m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


ss = load_split()


def stage(sph_type):
    """(control section, code area) of one stage: control +0x20 its length, +0x704 the code area's; the code area a 0x30 header, the SPH (word 0: stage in bits 10..13), a few words"""
    code = bytearray(ss.GFX_MAGIC + bytes(0x2c)); code += struct.pack('<I', sph_type) + bytes(0x4c); code += bytes.fromhex('0f00000000000000') * 8
    ctl = bytearray(0x880); ctl[:4] = ss.CTL_MAGIC; struct.pack_into('<I', ctl, 0x20, 0x880); struct.pack_into('<I', ctl, 0x704, len(code))
    return bytes(ctl), bytes(code)


VS, PS = stage(0x0461), stage(0x1462)


def bnsh(tag=0):
    """A BNSH of one variation with a vertex and a pixel stage; tag changes the code (another key)"""
    d = bytearray(0x300); d[:8] = b'BNSH\0\0\0\0'; d[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', d, 0x7c, 1, 0x100)              # one variation, its table at 0x100
    struct.pack_into('<Q', d, 0x110, 0x140)                 # variation 0: the binary program at 0x140
    for si, desc, (ctl, code) in ((0, 0x200, VS), (4, 0x240, PS)):
        code = code[:-8] + struct.pack('<Q', tag)
        struct.pack_into('<Q', d, 0x148 + 8 * si, desc)     # stage slots from program +8: vertex 0, pixel 4
        at = len(d); d += ctl + code
        struct.pack_into('<QQII', d, desc + 8, at, at + len(ctl), len(code), len(ctl))
    struct.pack_into('<I', d, 0x1c, len(d))
    return bytes(d)


def sarc(members):
    names = b''.join(n + b'\0' + bytes(-(len(n) + 1) % 4) for n, _ in members)
    sfat = b'SFAT' + struct.pack('<HHI', 0xc, len(members), 0x65)
    data, at, offs = b'', 0, []
    for n, m in members:
        offs.append(at); data += m + bytes(-len(m) % 0x100); at = len(data)
    noff = 0
    for (n, m), o in zip(members, offs):
        sfat += struct.pack('<IIII', 0, 0x01000000 | noff // 4, o, o + len(m)); noff += len(n) + 1 + (-(len(n) + 1) % 4)
    head = 0x14 + len(sfat) + 8 + len(names); doff = (head + 0xff) & ~0xff
    d = b'SARC' + struct.pack('<HHII', 0x14, 0xfeff, doff + len(data), doff) + bytes(4) + sfat + b'SFNT' + struct.pack('<HH', 8, 0) + names
    return d + bytes(doff - len(d)) + data


def yaz0(b):
    """Yaz0 that writes each run of one byte as back-references of distance 1 (at most 0x111 bytes each) and the rest as literals"""
    out = bytearray(b'Yaz0' + len(b).to_bytes(4, 'big') + bytes(8)); i = 0
    while i < len(b):
        head = len(out); out.append(0)
        for bit in range(8):
            if i >= len(b): break
            run = 0
            while i and i + run < len(b) and run < 0x111 and b[i + run] == b[i - 1]: run += 1
            if run >= 0x12: out += bytes([0, 0, run - 0x12])
            elif run >= 3: out += bytes([(run - 2) << 4, 0])
            else: out[head] |= 0x80 >> bit; out.append(b[i]); run = 1
            i += run
    return bytes(out)


def fanout(member, n=8, pad=0):
    """A SARC whose n members all span the same bytes, then pad zero bytes"""
    head = 0x14 + 0xc + 16 * n + 8; doff = (head + 0xff) & ~0xff
    d = b'SARC' + struct.pack('<HHII', 0x14, 0xfeff, doff + len(member) + pad, doff) + bytes(4) + b'SFAT' + struct.pack('<HHI', 0xc, n, 0x65)
    d += struct.pack('<IIII', 0, 0, 0, len(member)) * n + b'SFNT' + struct.pack('<HH', 8, 0)
    return d + bytes(doff - len(d)) + member + bytes(pad)


def outside(d):
    """the SARC with its first member's end moved far past the archive"""
    d = bytearray(d); struct.pack_into('<I', d, 0x14 + 0xc + 12, 0x100000); return bytes(d)


def far(d):
    """the BNSH with its variation table moved far past its end"""
    d = bytearray(d); struct.pack_into('<IQ', d, 0x7c, 1, 0x100000); return bytes(d)


def index(nchunk, nrec, recs=None, chunks=None, names=b'.data\0'):   # nlg_dict's layout: header, 0x18-byte records, 0x10-byte chunks, names
    d = ss.NLG_DICT + bytes([4, 1, 1, 0]) + struct.pack('<I', 0x1000) + bytes([nchunk, nrec, names.count(b'\0'), 0])
    for r in (recs if recs is not None else [bytes(16)] * nrec): d += struct.pack('<IHH', 0, 0, 0) + r
    for c in (chunks if chunks is not None else [(0, 0, 0, 0)] * nchunk): d += struct.pack('<3IHBB', *c[:3], 0, c[3], 0)
    return d + names


class NlgLayout(unittest.TestCase):
    def test_known_layout_reads(self):
        self.assertEqual(ss.nlg_dict(index(2, 1, [bytes([0, 1]) + bytes(14)]), '/a.dict'), (1, [(0, [0, 1] + [0] * 14)], [(0, 0, 0, 0), (0, 0, 0, 0)], ['.data']))

    def test_other_layouts_are_named(self):
        for name, d in (('cut short', index(5, 2)[:0x28]), ('header only', ss.NLG_DICT + bytes(4)),
                        ('section chunk out of range', index(2, 1, [bytes([0, 2]) + bytes(14)])), ('more records than chunks', index(1, 2))):
            with self.subTest(name), self.assertRaisesRegex(SystemExit, r"^\[RED\] /a\.dict: the index .* is not in the layout of Luigi's Mansion 3's archives"):
                ss.nlg_dict(d, '/a.dict')


class ScanFile(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(lambda: __import__('shutil').rmtree(self.tmp))

    def scan(self, path, rel):
        out = io.StringIO()
        with contextlib.redirect_stdout(out): r = ss.scan_file((str(path), rel))
        return r, out.getvalue()

    def test_red_line_of_a_reader(self):
        p = self.tmp / 'a.dict'; p.write_bytes(index(5, 2)[:0x28])
        (rel, keys, pairs, extra), out = self.scan(p, 'sub/a.dict')
        self.assertEqual((rel, keys, pairs, extra[0]), ('sub/a.dict', {}, set(), 'unreadable'))
        self.assertTrue(extra[1].startswith('/sub/a.dict: the index'))
        self.assertEqual(out.count('/sub/a.dict'), 1); self.assertTrue(out.startswith('[RED] /sub/a.dict: the index') and out.endswith('(its shaders are left out)\n'))

    def test_exception_of_a_reader(self):
        p = self.tmp / 'a.dict'; p.write_bytes(index(1, 1))
        with mock.patch.object(ss, 'nlg_shaders', side_effect=IndexError('list index out of range')):
            (_, _, _, extra), out = self.scan(p, 'a.dict')
        self.assertEqual(extra, ('unreadable', 'a.dict: IndexError: list index out of range'))
        self.assertEqual(out, '[RED] a.dict: IndexError: list index out of range (its shaders are left out)\n')

    def test_interrupt_is_not_taken(self):
        p = self.tmp / 'a.dict'; p.write_bytes(index(1, 1))
        with mock.patch.object(ss, 'nlg_shaders', side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt): self.scan(p, 'a.dict')

    def test_readable_file_is_unchanged(self):
        p = self.tmp / 'a.bnsh'; p.write_bytes(bnsh())
        (rel, keys, pairs, extra), out = self.scan(p, 'a.bnsh')
        self.assertEqual((rel, len(keys), len(pairs), extra, out), ('a.bnsh', 2, 1, {}, ''))


class Split(unittest.TestCase):
    """The whole split: the unreadable files are reported once more at the end, the tables are written, the split exits 0, and the other files' rows are those of a split without them"""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(lambda: __import__('shutil').rmtree(self.tmp))
        self.main = self.tmp / 'main'; self.main.write_bytes(bytes(0x100))   # an NSO header of three empty segments

    def split(self, files, name):
        rom = self.tmp / name / 'RomFS'
        for rel, data in files.items(): (rom / rel).parent.mkdir(parents=True, exist_ok=True); (rom / rel).write_bytes(data)
        out = [self.tmp / name / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv')]
        r = subprocess.run([sys.executable, str(TOOLS / 'shader_split.py'), str(rom), str(self.main), *map(str, out)], capture_output=True, text=True,
                           env=dict(os.environ, DM_GAME=GAME, J='2'))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        for p in out: self.assertTrue(p.exists(), p)
        rows = [ln for ln in out[1].read_text().splitlines() if ln and not ln.startswith(('#', 'package\t'))]
        return r.stdout, rows, out[3].read_text(), {f.name: f.read_bytes() for f in sorted(out[0].iterdir())}

    def test_split_goes_on(self):
        good = {'a/one.bnsh': bnsh(1), 'b/two.sarc': sarc([(b'x.bnsh', bnsh(2))]), 'c.bin': b'no shaders'}
        bad = {'sub/a.dict': index(5, 2)[:0x28], 'sub/a.data': bytes(64),                                   # an index of another layout
               'bad/cut.szs': yaz0(sarc([(b'x.bnsh', bnsh(3))]))[:0x40],                                   # a Yaz0 stream cut short
               'bad/out.sarc': outside(sarc([(b'x.bnsh', bnsh(5))])),                                       # a member outside its archive
               'bad/ptr.bnsh': far(bnsh(4)),                                                               # a BNSH whose variation table lies past its end
               # nxnative's fuzz input (40 members of one inner SARC, 6 deep); the BNSH magic after it makes the split's prefilter open it
               'bad/fanout.sarc': (ROOT / 'tests/fixtures/shader_split/sarc-fanout.bin').read_bytes() + b'BNSH\0\0\0\0'}
        log0, rows0, pairs0, pkg0 = self.split(good, 'clean')
        log1, rows1, pairs1, pkg1 = self.split({**good, **bad}, 'damaged')
        self.assertNotIn('[RED]', log0)
        self.assertEqual((rows1, pairs1, pkg1), (rows0, pairs0, pkg0))
        self.assertEqual(len(rows0), 4, rows0)
        self.assertIn('\n[RED] 5 file(s) could not be read, their shaders are left out (first: ', '\n' + log1)
        for rel in ('sub/a.dict', 'bad/cut.szs', 'bad/out.sarc', 'bad/ptr.bnsh', 'bad/fanout.sarc'):
            self.assertEqual(len([ln for ln in log1.splitlines() if ln.startswith('[RED] /' + rel) and ln.endswith('(its shaders are left out)')]), 1, (rel, log1))


class SplitShaders(unittest.TestCase):
    """build_ipa_from_nsp.split_shaders reads the split's summary line: a provisional profile goes on without those shaders and says so, a registered one stops (its tables
    were split from every file, so a file that no longer reads is a reader change)"""
    LOG = '[RED] /a.dict: the index ... (its shaders are left out)\n[RED] 2 file(s) could not be read, their shaders are left out (first: /a.dict: the index)\nshaders: 3 (vertex 3)\n'

    def setUp(self):
        sys.path.insert(0, str(ROOT))
        from porter.upstream import nsp
        self.nsp = nsp; self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(lambda: __import__('shutil').rmtree(self.tmp))
        (self.tmp / 'logs').mkdir(); (self.tmp / 'profile').mkdir()
        self.g = mock.Mock(id='0100000000000000', table=lambda n: self.tmp / 'profile' / n)
        for n in ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv'): (self.tmp / 'profile' / n).write_text('# old\nrow\n')
        for name, value in (('PROG', mock.Mock()), ('fingerprint', mock.Mock(return_value='key')), ('tool_files', mock.Mock(return_value=[])),
                            ('profile_files', mock.Mock(return_value=[])), ('run', mock.Mock(side_effect=self.split))):
            m = mock.patch.object(nsp, name, value); m.start(); self.addCleanup(m.stop)

    def split(self, cmd, log, env=None, **kw):
        (Path(cmd[4]) / 'x.bin').parent.mkdir(parents=True); (Path(cmd[4]) / 'x.bin').write_bytes(b'')
        for t in cmd[5:]: Path(t).write_text('# new\nrow\n')
        Path(log).write_text(self.LOG)

    def go(self, provisional):
        return self.nsp.split_shaders(sys.executable, ROOT / 'nxnative', [self.tmp / 'RomFS'], [self.tmp / 'main'], self.tmp / 'pkg', {}, self.tmp / 'logs', self.g, provisional)

    def test_provisional_goes_on(self):
        with mock.patch.object(self.nsp, 'say') as say: self.go(True)
        self.assertTrue(say.call_args[0][0].startswith('[RED] 2 file(s) could not be read'))
        self.assertIn('2 file(s) unreadable', self.nsp.PROG.end.call_args[0][0])

    def test_registered_stops(self):
        with self.assertRaisesRegex(self.nsp.BuildError, r'^2 file\(s\) could not be read'): self.go(False)
        self.assertFalse((self.tmp / 'pkg').exists())


class Nesting(unittest.TestCase):
    """The budgets of archives in archives (scan_blob, NEST): a Yaz0 member that expands is not overlap (Mario Kart 8 Deluxe's UI/<lang>/race.sarc, one layout archive that
    expands 9 times, was left out in nxnative), while members that overlap and Yaz0 members that expand without end are stopped early"""
    def scan(self, d):
        out = []
        with mock.patch.object(ss, 'scan_blob', wraps=ss.scan_blob) as calls: ss.scan_blob(d, '/UI/EUde/race.sarc', out)
        return out, calls.call_count

    def test_self_nesting_is_stopped(self):   # a SARC member spanning its whole archive: scanned as the archive again until RecursionError before
        d = bytearray(fanout(b'', n=1)); struct.pack_into('<II', d, 0x14 + 0xc + 8, 0, len(d) - 0x100)
        doff = struct.unpack_from('<I', d, 0xc)[0]; struct.pack_into('<I', d, 0xc, 0)   # the data starts at the header: the member is the archive
        with self.assertRaisesRegex(SystemExit, r'SARC data at 0x0'): self.scan(bytes(d))
        self.assertEqual(doff, 0x100)

    def test_deep_nesting_is_stopped(self):
        d = bnsh()
        for i in range(ss.NEST + 2): d = sarc([(b'n%d' % i, d)])
        with self.assertRaisesRegex(SystemExit, f'archives nested more than {ss.NEST} deep'): self.scan(d)

    def test_compressed_member_is_not_overlap(self):
        inner = sarc([(b'shader.bnsh', bnsh())] + [(b'lyt%d.bflyt' % i, bytes(0x4000)) for i in range(8)])
        packed = yaz0(inner)
        outer = sarc([(b'rc_L_Rank_00.szs', packed), (b'pad.bin', b'\1' * (len(inner) // 9 - len(packed)))])
        self.assertTrue(8 * len(outer) < len(inner) < 10 * len(outer))
        out, _ = self.scan(outer)
        self.assertEqual({w.split('#')[0] for w, *_ in out}, {'/UI/EUde/race.sarc/rc_L_Rank_00.szs/shader.bnsh'})

    def test_overlapping_members_are_stopped(self):
        d = (ROOT / 'tests/fixtures/shader_split/sarc-fanout.bin').read_bytes()
        with self.assertRaisesRegex(SystemExit, r'members add up to more than 16 times the file \(members that overlap\)'): self.scan(d)

    def test_compressed_overlapping_members_are_stopped(self):   # every level 8 members spanning one Yaz0 of the next, padded to expand far: 8^6 scans without the budgets
        d = bytes(0x4000)
        for _ in range(6): d = fanout(yaz0(d), pad=0x4000)
        calls = []
        with self.assertRaisesRegex(SystemExit, 'more than 16 times the file'):
            with mock.patch.object(ss, 'scan_blob', wraps=ss.scan_blob) as m:
                try: ss.scan_blob(d, '/fanout', [])
                finally: calls.append(m.call_count)
        self.assertLess(calls[0], 200)

    def test_yaz0_expansion_is_charged_before_it_is_decoded(self):   # one member, no overlap, that expands about 87 times: stopped from its header, without decoding it
        d = sarc([(b'bomb.szs', yaz0(bytes(4 << 20)))])
        with mock.patch.object(ss, 'yaz0', wraps=ss.yaz0) as dec, self.assertRaisesRegex(SystemExit, 'Yaz0 archive members expand to more than 16 times the file'):
            ss.scan_blob(d, '/bomb.sarc', [])
        self.assertEqual(dec.call_count, 0)

    def test_member_names_and_time(self):   # each name ends at its own NUL (already so here): an 800-member archive reads in about 1 ms
        members = [(b'tex%04d.bntx' % i, bytes(0x4000)) for i in range(800)]
        archive = sarc(members)
        t = time.monotonic(); got = list(ss.sarc_members(archive)); took = time.monotonic() - t
        self.assertEqual(got, [(n.decode(), m) for n, m in members])
        self.assertLess(took, 1.0)


class CutData(unittest.TestCase):
    """Cut or out-of-range data red()s with the reader and the offset instead of an IndexError or struct.error naming nothing"""
    def test_yaz0_cut_short(self):
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] /x\.szs: the Yaz0 stream ends at'): ss.yaz0(yaz0(bytes(range(256)) * 4)[:0x40], '/x.szs')

    def test_yaz0_reference_before_the_start(self):
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] /x\.szs: a back-reference at 0x13 reaches'): ss.yaz0(b'Yaz0' + (16).to_bytes(4, 'big') + bytes(8) + b'\0\x10\x05', '/x.szs')

    def test_yaz0_round_trip(self):
        b = bytes(range(256)) + bytes(0x300) + b'abc' * 50
        self.assertEqual(ss.yaz0(yaz0(b)), b)

    def test_bnsh_pointers_past_the_end(self):
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] /x BNSH @0x0 variation 0 vs control section: 0x880 bytes at 0x300 lie outside'): ss.bnsh_shaders(bnsh()[:0x400], 0, '/x')

    def test_short_code_area_has_no_stage(self):
        self.assertIsNone(ss.sph_stage(ss.GFX_MAGIC + bytes(0x20)))

    def test_member_outside_the_data(self):
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] /x\.sarc: SARC member 0 at 0x0\.\.0x100000 of the data lies outside'): list(ss.sarc_members(outside(sarc([(b'a', b'1234')])), '/x.sarc'))

    def test_bahs_header_cut(self):
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] /x BAHS header: 0x8 bytes at 0x18'): ss.bahs_shaders(b'BAHS' + bytes(0x10), '/x')

    def test_sdf_entry_past_the_end(self):
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] /x SDF entry 0: 0x30 bytes at 0x20'): ss.sdf_shaders(b'SDF\0' + struct.pack('<HH', 1, 1) + bytes(8), '/x')


if __name__ == '__main__': unittest.main()
