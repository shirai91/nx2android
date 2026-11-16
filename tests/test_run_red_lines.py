# run(): a failed tool's red lines are printed before the tail and carried in the error - the tail holds the lines
# behind the failure, and behind a pool's children dying they are all broken pipes. python -m unittest discover tests.
import io, sys, tempfile, unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from porter.upstream import BuildError, nsp   # noqa: E402


class RedLines(unittest.TestCase):

    def test_a_failed_tool_reports_its_red_lines(self):
        log = Path(tempfile.mkdtemp(prefix='nx2a_red_lines_')) / 'tool.log'
        out = io.StringIO()
        with redirect_stdout(out), mock.patch.object(nsp, 'PROG') as prog:
            prog.end = lambda ok=True: None
            prog.say.side_effect = lambda msg: out.write(msg + '\n')
            with self.assertRaises(BuildError) as e:
                nsp.run([sys.executable, '-c', "print('[RED] the file does not fit in this device'); print('BrokenPipeError'); raise SystemExit(3)"], log)
        text = out.getvalue()
        self.assertIn('red lines of', text)
        self.assertIn('[RED] the file does not fit in this device', text)
        self.assertIn('BrokenPipeError', text)              # the tail is still there, behind the red lines
        self.assertIn(text.index('red lines of'), range(text.index('BrokenPipeError')))   # the red lines come first
        self.assertIn('[RED] the file does not fit in this device', str(e.exception))     # and in the error itself

    def test_a_tool_without_red_lines_keeps_the_old_message(self):
        log = Path(tempfile.mkdtemp(prefix='nx2a_red_lines_')) / 'tool.log'
        out = io.StringIO()
        with redirect_stdout(out), mock.patch.object(nsp, 'PROG') as prog:
            prog.end = lambda ok=True: None
            prog.say.side_effect = lambda msg: out.write(msg + '\n')
            with self.assertRaises(BuildError) as e:
                nsp.run([sys.executable, '-c', "print('BrokenPipeError'); raise SystemExit(3)"], log)
        self.assertNotIn('red lines of', out.getvalue())
        self.assertNotIn('[RED]', str(e.exception))


if __name__ == '__main__':
    unittest.main()
