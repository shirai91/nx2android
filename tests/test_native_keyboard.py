"""Optional native text input packaging; the controller-only APK stays SDK-free."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from porter import cli, pipeline


class NativeKeyboard(unittest.TestCase):
    def test_opt_in_and_external_display_select_correct_activity(self):
        self.assertTrue(cli._parser().parse_args(['port','/game','--native-keyboard']).native_keyboard)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('platforms/android-35/android.jar','build-tools/35/d8'+('.bat' if os.name=='nt' else '')):
                p=root/name; p.parent.mkdir(parents=True); p.touch()
            for external in (False,True):
                port=pipeline.Port(pipeline.Options(folder=root,native_keyboard=True,external_display=external))
                port.aw=root/'out'; port.logs=root/'logs'; port.env={}
                with mock.patch.object(pipeline.toolchain,'_sdk_roots',return_value=[root]), mock.patch.object(pipeline.shutil,'which',return_value='javac'), mock.patch.object(pipeline.nsp,'run') as run:
                    args=port.external_activity()
                self.assertEqual(args[-1], 'org.nx2a.KeyboardExternalActivity' if external else 'org.nx2a.GameActivity')
                sources=[str(x) for x in run.call_args_list[0].args[0]]
                self.assertTrue(any(x.endswith('GameKeyboard.java') for x in sources))
                self.assertEqual(args[0],'--dex')
        self.assertFalse(pipeline.Options(folder=Path('/game')).native_keyboard)

    def test_requested_keyboard_requires_available_sdk(self):
        port=pipeline.Port(pipeline.Options(folder=Path('/game'),native_keyboard=True))
        with mock.patch.object(pipeline.toolchain,'_sdk_roots',return_value=[]), mock.patch.object(pipeline.shutil,'which',return_value=None):
            with self.assertRaisesRegex(pipeline.BuildError,'native-keyboard'): port.external_activity()

    def test_external_display_without_keyboard_keeps_native_activity(self):
        source = pipeline.DM / 'android/java/org/nx2a/ExternalActivity.java'
        self.assertIn('extends NativeActivity', source.read_text())
        self.assertNotIn('readKeyboard(', source.read_text())


class NativeKeyboardBridge(unittest.TestCase):
    def test_cached_lookup_input_reset_and_concurrent_cancel(self):
        import shutil
        import subprocess
        if not shutil.which('cc'):
            self.skipTest('a host C compiler is required')
        root = Path(__file__).resolve().parents[1]
        front = (pipeline.DM / 'android/front_android.c').read_text()
        start = front.index('int front_text_input(')
        dispatch = front[start:front.index('\n}\n', start) + 3]
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'keyboard_dispatch.inc').write_text(dispatch)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-I', str(tmp), '-I', str(pipeline.DM / 'android'), '-I', str(pipeline.DM / 'host'),
                            str(root / 'tests/keyboard_bridge_fixture.c'), '-o', str(tmp / 'bridge')], check=True, capture_output=True)
            result = subprocess.run([str(tmp / 'bridge')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
