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
                self.assertEqual(args[-1], 'org.nx2a.ExternalActivity' if external else 'org.nx2a.GameActivity')
                sources=[str(x) for x in run.call_args_list[0].args[0]]
                self.assertTrue(any(x.endswith('GameKeyboard.java') for x in sources))
                self.assertEqual(args[0],'--dex')
        self.assertFalse(pipeline.Options(folder=Path('/game')).native_keyboard)

    def test_requested_keyboard_requires_available_sdk(self):
        port=pipeline.Port(pipeline.Options(folder=Path('/game'),native_keyboard=True))
        with mock.patch.object(pipeline.toolchain,'_sdk_roots',return_value=[]), mock.patch.object(pipeline.shutil,'which',return_value=None):
            with self.assertRaisesRegex(pipeline.BuildError,'native-keyboard'): port.external_activity()
