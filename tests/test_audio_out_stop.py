from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / 'nxnative/dm/host'
FIXTURE = ROOT / 'tests/audio_out_stop.c'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('cc'), 'requires the macOS AudioToolbox SDK')
class AudioOutStopTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory(prefix='nx2a-audio-stop-')
        cls.addClassCleanup(temp.cleanup)
        work = Path(temp.name)
        # The audio host uses no profile constants in this fixture.
        (work / 'game_profile.h').touch()
        cls.binary = work / 'audio_out_stop'
        cls.compile_command = [
            'cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-I', str(work), '-I', str(HOST), str(FIXTURE),
            '-framework', 'AudioToolbox',
        ]
        subprocess.run([*cls.compile_command, '-o', str(cls.binary)], check=True, timeout=60)

    def run_fixture(self, scenario, binary=None):
        result = subprocess.run([str(binary or self.binary), scenario], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_stop_returns_played_partial_and_pending_buffers_in_order(self):
        self.run_fixture('fifo')

    def test_stop_wakes_a_waiter_to_reclaim_buffers(self):
        self.run_fixture('event')

    def test_empty_started_output_wakes_guest_once_on_stop(self):
        self.run_fixture('empty')

    def test_unclaimed_releases_count_against_queue_capacity(self):
        self.run_fixture('capacity')

    def test_backend_changes_are_safe_for_the_silent_sink(self):
        binary = self.binary.with_name('audio_out_stop_tsan')
        subprocess.run([*self.compile_command, '-fsanitize=thread', '-g', '-o', str(binary)], check=True, timeout=60)
        self.run_fixture('backend', binary)
