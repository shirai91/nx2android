"""porter/toolchain.py: the prod.keys check. The file's presence is not enough: a title.keys file renamed as prod.keys
passes it, and the port then fails at the extract with an NCA header it cannot decrypt. Only the key names are read."""
import tempfile, unittest
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from porter.toolchain import keys_check


class Keys(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix='nx2a-keys-')
        self.addCleanup(self.dir.cleanup)

    def write(self, name, text):
        p = Path(self.dir.name) / name
        p.write_text(text)
        return p

    def test_a_keyset_with_the_master_keys_is_good(self):
        p = self.write('prod.keys', 'master_key_00 = 00000000000000000000000000000000\nmaster_key_01 = 11111111111111111111111111111111\n')
        ok, detail, _ = keys_check(str(p))
        self.assertTrue(ok, detail)
        self.assertIn('2 master keys', detail)

    def test_a_title_keys_file_is_not_a_keyset(self):
        p = self.write('prod.keys', '01000000000100000000000000000000 = 00000000000000000000000000000000\n')
        ok, detail, fix = keys_check(str(p))
        self.assertFalse(ok)
        self.assertIn('no master_key_00', detail)
        self.assertTrue(fix)

    def test_a_keyset_without_the_first_master_key_is_refused(self):
        p = self.write('prod.keys', 'master_key_01 = 11111111111111111111111111111111\n')
        ok, detail, _ = keys_check(str(p))
        self.assertFalse(ok)
        self.assertIn('master_key_00', detail)

    def test_a_comma_separated_keyset_is_good(self):
        # LibHac's reader takes ',' between the name and the value too
        p = self.write('prod.keys', 'master_key_00,00000000000000000000000000000000\nmaster_key_01,11111111111111111111111111111111\n')
        ok, detail, _ = keys_check(str(p))
        self.assertTrue(ok, detail)
        self.assertIn('2 master keys', detail)

    def test_the_names_are_read_in_any_case(self):
        # LibHac's reader lowercases the names, so a keyset that names them in upper case is the same keyset
        p = self.write('prod.keys', 'MASTER_KEY_00 = 00000000000000000000000000000000\n')
        ok, detail, _ = keys_check(str(p))
        self.assertTrue(ok, detail)
        self.assertIn('1 master key', detail)

    def test_a_keyset_with_only_the_derived_keys_is_good(self):
        # LibHac opens a keyset with derived keys only (header_key, key_area_key_application_*, titlekek_*): it has no master keys, and the header key is enough for what it holds
        p = self.write('prod.keys', 'header_key = 00000000000000000000000000000000\nkey_area_key_application_0 = 11111111111111111111111111111111\n')
        ok, detail, _ = keys_check(str(p))
        self.assertTrue(ok, detail)
        self.assertIn('0 master keys', detail)

    def test_a_missing_file_and_no_path_are_not_good_either(self):
        ok, detail, _ = keys_check('')
        self.assertFalse(ok)
        self.assertIn('not set', detail)
        ok, detail, _ = keys_check(str(Path(self.dir.name) / 'nothing.keys'))
        self.assertFalse(ok)
        self.assertIn('no such file', detail)

    def test_the_values_are_never_shown(self):
        p = self.write('prod.keys', 'master_key_00 = DEADBEEFDEADBEEFDEADBEEFDEADBEEF\n')
        ok, detail, _ = keys_check(str(p))
        self.assertTrue(ok, detail)
        self.assertNotIn('DEADBEEF', detail)


if __name__ == '__main__':
    unittest.main()
