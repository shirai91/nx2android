"""Optional native text input packaging; the controller-only APK stays SDK-free."""
import os
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import host_cc
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
                with mock.patch.object(pipeline.toolchain,'_sdk_roots',return_value=[root]), mock.patch.object(pipeline.shutil,'which',return_value='javac'),                      mock.patch.object(pipeline,'_jdk_env',side_effect=lambda javac,env:dict(env or {})), mock.patch.object(pipeline,'_run_java') as run:
                    args=port.java_bridge()
                self.assertEqual(args[-1], 'org.nx2a.KeyboardExternalActivity' if external else 'org.nx2a.KeyboardGameActivity')
                sources=[str(x) for x in run.call_args_list[0].args[0]]
                self.assertTrue(any(x.endswith('GameKeyboard.java') for x in sources))
                self.assertEqual(args[0],'--dex')
        self.assertFalse(pipeline.Options(folder=Path('/game')).native_keyboard)

    def test_requested_keyboard_requires_available_sdk(self):
        port=pipeline.Port(pipeline.Options(folder=Path('/game'),native_keyboard=True))
        with mock.patch.object(pipeline.toolchain,'_sdk_roots',return_value=[]), mock.patch.object(pipeline.shutil,'which',return_value=None),              mock.patch.object(pipeline,'PREBUILT_DEX',Path('/no/prebuilt/classes.dex')):
            with self.assertRaisesRegex(pipeline.BuildError,'native-keyboard'): port.java_bridge()

    def test_external_display_without_keyboard_keeps_native_activity(self):
        source = pipeline.DM / 'android/java/org/nx2a/ExternalActivity.java'
        self.assertIn('extends NativeActivity', source.read_text(encoding='utf-8'))
        self.assertNotIn('readKeyboard(', source.read_text(encoding='utf-8'))


class NativeKeyboardBridge(unittest.TestCase):
    @unittest.skipUnless(host_cc.posix(), 'a POSIX host C compiler is required (<sys/mman.h>, pthread, setenv)')
    def test_keyboard_jni_runs_off_guest_caller(self):
        root = Path(__file__).resolve().parents[1]
        front = (pipeline.DM / 'android/front_android.c').read_text(encoding='utf-8')
        start = front.index('int front_text_input(')
        dispatch = front[start:front.index('\n}\n', start) + 3]
        touch_state = next(line for line in front.splitlines() if ' g_touch_btn;' in line)
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'keyboard_dispatch.inc').write_text(dispatch)
            (tmp / 'keyboard_touch_state.inc').write_text(touch_state + '\n')
            build = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                                    '-I', str(tmp), '-I', str(pipeline.DM / 'android'), '-I', str(pipeline.DM / 'host'),
                                    str(root / 'tests/keyboard_bridge_fixture.c'), '-o', str(tmp / 'bridge')],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(tmp / 'bridge')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
