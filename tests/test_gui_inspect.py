"""Inspection diagnostics through real Qt processes; no game files or devices."""
import json, os, sys, tempfile, unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
try:
    from PySide6.QtCore import QEvent, QEventLoop, QObject, QProcess, QTimer
    from PySide6.QtGui import QTextDocument
    from PySide6.QtWidgets import QApplication
except ImportError:
    QApplication = None
if QApplication is not None:
    from gui import runner
    from gui.app import PortTab


@unittest.skipIf(QApplication is None, 'PySide6 is needed for the GUI tests')
class InspectionProcesses(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='nx2a_gui_test_')
        self.addCleanup(self.folder.cleanup)
        # Construction normally refreshes adb devices; inspection needs none.
        with patch.object(PortTab, 'refresh_devices'):
            self.tab = PortTab(None)
        self.addCleanup(self.tab.deleteLater)
        self.tab.folder.setText(self.folder.name)

    def run_child(self, script, *, inspect=True, failed=False, missing=False):
        """Run the actual callbacks and wait for their deferred process cleanup."""
        loop, owner = QEventLoop(), self.tab if inspect else QObject()
        replies, errors, destroyed, processes = [], [], [], []
        def child(parent, args):
            p = QProcess(parent)
            p.setProgram(str(Path(self.folder.name) / 'missing-executable') if missing else sys.executable)
            p.setArguments(['-c', script])
            p.destroyed.connect(lambda: destroyed.append(True))
            p.destroyed.connect(loop.quit)
            processes.append(p)
            return p
        timer = QTimer(); timer.setSingleShot(True); timer.timeout.connect(loop.quit)
        with patch.object(runner, '_porter', child):
            if inspect:
                self.tab.inspect()
            else:
                options = {'failed': lambda message, code: errors.append((message, code))} if failed else {}
                runner.call_json(owner, ['synthetic'], lambda obj, code: replies.append((obj, code)), **options)
            timer.start(3000); loop.exec(); timer.stop()
        cleaned_up = bool(destroyed)
        if not cleaned_up:
            for p in processes:
                if p.state() != QProcess.NotRunning: p.kill(); p.waitForFinished(1000)
                p.deleteLater()
            self.app.sendPostedEvents(None, QEvent.DeferredDelete)
        return replies, errors, cleaned_up

    def card_text(self):
        doc = QTextDocument(); doc.setHtml(self.tab.card.text())
        return doc.toPlainText()

    def test_stderr_failure_reaches_inspection_card_as_literal_text(self):
        diagnostic = 'ImportError: synthetic <dependency> & details\nsecond line'
        _, _, cleaned = self.run_child(f'import sys; sys.stderr.write({diagnostic!r}); sys.exit(7)')
        text = self.card_text()
        self.assertIn('ImportError: synthetic <dependency> & details', text)
        self.assertIn('second line', text)
        self.assertNotIn('No game found', text)
        self.assertFalse(self.tab.port_btn.isEnabled())
        self.assertIsNone(self.tab.info)
        self.assertTrue(cleaned)

    def test_invalid_or_empty_output_reports_process_failure(self):
        for script in ('print("not JSON")', 'pass', 'import sys; sys.exit(9)'):
            with self.subTest(script=script):
                _, _, cleaned = self.run_child(script)
                self.assertNotIn('No game found', self.card_text())
                self.assertIn('inspect', self.card_text().lower())
                self.assertTrue(cleaned)

    def test_failed_launch_finishes_inspection_and_cleans_up(self):
        _, _, cleaned = self.run_child('', missing=True)
        self.assertNotIn('Reading the game folder', self.card_text())
        self.assertIn('start', self.card_text().lower())
        self.assertIsNone(self.tab.info)
        self.assertTrue(cleaned)

    def test_structured_error_is_preserved_and_escaped(self):
        diagnostic = 'Cannot read <synthetic> & another file'
        result = json.dumps({'error': diagnostic})
        _, _, cleaned = self.run_child(f'import sys; print({result!r}); sys.stderr.write("ignored warning"); sys.exit(2)')
        self.assertEqual(self.card_text(), diagnostic)
        self.assertIsNone(self.tab.info)
        self.assertTrue(cleaned)

    def test_unsupported_result_with_exit_two_remains_inspectable(self):
        info = {'title_id': '0100000000AAA000', 'name': 'Synthetic title', 'android': None, 'profile': False,
                'supported': False, 'experimental': True, 'updates': [], 'dlc': [], 'reason': 'No Android profile'}
        result = json.dumps(info)
        _, _, cleaned = self.run_child(f'import sys; print({result!r}); sys.stderr.write("ignored warning"); sys.exit(2)')
        self.assertEqual(self.tab.info, info)
        self.assertIn('Synthetic title', self.card_text())
        self.assertFalse(self.tab.port_btn.isEnabled())
        self.tab.experimental.setChecked(True)
        self.assertTrue(self.tab.port_btn.isEnabled())
        self.assertTrue(cleaned)

    def test_list_callers_keep_list_and_none_results(self):
        for script, want in [('print("[]")', ([], 0)), ('import sys; sys.stderr.write("failure"); sys.exit(3)', (None, 3))]:
            with self.subTest(script=script):
                replies, errors, cleaned = self.run_child(script, inspect=False)
                self.assertEqual(replies, [want])
                self.assertEqual(errors, [])
                self.assertTrue(cleaned)

    def test_optional_failure_callback_does_not_replace_valid_json(self):
        for result in ('[]', '{"error": "structured failure"}', '{"supported": false}'):
            with self.subTest(result=result):
                replies, errors, cleaned = self.run_child(f'import sys; print({result!r}); sys.exit(2)', inspect=False, failed=True)
                self.assertEqual(replies, [(json.loads(result), 2)])
                self.assertEqual(errors, [])
                self.assertTrue(cleaned)

    def test_failed_launch_calls_legacy_callback_once(self):
        replies, errors, cleaned = self.run_child('', inspect=False, missing=True)
        self.assertEqual(len(replies), 1)
        self.assertIsNone(replies[0][0])
        self.assertNotEqual(replies[0][1], 0)
        self.assertEqual(errors, [])
        self.assertTrue(cleaned)

    @unittest.skipIf(os.name == 'nt', 'POSIX signal exit')
    def test_crashed_process_calls_failure_callback_once(self):
        script = 'import os, signal, sys; sys.stderr.write("synthetic crash\\n"); sys.stderr.flush(); os.kill(os.getpid(), signal.SIGKILL)'
        replies, errors, cleaned = self.run_child(script, inspect=False, failed=True)
        self.assertEqual(replies, [])
        self.assertEqual(len(errors), 1)
        self.assertIn('synthetic crash', errors[0][0])
        self.assertNotEqual(errors[0][1], 0)
        self.assertTrue(cleaned)


if __name__ == '__main__': unittest.main()
