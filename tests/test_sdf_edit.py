# Edits inside Snowdrop packed data (porter/sdf_edit.py): a BVM asset read and written back byte for byte, a list entry changed, and the changed zlib
# stream made again to exactly its old length so nothing else in the slice moves. No game files.
import hashlib, os, struct, sys, tempfile, unittest, zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from porter import sdf_edit   # noqa: E402


def bvm():
    strs = [b'name', b'data', b'twk_Menu', b'Play', b'DLC', b'Quit']
    lst = ('VarMap', ([('String', 3), ('String', 4), ('String', 5)], []))
    root = ('VarMap', ([], [(('String', 0), ('String', 2)), (('String', 1), lst), (('String', 3), ('Int32', b'\x72\x00'))]))   # a signed int not in its shortest form
    return sdf_edit.bvm_dump(strs, root)


def stream(data):
    # stored: room for the re-made stream, as a 64 KB game stream has
    c = zlib.compressobj(0, zlib.DEFLATED, -10); return b'\x28\x15' + c.compress(data) + c.flush() + struct.pack('>I', zlib.adler32(data))


class SdfEdit(unittest.TestCase):
    def test_bvm_round_trip(self):
        b = bvm(); self.assertEqual(sdf_edit.bvm_dump(*sdf_edit.bvm_load(b)), b)

    def test_list_edit(self):
        new = sdf_edit.bvm_set_list(bvm(), 'twk_Menu', [1], 'Quit', ['Play', 'DLC', 'Quit'])
        strs, root = sdf_edit.bvm_load(new); lst = dict(root[1][1])[('String', 1)][1][0]
        self.assertEqual([strs[x[1]] for x in lst], [b'Play', b'Quit', b'Quit'])
        with self.assertRaises(sdf_edit.EditError): sdf_edit.bvm_set_list(bvm(), 'twk_Menu', [1], 'Quit', ['Play', 'Advert', 'Quit'])

    def test_slice_patch_keeps_everything_else(self):
        asset = bvm() + bytes(40)   # (padding stands in for a bigger asset; only the edit's bytes change)
        head, tail = os.urandom(300), os.urandom(200)
        d = head + stream(asset) + tail
        new = sdf_edit.bvm_set_list(bvm(), 'twk_Menu', [1], 'Quit', ['Play', 'DLC', 'Quit']) + bytes(40)
        out, n = sdf_edit.replace_asset(d, len(head), new)
        self.assertEqual((len(out), n), (len(d), 1))
        self.assertEqual(out[:len(head)], head); self.assertEqual(out[-len(tail):], tail)
        self.assertEqual(b''.join(x for _, _, x in sdf_edit.streams(out, len(head))), new)

    def test_table(self):
        d = Path(tempfile.mkdtemp(prefix='nx2a_test_sdf_')); asset = bvm()
        (d / 'rom/a').mkdir(parents=True); (d / 'rom/a/s.sdfdata').write_bytes(b'H' * 10 + stream(asset) + b'T' * 10)
        (d / 't.tsv').write_text('# x\n' + '\t'.join(['a/s.sdfdata', '10', hashlib.md5(asset).hexdigest(), 'bvm-list', 'twk_Menu', '1', 'Quit', 'Play,DLC,Quit']) + '\n')
        self.assertEqual(sdf_edit.patch(d / 'rom', d / 't.tsv', d / 'out'), 1)
        self.assertNotEqual((d / 'out/a/s.sdfdata').read_bytes(), (d / 'rom/a/s.sdfdata').read_bytes())

    def test_table_path_stays_inside(self):   # a row cannot write outside romfs_patch
        d = Path(tempfile.mkdtemp(prefix='nx2a_test_sdf_'))
        for f in ('../s.sdfdata', '/tmp/s.sdfdata', 'a/../../s.sdfdata', 'C:/s.sdfdata'):
            (d / 't.tsv').write_text('\t'.join([f, '10', '0' * 32, 'bvm-list', 'twk_Menu', '1', 'Quit', 'Play,DLC,Quit']) + '\n')
            with self.assertRaises(sdf_edit.EditError): sdf_edit.patch(d / 'rom', d / 't.tsv', d / 'out')


if __name__ == '__main__': unittest.main()
