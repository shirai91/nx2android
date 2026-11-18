# language_code(): a language as typed → the Switch language code it names. A phone's locale tag (ja-JP, zh-Hans-CN) resolves by
# dropping what the game does not carry: the region first, then the script. python -m unittest discover tests.
import sys, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from porter.upstream import nsp   # noqa: E402


class LanguageCode(unittest.TestCase):

    def test_a_code_is_itself(self):
        for c in ('ja', 'ko', 'de', 'ru', 'it', 'nl', 'zh-Hans', 'zh-Hant', 'en-US', 'en-GB', 'fr', 'fr-CA', 'es', 'es-419', 'pt', 'pt-BR'):
            self.assertEqual(nsp.language_code(c), c)

    def test_the_case_and_the_underscore_do_not_matter(self):
        self.assertEqual(nsp.language_code('ZH_hans'), 'zh-Hans')
        self.assertEqual(nsp.language_code('PT-br'), 'pt-BR')

    def test_a_name_is_its_code(self):
        self.assertEqual(nsp.language_code('Japanese'), 'ja')
        self.assertEqual(nsp.language_code('Korean'), 'ko')
        self.assertEqual(nsp.language_code('Simplified Chinese'), 'zh-Hans')

    def test_the_settings_older_codes(self):
        self.assertEqual(nsp.language_code('en'), 'en-US')
        self.assertEqual(nsp.language_code('English'), 'en-US')
        self.assertEqual(nsp.language_code('zh-cn'), 'zh-Hans')
        self.assertEqual(nsp.language_code('zh-tw'), 'zh-Hant')

    def test_a_tag_with_a_region_drops_it(self):
        # a phone's locale (Locale.getDefault().toLanguageTag()): most of the world writes a region in it
        self.assertEqual(nsp.language_code('ja-JP'), 'ja')
        self.assertEqual(nsp.language_code('ko-KR'), 'ko')
        self.assertEqual(nsp.language_code('de-DE'), 'de')
        self.assertEqual(nsp.language_code('ru-RU'), 'ru')
        self.assertEqual(nsp.language_code('it-IT'), 'it')
        self.assertEqual(nsp.language_code('nl-NL'), 'nl')
        self.assertEqual(nsp.language_code('fr-FR'), 'fr')
        self.assertEqual(nsp.language_code('pt-PT'), 'pt')

    def test_a_tag_with_a_script_and_a_region_drops_the_region(self):
        # the newer Androids write the script in their locale (zh-Hans-CN)
        self.assertEqual(nsp.language_code('zh-Hans-CN'), 'zh-Hans')
        self.assertEqual(nsp.language_code('zh-Hant-TW'), 'zh-Hant')
        self.assertEqual(nsp.language_code('zh-Hans-SG'), 'zh-Hans')

    def test_the_near_language_can_then_help(self):
        # es-ES is no code the game carries, but 'es' is the Spanish of the other regions (LANG_NEAR)
        self.assertEqual(nsp.game_language(nsp.language_code('es-ES'), False, ['es-419']), 'es-419')

    def test_a_language_that_is_not_one_is_none(self):
        self.assertIsNone(nsp.language_code('klingon'))
        self.assertIsNone(nsp.language_code(''))


if __name__ == '__main__':
    unittest.main()
