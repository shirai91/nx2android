"""The online SDK opt-in: NX2A_ALLOW_ONLINE_SDK=1 (set by build_ipa_from_nsp.collect_nros for a profile with `online_sdk 1`) packs an NRO that exports EOS_Platform_Create; without it the NRO is refused."""
import os, sys, unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import nro_collect


class OnlineSdkOptIn(unittest.TestCase):
    def test_default_refuses(self):
        with mock.patch.dict(os.environ, {}, clear=False), mock.patch('nso_syms.dynsyms', create=True, return_value=[('EOS_Platform_Create', True, 0)]):
            os.environ.pop('NX2A_ALLOW_ONLINE_SDK', None)
            self.assertEqual(nro_collect.online_sdk(b'x'), 'Epic Online Services SDK')

    def test_opt_in_packs(self):
        with mock.patch.dict(os.environ, {'NX2A_ALLOW_ONLINE_SDK': '1'}), mock.patch('nso_syms.dynsyms', create=True, return_value=[('EOS_Platform_Create', True, 0)]):
            self.assertIsNone(nro_collect.online_sdk(b'x'))


if __name__ == '__main__': unittest.main()
