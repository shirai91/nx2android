"""Android locale selection and the data pack's language settings."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

from porter import datapack


ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which('cc')


class AndroidGameLanguage(unittest.TestCase):
    def test_data_pack(self):
        profile = SimpleNamespace(name='Test Game', id='1', package='org.nx2a.test', env=[], settings={}, requires=[])
        with tempfile.TemporaryDirectory() as work:
            pack = Path(work) / 'data'
            for language, supported, expected in (
                ('system', ['en-US', 'zh-Hans'], ['DM_LANG=system', 'DM_LANG_SUPPORTED=en-US zh-Hans']),
                ('system', [], ['DM_LANG=system']),
                ('ja', ['en-US', 'ja'], ['DM_LANG=ja']),
            ):
                with self.subTest(language=language, supported=supported):
                    manifest = datapack.build(pack, profile=profile, version='v0', display='1.0', patched=None,
                                              romfs=None, shaders=None, language=language, languages=supported)
                    self.assertEqual(manifest['language'], language)
                    self.assertEqual((pack / 'env.txt').read_text(encoding='utf-8').splitlines()[1:], expected)

    @unittest.skipUnless(CC, 'a host C compiler is required')
    def test_locale_mapping(self):
        with tempfile.TemporaryDirectory() as work:
            source = Path(work) / 'locale.c'
            source.write_text('''
#include <assert.h>
#include "locale_android.h"
int main(void) {
    assert(strcmp(android_game_language("zh-CN", NULL), "zh-Hans") == 0);
    assert(strcmp(android_game_language("zh-Hant-TW", NULL), "zh-Hant") == 0);
    assert(strcmp(android_game_language("zh-HK", NULL), "zh-Hant") == 0);
    assert(strcmp(android_game_language("zh-Hans-HK", NULL), "zh-Hans") == 0);
    assert(strcmp(android_game_language("es-MX", "en-US es"), "es") == 0);
    assert(strcmp(android_game_language("fr-CA", "fr fr-CA"), "fr-CA") == 0);
    assert(strcmp(android_game_language("pt-PT", "en-US pt-BR"), "pt-BR") == 0);
    assert(strcmp(android_game_language("sv-SE", "en-US ja"), "en-US") == 0);
    assert(strcmp(android_game_language("sv-SE", "ko zh-Hans"), "ko") == 0);
    assert(strcmp(android_game_language("", "ja"), "ja") == 0);
    return 0;
}
''', encoding='utf-8')
            binary = Path(work) / 'locale'
            subprocess.run([CC, '-std=gnu11', '-I', str(ROOT / 'nxnative/dm/android'), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
