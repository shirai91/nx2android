# run(): a failed tool's red lines are printed before the tail and carried in the error - the tail holds the lines
# behind the failure, and behind a pool's children dying they are all broken pipes. Only a line that starts with the tag is a red line: a traceback quotes the source line it failed on, and the
# compiler's diagnostics quote C lines that hold a red message. python -m unittest discover tests.
import io, sys, tempfile, unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from porter.upstream import BuildError, nsp   # noqa: E402


class RedLines(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix='nx2a_red_lines_')
        self.addCleanup(self.dir.cleanup)
        self.log = Path(self.dir.name) / 'tool.log'

    def run_tool(self, script):
        """Run the script as a tool that fails, with the progress quietened: returns what was printed and the error"""
        out = io.StringIO()
        with redirect_stdout(out), mock.patch.object(nsp, 'PROG') as prog:
            prog.end = lambda ok=True: None
            prog.say.side_effect = lambda msg: out.write(msg + '\n')
            with self.assertRaises(BuildError) as e:
                nsp.run([sys.executable, '-c', script], self.log)
        return out.getvalue(), str(e.exception)

    def test_a_failed_tool_reports_its_red_lines(self):
        text, error = self.run_tool("print('[RED] the file does not fit in this device'); print('BrokenPipeError'); raise SystemExit(3)")
        self.assertIn('red lines of', text)
        self.assertIn('[RED] the file does not fit in this device', text)
        self.assertIn('BrokenPipeError', text)              # the tail is still there, behind the red lines
        self.assertIn(text.index('red lines of'), range(text.index('BrokenPipeError')))   # the red lines come first
        self.assertIn('last red line: the file does not fit in this device', error)      # and in the error itself, without the tag (the porter prints '[RED] {e}')

    def test_a_tool_without_red_lines_keeps_the_old_message(self):
        text, error = self.run_tool("print('BrokenPipeError'); raise SystemExit(3)")
        self.assertNotIn('red lines of', text)
        self.assertNotIn('[RED]', error)

    def test_a_quoted_red_line_is_not_a_red_line(self):
        # a traceback prints the source line it failed on (a print(f'[RED] ...') in a tool), and the compiler's diagnostics quote C lines that hold a red message: those are not the reason
        script = '''print('  File "shader_vk.py", line 42'); print('    print(f"[RED] {msg}")'); raise SystemExit(1)'''
        text, error = self.run_tool(script)
        self.assertNotIn('red lines of', text)
        self.assertNotIn('[RED]', error)
        self.assertNotIn('last red line', error)

    def test_a_red_line_at_the_start_of_a_line_is_one(self):
        text, error = self.run_tool("print('[RED] the profile needs a row'); raise SystemExit(1)")
        self.assertIn('red lines of', text)
        self.assertIn('last red line: the profile needs a row', error)


if __name__ == '__main__':
    unittest.main()
