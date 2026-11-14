# nxnative/dm/gfx/tools/uepak.py: the AES-256 ECB decryption of an encrypted Unreal pak index, on a machine that has no CommonCrypto
# (a Windows or Linux PC: the porter reads the pak where the game is. python -m unittest discover tests).
import sys, unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from porter.upstream import uepak   # noqa: E402

# NIST SP 800-38A, ECB-AES256: the key, one plaintext block and what decrypting that block gives (the ciphertext NIST
# lists for it is f3eed1bd…, which is the encryption of PLAIN; here the block is decrypted)
KEY = bytes.fromhex('603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4')
PLAIN = bytes.fromhex('6bc1bee22e409f96e93d7e117393172a')
CIPHER = '25cde6ca74d3539c375b66c3892cf70a'


class Aes(unittest.TestCase):
    def tearDown(self): uepak._cc = uepak._aes = None   # the provider is picked once per run: every test starts fresh

    def has_provider(self):   # CommonCrypto on a Mac, pycryptodome elsewhere
        try: uepak.ctypes.CDLL('/usr/lib/libSystem.B.dylib'); return True
        except OSError: pass
        try: import Crypto.Cipher.AES   # noqa: F401
        except ImportError:
            try: import Cryptodome.Cipher.AES   # noqa: F401
            except ImportError: return False
        return True

    def test_the_vector(self):   # the machine's own provider: CommonCrypto on a Mac, pycryptodome elsewhere
        if not self.has_provider(): self.skipTest('neither CommonCrypto nor pycryptodome is here')
        self.assertEqual(uepak.aes_decrypt(KEY, PLAIN).hex(), CIPHER)

    def test_without_commoncrypto(self):   # a PC has no libSystem: pycryptodome does the same work
        try: import Crypto.Cipher.AES   # noqa: F401
        except ImportError: self.skipTest('pycryptodome is not installed')
        with mock.patch.object(uepak.ctypes, 'CDLL', side_effect=OSError('no libSystem here')):
            self.assertEqual(uepak.aes_decrypt(KEY, PLAIN).hex(), CIPHER)

    def test_without_any_provider(self):   # neither of them: the error says what to install
        import builtins
        real_import = builtins.__import__
        def no_crypto(name, *a, **kw):
            if name.startswith('Crypto'): raise ImportError('no pycryptodome here')
            return real_import(name, *a, **kw)
        with mock.patch.object(uepak.ctypes, 'CDLL', side_effect=OSError('no libSystem here')), mock.patch.object(builtins, '__import__', no_crypto):
            with self.assertRaises(ValueError) as e: uepak.aes_decrypt(KEY, PLAIN)
        self.assertIn('pycryptodome', str(e.exception))
