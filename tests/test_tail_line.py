# tail_line(): the last complete line of a file, read from its end - the rewrite stage uses it to follow each
# module's own progress lines and pass them on. python -m unittest discover tests.
import sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from porter.upstream import nsp   # noqa: E402


class TailLine(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix='nx2a_tail_line_')
        self.addCleanup(self.dir.cleanup)

    def write(self, name, text):
        p = Path(self.dir.name) / name
        p.write_bytes(text if isinstance(text, bytes) else text.encode())
        return p

    def test_the_last_complete_line(self):
        p = self.write('a.log', 'first\nsecond\nprogress: 12.3 MB of 71.2 MB (45/298 pieces)\n')
        self.assertEqual(nsp.tail_line(str(p)), 'progress: 12.3 MB of 71.2 MB (45/298 pieces)')

    def test_a_line_still_being_written_is_not_a_line_yet(self):
        p = self.write('b.log', 'done\nprogress: 12.3 MB of')
        self.assertEqual(nsp.tail_line(str(p)), 'done')

    def test_an_empty_file_has_no_line(self):
        self.assertEqual(nsp.tail_line(str(self.write('c.log', b''))), '')

    def test_a_missing_file_has_no_line(self):
        self.assertEqual(nsp.tail_line(str(Path(self.dir.name) / 'nope.log')), '')


if __name__ == '__main__':
    unittest.main()
